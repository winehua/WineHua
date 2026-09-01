import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import {
  WineEnvService, WINE_ROOT, FILES_BASE, pathExists
} from '../service/WineEnvService';
import { SmokeRunner } from './SmokeRunner';
import { SmokeRequest } from './SmokeTypes';

const DOMAIN = 0x0000;
const TAG = 'SMOKE';

// master 侧 smoke 自动化枢纽: 全部 smoke 状态与编排收敛在 ets/smoke/ 目录,
// 公共产品文件只留带 // [[SMOKE]] 标记的一行钩子 (WineEnvService.attach /
// WineEnvService.onNewWant / WineEnvService.enterReady 内 onEngineReady /
// EntryAbility.applyWant / Index.<SmokeDevPanel/>)。main-ui 合并摘除动作:
// 删 ets/smoke/ 整目录 + 删各标记行, 见 docs/SMOKE_REBUILD_20260831.md §11。
//
// 触发语义 (与重构前的 consumeSmokeRequest/maybeRunSmoke 逐条对齐):
//   - 冷启动: init 的 attach 只消费请求 (clean 编排接管则返回 true), 跑测
//     等 enterReady 链尾的 onEngineReady (引擎 ready 且 prefix 真正就绪 —
//     回放式 ready 不可信: broker 未就绪时 runWineProgram 返回 -1, 实测)。
//   - 二次 Want (App 存活): onNewWant 消费后立即跑 (引擎必然已就绪)。
export class SmokeHook {
  private static instance: SmokeHook | null = null;

  static getInstance(): SmokeHook {
    if (!SmokeHook.instance) {
      SmokeHook.instance = new SmokeHook();
    }
    return SmokeHook.instance;
  }

  private env: WineEnvService | null = null;
  // 已消费待执行的 smoke 请求 (消费后置空 = 已启动, 不跨会话重触发;
  // App 存活期间二次 want 会重新置入, SmokeRunner.busy 负责防重入)
  private pending: SmokeRequest | null = null;
  // clean 重置编排已启动 (幂等 guard): 编排中 prefixReady=false, 完成边沿由
  // 订阅回调拾起进入跑测分支
  private resetStarted: boolean = false;
  private seedDone: boolean = false;

  // EntryAbility.publishLaunchRequest 调用: want 里 mode=smoke 时发布 5 键请求
  // 到 AppStorage (消费清键, 不重触发)。正常会话 (无 mode) 零行为变化。
  applyWant(parameters: Record<string, Object> | undefined): void {
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    if (value('winehua.mode', '') !== 'smoke') return;
    AppStorage.setOrCreate<string>('winehua.smoke.request', JSON.stringify({
      suite: value('winehua.suite', 'core'),
      runId: value('winehua.run_id', `manual-${Date.now()}`),
      prefix: value('winehua.prefix', 'reuse') === 'clean' ? 'clean' : 'reuse',
      longSeconds: Number(value('winehua.long_seconds', '3600')),
      source: 'want'
    }));
    hilog.info(DOMAIN, TAG, 'want request: suite=%{public}s runId=%{public}s prefix=%{public}s',
      value('winehua.suite', 'core'), value('winehua.run_id', ''),
      value('winehua.prefix', 'reuse'));
  }

  // WineEnvService.init 调用 (native 状态回调与订阅已注册后): 消费冷启动请求;
  // clean 编排启动成功返回 true = init 提前返回 (重置编排接管)。跑测不在此做 —
  // 冷启动等 onEngineReady (引擎真 ready), 由 enterReady 链尾通知。
  // 注意: 不订阅 env 状态镜像 — clean 编排启动瞬间引擎仍短暂 ready
  // (stopSession 异步), 任何 notify 兜底触发都会把 clean 请求当 reuse 提前跑
  // (r6 实测误触发)。跑测只走三个显式入口: attach 之 tick (仅 clean 编排) /
  // onNewWant (引擎必然已就绪) / onEngineReady (waitForPrefix 完成链)。
  attach(env: WineEnvService): boolean {
    this.env = env;
    this.consume();
    return this.tick();
  }

  // WineEnvService.onNewWant 调用 (App 存活时二次 Want): 引擎必然已就绪,
  // 消费后直接编排/跑测, 不轮询 (无常驻 timer)。
  onNewWant(): void {
    this.consume();
    this.tick();
  }

  // WineEnvService.enterReady 链尾调用: 引擎 ready 且 waitForPrefix 完成 —
  // 播种 + 跑测的入口 (冷启动/clean 复位/核心重启共用)。
  onEngineReady(): void {
    this.runPending();
  }

  // 编排单点 (幂等): clean 优先 (启动重置编排) → 跑测条件满足就跑 → 否则等待。
  // 返回 true = 本次调用启动了重置编排 (init 据此提前 return)。
  private tick(): boolean {
    const req = this.pending;
    const env = this.env;
    if (!req || !env) return false;
    if (req.prefix === 'clean' && !this.resetStarted) {
      if (env.isBusy()) {
        return false; // busy: 等下一 tick (编排完成后 ready 边沿触发)
      }
      this.resetStarted = true;
      if (env.cleanPrefixAndRestart()) {
        return true;
      }
      this.resetStarted = false;
      return false;
    }
    this.runPending();
    return false;
  }

  // 跑测单点: 引擎 ready + prefix 就绪 + 有 pending 才启动 (pending 置空即
  // 防重; SmokeRunner.busy 挡运行中的重复请求)。二次 Want 走 onNewWant→tick;
  // 冷启动走 onEngineReady; onEnvChange 兜底 (ready 边沿竞态)。
  private runPending(): void {
    const req = this.pending;
    const env = this.env;
    if (!req || !env) return;
    if (!env.prefixReady || env.engineState !== 'ready') return;
    this.pending = null;
    this.seed();
    SmokeRunner.getInstance().start(req);
  }

  // 请求消费单点: AppStorage 键 (applyWant 写入) → pending; 立即清键防重触发。
  private consume(): void {
    const raw = AppStorage.get<string>('winehua.smoke.request');
    if (!raw) return;
    AppStorage.setOrCreate<string>('winehua.smoke.request', '');
    try {
      this.pending = JSON.parse(raw) as SmokeRequest;
      hilog.info(DOMAIN, TAG, 'request consumed: suite=%{public}s runId=%{public}s prefix=%{public}s',
        this.pending.suite, this.pending.runId, this.pending.prefix || 'reuse');
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'request parse error: %{public}s', JSON.stringify(e));
      this.pending = null;
    }
  }

  // C:\smoke 载荷播种 (suites.json + exe, assemble.sh 打包随 wine 数据):
  // 引擎就绪后执行一次, 失败只记日志不阻塞 (与旧 seedSmokePayload 同语义)。
  private seed(): void {
    if (this.seedDone) return;
    this.seedDone = true;
    const source = `${WINE_ROOT}/smoke`;
    const target = `${FILES_BASE}/.wine/drive_c/smoke`;
    if (pathExists(target) || !pathExists(source)) return;
    try {
      this.copyTree(source, target);
      hilog.warn(DOMAIN, TAG, 'seeded smoke payload → C:\\smoke');
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'seed smoke payload failed: %{public}s', JSON.stringify(e));
    }
  }

  // 目录递归复制 (与 WineEnvService.copyTree 同款原语; 不用 fs.copy — 沙箱内
  // 实测失败: 裸路径 401, file:// URI 13900020)。
  private copyTree(src: string, dst: string): void {
    const st = fs.statSync(src);
    if (st.isDirectory()) {
      fs.mkdirSync(dst, true);
      for (const name of fs.listFileSync(src)) {
        this.copyTree(`${src}/${name}`, `${dst}/${name}`);
      }
      return;
    }
    fs.copyFileSync(src, dst);
  }
}

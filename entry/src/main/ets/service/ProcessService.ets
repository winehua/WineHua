import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineEnvService } from './WineEnvService';
import { WineWindowManager } from './WineWindowManager';

const DOMAIN = 0x0000;

export interface ProcessInfo {
  pid: number;
  name: string;
  path: string;
  state: string;
  // 桌面 root 出现前加入的会话基础进程 (desktop + 桌面前的 explorer 等),
  // native 在桌面首次出现时打标, 结束会破坏桌面运行, 隐藏"结束"操作。
  desktopShell: boolean;
}

// wine 内部系统进程 (broker 已全量登记): 运行中页只显示用户程序,
// 隐藏 wineserver/services 等服务进程。AddProcess 登记的是 basename。
// winehua_keep 是桌面保活驻留 (桌面销毁后仍 Sleep 永驻), 属基础设施;
// 不过滤它进程列表永不为空, "全部退出" 判定永远不成立。
// wineboot (无 .exe) 是 broker 登记 NCP 壳的名字 (first boot 的 wineboot
// 经 broker 通道 spawn, 全量登记后带此名) — 与 wineboot.exe 同一功能,
// 同样过滤; 否则首启期间它会以绿点出现在运行中列表 (P1 残留观感)
const SYSTEM_PROC_BLACKLIST: string[] = [
  'wineserver', 'services.exe', 'csrss.exe', 'winedevice.exe',
  'plugplay.exe', 'svchost.exe', 'rpcss.exe', 'wineboot',
  'wineboot.exe', 'winehua_keep.exe',
];

// Wine 进程列表服务: 原 Index.ets 的 refreshProcessList/doKillProcess 原样迁入。
// 页面经 subscribe 镜像 processList; bindEnv 后随 WineEnvService 状态自动刷新。
export class ProcessService {
  private static instance: ProcessService | null = null;

  static getInstance(): ProcessService {
    if (!ProcessService.instance) {
      ProcessService.instance = new ProcessService();
    }
    return ProcessService.instance;
  }

  processList: ProcessInfo[] = [];
  actionMessage: string = '';

  private listeners: Array<() => void> = [];
  private envBound: boolean = false;
  private pendingKillPids: Set<number> = new Set<number>();
  // 刷新节流: wine 内部进程高频进出时会触发大量 proc-updated 事件,
  // 合并到 300ms 内一次刷新, 避免 UI 反复重建列表 (master 同款策略)
  private refreshPending: boolean = false;
  private refreshTimer: number = -1;
  // D45: proc-exited 待检测标记 (节流刷新完成点消费, 见 bindEnv 注释)
  private desktopGoneCheckPending: boolean = false;
  // D45 判据门: 本会话内见过 desktopShell 打标 (桌面 root 确实出现过)。
  // 引擎 stopped 时重置 (clear() 由 WineEnvService 停止编排调用)。
  private desktopShellSeen: boolean = false;

  // 极简订阅: 状态变化后逐个调用
  subscribe(listener: () => void): void {
    if (this.listeners.indexOf(listener) >= 0) return;
    this.listeners.push(listener);
  }

  unsubscribe(listener: () => void): void {
    const index = this.listeners.indexOf(listener);
    if (index >= 0) this.listeners.splice(index, 1);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 原 refreshProcessList (含 master 的系统进程黑名单过滤)
  refresh = (): void => {
    try {
      const all = testNapi.getProcessList() as ProcessInfo[];
      this.processList = all.filter((p: ProcessInfo) => SYSTEM_PROC_BLACKLIST.indexOf(p.name) < 0);
      // D45 判据门: desktopShell 打标发生在桌面 root 首现时刻 (native
      // MarkDesktopShellProcesses) —— ready 后 root 出现前的窗口期里集合
      // 必然为空, 那不是"桌面退出"而是"桌面还没到"。见过打标才允许后续
      // 的全灭判定 (checkDesktopShellGone), 见 desktopGoneCheckPending。
      if (!this.desktopShellSeen && this.processList.some((p: ProcessInfo) => p.desktopShell)) {
        this.desktopShellSeen = true;
      }
    } catch (e) {
      // Keep the last successful snapshot. A transient NAPI failure must not
      // make every running program disappear from the task manager.
      hilog.error(DOMAIN, 'wine', 'getProcessList error: %{public}s', JSON.stringify(e));
    }
    this.notify();
  }

  // 桌面 shell 基础进程不可由用户结束: desktopShell 由 native 在桌面 root 首次
  // 出现时对"当时 running 的进程"统一打标 (desktop + 桌面出现前加入的多个
  // explorer.exe), 基于时间边界而非进程名 — 桌面出现后手动启动的 explorer
  // 文件管理器不受影响, 仍可正常结束。结束任一基础进程都会破坏桌面运行
  // (会话错乱/热重启污染), 运行中页据此隐藏结束操作, 防用户误操作。
  isKillable(proc: ProcessInfo): boolean {
    return !proc.desktopShell;
  }

  isKillPending(pid: number): boolean {
    return this.pendingKillPids.has(pid);
  }

  kill = (pid: number, name: string = ''): boolean => {
    if (pid <= 0 || this.pendingKillPids.has(pid)) return false;
    try {
      const accepted = testNapi.killProcess(pid) as boolean;
      if (!accepted) {
        this.actionMessage = `结束 ${name || pid} 失败：进程已退出或不可结束`;
        this.refresh();
        return false;
      }
      this.pendingKillPids.add(pid);
      this.actionMessage = `正在结束 ${name || pid}…`;
      this.refresh();
      this.pollKillResult(pid, name, 0);
      return true;
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'killProcess error: %{public}s', JSON.stringify(e));
      this.actionMessage = `结束 ${name || pid} 失败`;
      this.notify();
      return false;
    }
  }

  private pollKillResult(pid: number, name: string, attempt: number): void {
    setTimeout(() => {
      this.refresh();
      const alive = this.processList.some((p: ProcessInfo) => p.pid == pid);
      if (!alive) {
        this.pendingKillPids.delete(pid);
        this.actionMessage = `${name || '进程'} 已结束`;
        this.notify();
        return;
      }
      if (attempt >= 9) {
        this.pendingKillPids.delete(pid);
        this.actionMessage = `${name || '进程'} 仍在退出；可稍后刷新`;
        this.notify();
        return;
      }
      this.pollKillResult(pid, name, attempt + 1);
    }, 350);
  }

  // 清空列表 (WineEnvService.doReset 用, 原 this.processList = [])
  clear(): void {
    this.processList = [];
    this.resetDesktopShellSeen();
    this.pendingKillPids.clear();
    this.actionMessage = '';
    this.notify();
  }

  // D45: 会话结束 (stopAll→stopped) 时重置打标历史, 新会话 init 窗口期
  // 不得判全灭 (见 checkDesktopShellGone 的 desktopShellSeen 门)
  resetDesktopShellSeen(): void {
    this.desktopShellSeen = false;
    this.desktopGoneCheckPending = false;
  }

  // 节流刷新: 引擎事件驱动 (proc-updated 高频), 手动 ↻ 走 refresh 直刷
  private refreshThrottled = (): void => {
    if (this.refreshPending) {
      return;
    }
    this.refreshPending = true;
    this.refreshTimer = setTimeout(() => {
      this.refreshPending = false;
      this.refreshTimer = -1;
      this.refresh();
      // D45: 桌面 shell 全灭检测挂在刷新完成点之后 (见 bindEnv 注释)
      if (this.desktopGoneCheckPending) {
        this.desktopGoneCheckPending = false;
        this.checkDesktopShellGone();
      }
    }, 300);
  }

  // 订阅引擎事件: 进程相关事件 (含 broker 登记的 proc-updated) 到达时节流刷新;
  // 复刻 master refreshProcessList 的触发集 + 300ms 合并。
  // D45: proc-exited 只置 pending 标记, 检测延到节流刷新完成点 —— 不得在
  // evt 分发同步路径上加 native 调用 (2026-10-10 实测教训: 同步 refresh()
  // 改变 init 期 JS 线程时序, 把既有的 wine 子进程 SIGSEGV 竞态触发概率
  // 放大到启动必崩 (D47, 7+ 连崩); revert 恢复原时序后即正常。时序敏感,
  // 机制未明, 见 D47)。
  private env: WineEnvService | null = null;

  bindEnv(env: WineEnvService): void {
    if (this.envBound) {
      return;
    }
    this.envBound = true;
    this.env = env;
    env.subscribeEvents((evt: string) => {
      if (evt.startsWith('proc-') || evt.startsWith('launch-')) {
        this.refreshThrottled();
        if (evt.startsWith('proc-exited')) {
          this.desktopGoneCheckPending = true;
        }
      }
      // D45 seen 的权威置位点: evt:desktop-ready 与打标 (MarkDesktopShellProcesses)
      // 在 native 同一分支发出 (wayland_server.cpp DesktopRoot), = 本 app 进程
      // 亲历了桌面 root 出现。refresh 的被动观察仅作兜底 —— 接管残留会话时
      // 注册表带旧打标但本进程没见过 root, 不得置 seen (02:06 误关窗教训:
      // deploy 后 wineserver 残留, 旧 explorer 退出被新 app 误判为桌面退出)。
      if (evt === 'desktop-ready') {
        this.desktopShellSeen = true;
      }
      // 游戏全屏适配 (2026-10-15): 桌面内容矩形变化 (native FrameStep 变化
      // 检测上报, "evt:desktop-content-resize:{w}x{h}", w/h = 桌面逻辑尺寸)
      // → PC/2in1 承载窗跟随 (WineWindowManager.applyDesktopContentSize;
      // Pad 侧该方法是空操作, 缩放由 native letterbox 承担)。纯 ArkTS 转发,
      // 不在本回调做同步 native 调用 (D47 时序纪律)。
      if (evt.startsWith('desktop-content-resize:')) {
        const dim = evt.substring('desktop-content-resize:'.length);
        const parts = dim.split('x');
        const w = parseInt(parts[0], 10);
        const h = parseInt(parts[1], 10);
        if (w > 0 && h > 0) {
          WineWindowManager.getInstance().applyDesktopContentSize(w, h);
        }
      }
    });
    this.refresh();
  }

  // D45: x11 路线桌面 shell 全灭 = 桌面会话结束 → 关画布窗回主页。wayland
  // 有 toplevel destroyed 事件链 (WineWindowManager.onToplevelDestroyed),
  // x11 桥无此事件。判据走进程面: desktopShell 是 native 在桌面 root 首现
  // 时对当时 running 进程的打标 (ProcessInfo 字段注释); 过滤黑名单 (基础设施
  // 进程不入列) 后集合空 = desktop + explorer 全退。teardown 的连续
  // proc-exited 合并进同一次节流刷新, 幂等 (closeX11Canvas 内部短路)。
  // desktopShellSeen 门: 唯一权威置位点 = evt:desktop-ready (与 native 打标
  // 同分支发出, = 本 app 进程亲历桌面 root 出现)。两轮教训:
  //   ① init 窗口期 (root 未现) 集合恒空, 无门则 proc-exited 一到就误判
  //     (01:0x 三连崩, onTerminateScene callerContainerSession EntryAbility);
  //   ② seen 依赖 refresh 被动观察会漏掉静默期退出 (01:41 正当关窗被拦);
  //   ③ 接管残留会话 (deploy 后 wineserver 存活, 新 app 同步旧注册表) 时
  //     旧会话进程死亡也被误判 —— 故 engineState=='ready' 不可作替代条件
  //     (接管后同样达 ready), 只有 desktop-ready 事件能区分「亲历」与「接管」。
  //   ④ D48 (2026-10-10 03:43 实测): DesktopRoot 存在 init 期误触发 (explorer
  //     尚未 spawn 时被识别为 root) → 打标集合 = 当时的 init 进程, explorer/
  //     desktop 之后 spawn 反而不带标 (gDesktopShellMarked 防重锁) → init
  //     进程一退 (winedevice exit=0), 带标集合假性「全灭」→ 误关窗 → 合成
  //     器随 surface 销毁停机 → X 消失 → explorer 断连重启循环 → 恒
  //     ready-degraded。故判空必须 AND shell 本体判据: explorer.exe/desktop
  //     任一在列表 (running) = 桌面仍在, 与标无关。两者同时成立才关窗。
  private checkDesktopShellGone(): void {
    const wm = WineWindowManager.getInstance();
    if (!wm.isDesktopAbilityRunning()) {
      return;
    }
    if (!this.desktopShellSeen) {
      return;
    }
    if (this.processList.some((p: ProcessInfo) => p.desktopShell)) {
      return;
    }
    const shellAlive = this.processList.some((p: ProcessInfo) =>
      p.name === 'explorer.exe' || p.name === 'desktop');
    if (shellAlive) {
      return;
    }
    wm.closeX11Canvas();
  }
}

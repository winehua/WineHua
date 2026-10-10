import testNapi from 'libentry.so';
import smokeNapi from 'libsmoke_napi.so';
import displayRouteNapi from 'libdisplay_route_napi.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  WineEnvService, DRIVE_C, readTextIfExists, removeFileIfExists,
  ensureDirectory, writeTextFile
} from '../service/WineEnvService';
import { SmokeJob, SmokeJobParams, SmokeRequest, SmokeSuiteTest,
  InjectSpec, InjectAction } from './SmokeTypes';
import { mapMouseButton } from '../common/MouseMap';
import { DisplayRouteService } from '../service/DisplayRouteService';
import { WineWindowManager } from '../service/WineWindowManager';

// smokeMapPoint 返回（XComponent 内物理坐标，与 libsmoke_napi.so 的 d.ts 对应）
interface PointMap {
  px: number;
  py: number;
}

// displayRouteFrameSize 返回（X 屏幕逻辑尺寸 = guest 桌面坐标系，
// 与 libdisplay_route_napi.so 的 d.ts 对应）
interface FrameSize {
  w: number;
  h: number;
}

const DOMAIN = 0x0000;
const TAG = 'SMOKE';

// suites.json (assemble.sh 随载荷生成的编排定义) 的运行时视图
interface SmokeSuite {
  tests?: SmokeSuiteTest[];
}

interface SuitesDoc {
  suiteVersion?: string;
  suites?: Record<string, SmokeSuite>;
}

interface SmokeTestResult {
  schemaVersion: number;
  runId: string;
  testId: string;
  status: string;
  stage: string;
  message?: string;
  pid?: number;
  metrics?: Record<string, number>;
}

interface SmokeSummary {
  schemaVersion: number;
  runId: string;
  suite: string;
  status: string;
  tests?: SmokeTestResult[];
}

// 结果文件 / runWineProgram 返回的窄化视图 (ArkTS 禁止对象字面量类型声明)
interface SmokeResultFile {
  status?: string;
  stage?: string;
}

interface RunProgramResult {
  pid: number;
}

// queryWineProcess 返回的窄化视图 (native 进程注册表快照)
interface ProcessSnapshot {
  found: boolean;
  status: string;
  exitCode?: number | null;
  exitCodeSource?: string;
}

// 结果 JSON 的终态集合 (与 smoke 程序协议一致: PASS/FAIL/SKIP/UNSUPPORTED)
const FINAL_STATUSES: string[] = ['PASS', 'FAIL', 'SKIP', 'UNSUPPORTED'];
const POLL_INTERVAL_MS: number = 500;
// 进程退出与结果落盘有先后差: 判崩溃前多等几轮再定
const EXIT_GRACE_READS: number = 4;
const EXIT_GRACE_MS: number = 150;

// 瘦编排解释器: 读 suites.json → 逐测试 runWineProgram → 轮询结果 JSON → 汇总
// suite-summary.json。只搬运 env 不懂 env; 产品语义 env (DXVK 稳定化/后端档位) 由
// native 管线收口, 本类只注入结果协议键 RUN_ID/TEST_ID 与 suites 声明的诊断键。
export class SmokeRunner {
  private static instance: SmokeRunner | null = null;

  static getInstance(): SmokeRunner {
    if (!SmokeRunner.instance) {
      SmokeRunner.instance = new SmokeRunner();
    }
    return SmokeRunner.instance;
  }

  private listeners: (() => void)[] = [];
  private busy: boolean = false;
  private currentStatus: string = '';
  private currentResult: string = '';
  private currentRunId: string = '';

  subscribe(listener: () => void): void {
    this.listeners.push(listener);
  }

  // 退订: 组件 aboutToDisappear 必须调用, 否则每次挂载都积一份闭包
  unsubscribe(listener: () => void): void {
    const i = this.listeners.indexOf(listener);
    if (i >= 0) {
      this.listeners.splice(i, 1);
    }
  }

  private notify(): void {
    for (const l of this.listeners) {
      l();
    }
  }

  isRunning(): boolean {
    return this.busy;
  }

  statusText(): string {
    return this.currentStatus;
  }

  resultText(): string {
    return this.currentResult;
  }

  runId(): string {
    return this.currentRunId;
  }

  // 单入口: want/ui 双通道共用。忙时直接拒绝 (调用方已做 enabled 拦截)。
  start(request: SmokeRequest): void {
    if (this.busy) {
      hilog.warn(DOMAIN, TAG, 'start blocked (busy) suite=%{public}s', request.suite);
      return;
    }
    this.busy = true;
    this.currentRunId = request.runId;
    this.currentStatus = `Smoke ${request.suite} 启动 (${request.runId})`;
    this.currentResult = '';
    this.notify();
    void this.run(request);
  }

  private async run(request: SmokeRequest): Promise<void> {
    hilog.info(DOMAIN, TAG, 'run begin suite=%{public}s runId=%{public}s prefixReady=%{public}d',
      request.suite, request.runId, WineEnvService.getInstance().prefixReady);
    try {
      const envSvc = WineEnvService.getInstance();
      if (!envSvc.prefixReady) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', 'engine prefix not ready', -1)
        ]);
        return;
      }
      // [[SMOKE]] M0-T7: job.displayroute=true 触发显示路线 bring-up
      // (smokeNapi → entry WineHua_DisplayRoute_Start), 供无 UI 自动化编排;
      // displayroute-only (无测试项) 触发后即收尾
      if (request.job && request.job.displayroute) {
        hilog.info(DOMAIN, TAG, 'job.displayroute: triggering DisplayRoute start');
        // D8: 引擎代际守卫 —— x11 路线的印章必须随冷启 want 盖下
        // (Entry onCreate 双写 ArkTS override + native mirror → BuildWineEnv
        // 给引擎全部代际发路由键)。job.displayroute 触发晚于冷启时, 引擎
        // (含桌面 explorer 链) 已按 wayland 驱动 spawn, 此刻补章无法改写
        // 已有代际 —— 桌面建到 wine-wayland socket, X 上永无桌面窗
        // (v11-v13 实证); 触发后重启引擎又会在首启半程杀坏 prefix
        // (v14/v15 实测)。所以只认真冷启会话, 否则失败并指路。
        const wasX11 = DisplayRouteService.getInstance().isX11();
        if (!wasX11) {
          await this.finishSuite(request, [
            this.failure(request, 'suites',
              'displayroute 需要 x11 冷启: 用 --display-route x11 重跑 (want 随冷启盖章); 运行中补章改不了已 spawn 的引擎代际', -1)
          ]);
          return;
        }
        // D5: 路线状态先于链启动。smoke 接入不经 want (裸 aa start 带参
        // failInit), ArkTS 路线必须在此补齐 —— 否则画布页 (DesktopWindow)
        // 按 wayland 分支处理 surface/输入, 合成器绑到 dev panel 800x600
        // 小面, 全屏画布全程不绑定 (v9 实测三症状同此根因)。
        DisplayRouteService.getInstance().setOverrideMode('x11');
        // D8: native 镜像同步盖章 (与 EntryAbility want 路径同款双写)。
        // BuildWineEnv 的印章读点在此镜像 —— 缺它 = 会话 env 不带路由键
        // 和 DISPLAY=:0, wine_child 的撤 WAYLAND_DISPLAY 分支不触发, 桌面
        // 链 (explorer/keep) 走 wayland 驱动把桌面建到 wine-wayland socket
        // 上, X 上永远没有桌面窗 (v11/v12 tree-dump 实证: 树上只有
        // notepad+菜单, 无 DESKTOP 类窗)。引擎重启代际在 stamp 之后 spawn,
        // 新代际全部吃到盖章。
        displayRouteNapi.setDisplayRoute('x11');
        // M1-T4: X 档位用例与出图链的就绪同步 —— 先删旧标记再触发, 等新
        // 标记 (display_compositor 链尾写入) 后才 spawn, 否则 X client
        // 抢在 X socket 就绪前连接 (winemine 首跑实测竞态)。
        removeFileIfExists(`${DRIVE_C}/displayroute-ready`);
        // 第二参 = 真机门自动注入脚本 (T1-T5 验证编排): 本路径是 displayroute
        // 回归验证流, 编排即判据载体, 缺省开; 手动面板/injectAction 路径不传 =
        // 默认关 (测试资产不默认进产品行为)。job 可显式关 (手动验收类 job
        // 用产品级干净会话, 见 SmokeTypes.displayrouteScript)。
        const scriptFlag = request.job.displayrouteScript !== false;
        // D5: 画布优先 —— 链是 first-caller-wins (g_started 后到者仅
        // refresh), 所以必须等画布就绪再启动链。裸启动流程画布由 wayland
        // 段开窗, 与 job 触发存在竞态: v10 实测触发早 120ms 到, 画布未
        // 登记 → 回退 drSid 小面重演 (present window from surface …254,
        // 画布分支落在 already started)。画布不在则主动拉起桌面窗
        // (openX11Canvas, isX11 已置位) 再等; 5s 超时才回退 dev panel
        // 小面 / headless (CI 无 UI 场景)。
        const wm = WineWindowManager.getInstance();
        let canvas = wm.getDesktopCanvas();
        if (canvas === null) {
          wm.openX11Canvas();
        }
        const canvasDeadline = Date.now() + 5000;
        while ((canvas === null || canvas.w === 0) && Date.now() < canvasDeadline) {
          await new Promise<void>((r) => setTimeout(r, 100));
          canvas = wm.getDesktopCanvas();
        }
        if (canvas !== null && canvas.w > 0) {
          const s = wm.getEffectiveScale();
          displayRouteNapi.startDisplayRouteChain(BigInt(canvas.surfaceId), scriptFlag,
            Math.round(canvas.w / s), Math.round(canvas.h / s), true);
        } else {
          hilog.warn(DOMAIN, TAG, 'displayroute: canvas not ready in 5s, fallback to dr surface');
          // T8: XComponent surface 就绪则带出图链 (AppStorage 桥, SmokeDevPanel 写)
          const drSid = AppStorage.get<string>('smoke.drSurfaceId') ?? '';
          displayRouteNapi.startDisplayRouteChain(drSid !== '' ? BigInt(drSid) : 0n, scriptFlag);
        }
        // 早退 = 本次运行确实没有测试要跑: 既无 inline, 也无套件/选测声明。
        // (M2-T3 实测: 只看 inline 会把「displayroute + 套件用例」的编排
        // 挡在 bring-up 之后直接收尾 —— 套件用例一条都不跑。)
        const noInline: boolean = !request.job.inline || request.job.inline.length === 0;
        const noSuiteWork: boolean = !request.job.suite &&
          (!request.job.tests || request.job.tests.length === 0);
        if (noInline && noSuiteWork) {
          await this.finishSuite(request, []);
          return;
        }
        const readyDeadline = Date.now() + 15000;
        while (!readTextIfExists(`${DRIVE_C}/displayroute-ready`)) {
          if (Date.now() > readyDeadline) {
            hilog.error(DOMAIN, TAG, 'displayroute-ready marker timeout (15s)');
            break;
          }
          await new Promise<void>((r) => setTimeout(r, 200));
        }
      }
      // 测试清单来源 (job > suites.json): inline 自带完整定义不读 suites.json;
      // tests 是在 suite 上的过滤; 都没有则跑整个 suite
      const job: SmokeJob | undefined = request.job;
      let tests: SmokeSuiteTest[] = [];
      if (job && job.inline && job.inline.length > 0) {
        tests = job.inline;
      } else {
        const suitesPath: string = `${DRIVE_C}/smoke/suites.json`;
        const raw: string = readTextIfExists(suitesPath);
        if (!raw) {
          await this.finishSuite(request, [
            this.failure(request, 'suites', `missing ${suitesPath} (seed 未完成或载荷缺失)`, -1)
          ]);
          return;
        }
        const doc: SuitesDoc = JSON.parse(raw) as SuitesDoc;
        const suite: SmokeSuite | undefined = doc.suites ? doc.suites[request.suite] : undefined;
        tests = suite && suite.tests ? suite.tests : [];
        const wanted: string[] = job && job.tests ? job.tests : [];
        if (wanted.length > 0) {
          tests = tests.filter((t) => wanted.includes(t.testId));
        }
      }
      if (tests.length === 0) {
        await this.finishSuite(request, [
          this.failure(request, 'suites',
            `suite "${request.suite}" 无测试 (定义缺失或 job.tests 过滤后为空)`, -1)
        ]);
        return;
      }
      const resultsDir: string = `${DRIVE_C}/smoke/results/${request.runId}`;
      ensureDirectory(resultsDir);
      const results: SmokeTestResult[] = [];
      for (let i = 0; i < tests.length; i++) {
        const test: SmokeSuiteTest = tests[i];
        this.currentStatus = `Smoke ${request.suite}: ${i + 1}/${tests.length} ${test.testId}`;
        this.notify();
        const r = await this.runTest(request, test, resultsDir);
        results.push(r);
        hilog.info(DOMAIN, TAG, 'test done suite=%{public}s test=%{public}s status=%{public}s stage=%{public}s',
          request.suite, test.testId, r.status, r.stage);
      }
      const passed = results.filter(r => r.status === 'PASS').length;
      const summary: SmokeSummary = {
        schemaVersion: 1,
        runId: request.runId,
        suite: request.suite,
        status: passed === results.length && results.length > 0 ? 'PASS' : 'FAIL',
        tests: results
      };
      writeTextFile(`${resultsDir}/suite-summary.json`, JSON.stringify(summary));
      this.currentResult = `${summary.status} ${passed}/${results.length}`;
      this.currentStatus = `Smoke ${request.suite} ${summary.status} (${summary.status === 'PASS' ?
        '全部通过' : '详见 suite-summary.json'})`;
      hilog.info(DOMAIN, TAG, 'run done suite=%{public}s status=%{public}s %{public}d/%{public}d',
        request.suite, summary.status, passed, results.length);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'run ERROR: %{public}s', JSON.stringify(e));
      this.currentResult = 'ERROR';
      this.currentStatus = `Smoke ${request.suite} ERROR`;
    } finally {
      this.busy = false;
      this.notify();
    }
  }

  private async runTest(request: SmokeRequest, test: SmokeSuiteTest, resultsDir: string): Promise<SmokeTestResult> {
    const envSvc = WineEnvService.getInstance();
    // job.params 是对选中测试的统一覆盖 (调试参数组合): env 合并 (params 优先),
    // argv 追加, 后端与时长参数覆盖测试定义
    const params: SmokeJobParams = request.job && request.job.params ? request.job.params : {};
    // 结果协议键由 runner 控制; suites 声明的诊断键随后合并 (测试专属, 非产品语义)
    const environment: Record<string, string> = {
      'WINEHUA_SMOKE_RUN_ID': request.runId,
      'WINEHUA_SMOKE_TEST_ID': test.testId
    };
    const declared: Record<string, string> = test.env || {};
    for (const key of Object.keys(declared)) {
      if (declared[key] !== undefined) {
        environment[key] = declared[key];
      }
    }
    const overridden: Record<string, string> = params.env || {};
    for (const key of Object.keys(overridden)) {
      if (overridden[key] !== undefined) {
        environment[key] = overridden[key];
      }
    }
    // seconds/timeoutMs: 测试定义 > job.params > 默认; -1 = 取请求的 longSeconds
    const definedSeconds: number = test.seconds === undefined ? 0 : test.seconds;
    const unresolved: number = params.seconds === undefined ? definedSeconds : params.seconds;
    const seconds: number = unresolved < 0 ? (request.longSeconds || 3600) : unresolved;
    const definedTimeout: number = test.timeoutMs === undefined ? 120000 : test.timeoutMs;
    const timeoutMs: number = params.timeoutMs === undefined ? definedTimeout : params.timeoutMs;
    const resolvedTimeout: number = timeoutMs < 0 ? seconds * 1000 + 90000 : timeoutMs;
    // arg 展开: 'std' (默认) = smoke 统一协议 argv, runner 生成
    // (--automation → 结果协议由 argv 承载, 见 winehua_smoke_protocol.h);
    // 'raw' = 原样 argv (d3d12 的 --frames/--result/--checkpoint), 占位符替换
    const resultPath: string = `${resultsDir}/${test.testId}.json`;
    const argv: string[] = [];
    if (test.argvMode === 'raw') {
      const declaredArgv: string[] = test.argv || [];
      for (const arg of declaredArgv) {
        argv.push(arg.split('<run-id>').join(request.runId)
          .split('<test-id>').join(test.testId));
      }
    } else {
      // 协议键必须 Windows 盘符形式 (C:\smoke\results\...): native 路径
      // 转反斜杠是无盘符形式 (\data\...), wine 解析为当前盘根下相对路径,
      // PE 结果落错目录 — runner 轮询 native resultPath 永远超时 (实测
      // 渲染 613 帧完成但结果文件未出现在轮询目录)。raw 模式由 suites
      // 自带盘符形式 argv, 不经过里。
      const std = ['--automation', '--run-id', request.runId,
        '--test-id', test.testId,
        '--result', `C:\\smoke\\results\\${request.runId}\\${test.testId}.json`];
      if (test.mode === 'offscreen') {
        argv.push(...std, '--offscreen');
      } else {
        argv.push(...std, '--present', '--seconds', `${seconds}`);
      }
      const extra: string[] = (test.extraArgs || []).concat(params.argv || []);
      for (const arg of extra) {
        argv.push(arg);
      }
    }
    // d3d 后端: job.params > suites 声明 (回归跑固定后端) > 当前档位 (UI 手动 core)
    const d3dBackend: string = params.d3dBackend ? params.d3dBackend :
      (test.d3dBackend ? test.d3dBackend : envSvc.getD3dBackend());
    const winPath: string = 'C:\\smoke\\' + test.exe.replace(/\//g, '\\');
    // 注入残留清理 (D30, r20261007-054026 实锤): 上一轮的 inject-request/
    // inject-done 落盘后不自动消失, runner 轮询先于 guest 清理时会把**上一
    // 轮的请求**当本轮的注入 (打在前一会话的桌面状态上), 随后 injectHandled
    // 拉黑 testId, guest 的真正请求永不被处理 —— 表现为成套跑注入类用例
    // 间歇全空而 inject-done 显示 ok。spawn 前删双文件, 只认本轮 guest 的
    // 新请求。
    if (test.inject) {
      removeFileIfExists(`${DRIVE_C}/smoke/inject-request-${test.testId}.json`);
      removeFileIfExists(`${DRIVE_C}/smoke/inject-done-${test.testId}.json`);
    }
    hilog.info(DOMAIN, TAG, 'spawn test=%{public}s exe=%{public}s seconds=%{public}d timeoutMs=%{public}d back=%{public}s',
      test.testId, winPath, seconds, resolvedTimeout, d3dBackend);
    const dxvkBackend: string = params.dxvkBackend ? params.dxvkBackend :
      (test.dxvkBackend ? test.dxvkBackend : envSvc.getDxvkBackend());
    const process = testNapi.runWineProgram({
      windowsExePath: winPath,
      argv: argv,
      environment: environment,
      workingDirectory: 'C:\\smoke',
      d3dBackend: d3dBackend,
      dxvkBackend: dxvkBackend
    }) as RunProgramResult;
    if (!process || process.pid <= 0) {
      return this.failure(request, 'startup', `runWineProgram failed: ${winPath}`, -1, test.testId);
    }
    // 载体用例 (judge='external'): 真应用写不出结果文件, 等文件必然拖满 timeout
    // 并被判成失败 —— 那是把"无自报"误判成"没跑成"。这里只跑声明的时长窗口,
    // 到点结束进程, 落一条 status=SKIP 的"无结论"结果: 设备端不判定, 行为判定
    // 归编排 (displayroute 注入脚本) 与外部证据。
    if (test.judge === 'external') {
      const windowMs: number = (seconds > 0 ? seconds : 60) * 1000;
      hilog.info(DOMAIN, TAG, 'external vehicle test=%{public}s window=%{public}dms pid=%{public}d',
        test.testId, windowMs, process.pid);
      await new Promise<void>((r) => setTimeout(r, windowMs));
      const alive = testNapi.queryWineProcess(process.pid) as ProcessSnapshot;
      if (alive.found && alive.status === 'running') {
        testNapi.terminateWineProcess(process.pid);
      }
      writeTextFile(resultPath, JSON.stringify({
        schemaVersion: 1, runId: request.runId, testId: test.testId,
        status: 'SKIP', stage: 'external-vehicle',
        message: '载体用例: 无自报结论 (判定在编排/外部证据)', seconds: seconds
      }));
      return {
        schemaVersion: 1, runId: request.runId, testId: test.testId,
        status: 'SKIP', stage: 'external-vehicle', pid: process.pid
      };
    }
    // 子进程死了 (崩溃/被杀) 就不会再有结果文件, 只盯文件的轮询会把崩溃拖满
    // 整个 timeout, 失败信息只剩一句"轮询超时", 崩溃原因整个丢掉。查一次
    // native 进程注册表 (无 IO) 确认已退出的: 宽限几轮等结果落盘, 仍无终态
    // 即判失败并带上退出码与来源。
    const deadline = Date.now() + resolvedTimeout;
    let lastText: string = '';
    while (Date.now() < deadline) {
      await new Promise<void>((r) => setTimeout(r, POLL_INTERVAL_MS));
      if (test.inject) {
        await this.runInjectable(test);
      }
      lastText = readTextIfExists(resultPath);
      let final: SmokeResultFile | null = SmokeRunner.parseFinal(lastText);
      const snapshot = testNapi.queryWineProcess(process.pid) as ProcessSnapshot;
      const exited: boolean = !snapshot.found || snapshot.status !== 'running';
      if (exited) {
        for (let grace = 0; grace < EXIT_GRACE_READS && !final; grace++) {
          await new Promise<void>((r) => setTimeout(r, EXIT_GRACE_MS));
          lastText = readTextIfExists(resultPath);
          final = SmokeRunner.parseFinal(lastText);
        }
        if (!final) {
          const code: string = snapshot.exitCode === undefined || snapshot.exitCode === null
            ? '?' : `${snapshot.exitCode}`;
          const source: string = snapshot.exitCodeSource ? snapshot.exitCodeSource : 'unknown';
          hilog.error(DOMAIN, TAG,
            'test=%{public}s pid=%{public}d exited before result exit=%{public}s source=%{public}s',
            test.testId, process.pid, code, source);
          return this.failure(request, 'crashed',
            `进程在写出结果前退出 (exit=${code}, source=${source})`,
            process.pid, test.testId);
        }
      }
      if (final) {
        // 用例独立 (硬约束 5): 结果文件先于进程退出出现 (用例写完结果还有
        // 收尾 pump+销毁窗), 立刻 spawn 下一个会与前窗短暂共存 —— x11 虚拟
        // 桌面的注入按 win32 位置路由, 残留窗盖住注入点时 click/wheel 落进
        // 残留窗、焦点被夺 (D29 实测 r20261007-051133: keyboard 窗未退,
        // wheel 的 click 全空)。等进程真退再返回; 超时强杀兜底。
        if (!(await this.waitForProcessExit(process.pid, 3000))) {
          hilog.warn(DOMAIN, TAG, 'test=%{public}s pid=%{public}d 退出宽限超时, 强杀',
            test.testId, process.pid);
          testNapi.terminateWineProcess(process.pid);
          await this.waitForProcessExit(process.pid, 2000);
        }
        return {
          schemaVersion: 1,
          runId: request.runId,
          testId: test.testId,
          status: final.status ? `${final.status}` : '',
          stage: final.stage ? `${final.stage}` : 'complete',
          pid: process.pid
        };
      }
    }
    hilog.error(DOMAIN, TAG, 'timeout test=%{public}s lastText=%{public}s',
      test.testId, lastText.substring(0, 200));
    // 超时路径同样清进程: 残留存活进程会以同样方式毒化下一个用例
    testNapi.terminateWineProcess(process.pid);
    await this.waitForProcessExit(process.pid, 2000);
    return this.failure(request, 'timeout', `轮询超时 ${resolvedTimeout}ms (结果文件: ${resultPath})`, process.pid, test.testId);
  }

  private async waitForProcessExit(pid: number, timeoutMs: number): Promise<boolean> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const snap = testNapi.queryWineProcess(pid) as ProcessSnapshot;
      if (!snap.found || snap.status !== 'running') {
        return true;
      }
      await new Promise<void>((r) => setTimeout(r, 100));
    }
    return false;
  }

  // 已处理完注入的 testId（同一轮询循环内只编排一次）
  private injectHandled: Set<string> = new Set();

  // C 型注入编排: 等 guest 的 inject-request-<testId>.json（guest 声明希望
  // 被注入的序列）→ 逐条 smokeMapPoint 换算 + findToplevelAt 定位 +
  // sendPointerEvent 注入（与 ArkTS 触摸同语义: raiseToplevel + Press/Release）
  // → 写 inject-done-<testId>.json。失败写 error, guest 端按收不到消息断言
  // 失败上报 — runner 不改写用例结论。
  private async runInjectable(test: SmokeSuiteTest): Promise<void> {
    if (this.injectHandled.has(test.testId)) return;
    const reqPath: string = `${DRIVE_C}/smoke/inject-request-${test.testId}.json`;
    const raw: string = readTextIfExists(reqPath);
    if (!raw) return;
    this.injectHandled.add(test.testId);

    let status: string = 'ok';
    let message: string = '';
    let doneCount: number = 0;
    let spec: InjectSpec | null = null;
    try {
      spec = JSON.parse(raw) as InjectSpec;
    } catch (e) {
      status = 'error';
      message = 'inject-request JSON parse failed';
    }
    if (spec && spec.actions && spec.actions.length > 0) {
      const gap: number = spec.actionGapMs && spec.actionGapMs > 0 ? spec.actionGapMs : 250;
      for (const action of spec.actions) {
        const err: string = await this.injectAction(action);
        if (err) {
          status = 'error';
          message = `action ${doneCount} (${action.type}@${action.x},${action.y}): ${err}`;
          break;
        }
        doneCount++;
        await new Promise<void>((r) => setTimeout(r, gap));
      }
    } else if (spec && (!spec.actions || spec.actions.length === 0)) {
      status = 'error';
      message = 'inject-request has no actions';
    }
    writeTextFile(`${DRIVE_C}/smoke/inject-done-${test.testId}.json`,
      JSON.stringify({ status: status, done: doneCount, message: message }));
    hilog.info(DOMAIN, TAG, 'inject test=%{public}s status=%{public}s done=%{public}d %{public}s',
      test.testId, status, doneCount, message);
  }

  // type/ime 动作复用上一个 mouse 动作解析出的 toplevel（键盘焦点窗口）
  private lastInjectTl: number = 0;

  private async injectAction(action: InjectAction): Promise<string> {
    if (action.type === 'sleep') {
      const ms: number = action.ms && action.ms > 0 ? action.ms : 100;
      await new Promise<void>((r) => setTimeout(r, ms));
      return '';
    }
    if (action.type === 'displayroute') {
      // [[SMOKE]] M0-T7: 触发显示路线 bring-up (displayRouteNapi → entry WineHua_DisplayRoute_Start)
      displayRouteNapi.startDisplayRouteChain();
      await new Promise<void>((r) => setTimeout(r, 300));
      return '';
    }
    if (action.type === 'type') {
      return this.injectTyping(action.text ? action.text : '');
    }
    if (action.type === 'ime') {
      // XIM Task 4: x11 路线上屏走 XIM_COMMIT (焦点由 X 侧 XGetInputFocus
      // 决定, 不需要 wayland toplevel 句柄 —— 分支须在 lastInjectTl 检查前)。
      if (displayRouteNapi.displayRouteIsX11()) {
        displayRouteNapi.injectDisplayRouteText(action.text ? action.text : '');
        return '';
      }
      if (!this.lastInjectTl) return 'ime action without a prior mouse action (no focus target)';
      testNapi.sendImeCommit(action.text ? action.text : '');
      return '';
    }
    if (action.type === 'swipe') {
      return this.injectSwipe(action);
    }
    if (action.type === 'byTitle') {
      // M4a-T6 (I4): fusion 多窗按窗注入 —— 目标窗由 title 定位 (探针窗
      // 的 xs title 经 title 事件同步到 entry), 点击窗内中心。坐标基准 =
      // OHOS 承载窗局部物理像素 (sendPointerEvent 契约, x11 分叉按
      // win_w/win_h 比例换算到 X 逻辑)。wayland 路线不用本分支 (无 title
      // 定位需求), 走它会静默找不到窗。
      if (!action.title) return 'byTitle action missing title';
      // title 事件是异步链 (winex11→xwm→FireToplevel→本表), 找不到时等它
      // 收敛而不是判死 —— 探针写 inject-request 后 title 同步有秒级窗口。
      let tl = WineWindowManager.getInstance().findToplevelByTitle(action.title);
      for (let tries = 0; !tl && tries < 10; tries++) {
        await new Promise<void>((r) => setTimeout(r, 500));
        tl = WineWindowManager.getInstance().findToplevelByTitle(action.title);
      }
      if (!tl) return `no window titled "${action.title}" (not attached or title not synced)`;
      const win = WineWindowManager.getInstance().getWindowForToplevel(tl);
      if (!win) return `window for "${action.title}" not registered`;
      const rect = win.getWindowProperties().windowRect;
      const cx: number = rect.width / 2;
      const cy: number = rect.height / 2;
      testNapi.sendPointerEvent(tl, 3, cx, cy, 0);   // Move
      await new Promise<void>((r) => setTimeout(r, 60));
      testNapi.sendPointerEvent(tl, 1, cx, cy, 1);   // Press (left)
      await new Promise<void>((r) => setTimeout(r, 40));
      testNapi.sendPointerEvent(tl, 2, cx, cy, 1);   // Release
      return '';
    }
    if (action.type === 'wheel') {
      return this.injectWheel(action);
    }
    if (action.type === 'key') {
      // 裸键动作（evdev 码，如 KEY_ENTER=28），发给焦点 toplevel
      if (action.code === undefined) return 'key action missing code';
      if (this.x11Frame()) return this.x11Key(action.code);
      if (!this.lastInjectTl) return 'key action without a prior mouse action';
      testNapi.sendKeyEvent(this.lastInjectTl, action.code, true);
      await new Promise<void>((r) => setTimeout(r, 30));
      testNapi.sendKeyEvent(this.lastInjectTl, action.code, false);
      return '';
    }
    return this.injectMouse(action);
  }

  // swipe: 等距插值 Move 序列；raw 增量走 rawDx/rawDy（相对模式 delta 通道）。
  // 起点需先有 mouse 动作建立 toplevel，swipe 自带 x/y 也用于首次定位。
  // 声明 button 时为首段按住拖拽（Press → Move×N → Release，标题栏拖动等）。
  private async injectSwipe(action: InjectAction): Promise<string> {
    if (action.x === undefined || action.y === undefined ||
        action.dx === undefined || action.dy === undefined)
      return 'swipe action missing x/y/dx/dy';
    const x11f = this.x11Frame();
    if (x11f) return this.x11Swipe(action, x11f);
    const steps: number = action.steps && action.steps > 0 ? action.steps : 10;
    const holdBtn: boolean = action.button === 'left' || action.button === 'right';
    const mapped0 = smokeNapi.smokeMapPoint(action.x, action.y) as PointMap | null;
    if (!mapped0) return 'smokeMapPoint failed (swipe)';
    const tl = this.lastInjectTl ||
      (() => {
        const found = testNapi.findToplevelAt(mapped0.px, mapped0.py) as number;
        return found > 0 ? found : 0;
      })();
    if (!tl) return 'swipe without toplevel';
    this.lastInjectTl = tl;
    const stepDx: number = action.dx / steps;
    const stepDy: number = action.dy / steps;
    const btn = mapMouseButton(action.button === 'right' ? 2 : 1);
    if (holdBtn) {
      testNapi.sendPointerEvent(tl, MouseAction.Press, mapped0.px, mapped0.py, btn);
      await new Promise<void>((r) => setTimeout(r, 60));
    }
    for (let i = 0; i < steps; i++) {
      testNapi.sendPointerEvent(tl, MouseAction.Move,
        mapped0.px + stepDx * (i + 1), mapped0.py + stepDy * (i + 1), 0,
        stepDx, stepDy, true);
      await new Promise<void>((r) => setTimeout(r, 20));
    }
    if (holdBtn) {
      await new Promise<void>((r) => setTimeout(r, 40));
      testNapi.sendPointerEvent(tl, MouseAction.Release,
        mapped0.px + action.dx, mapped0.py + action.dy, btn);
    }
    return '';
  }

  // wheel: 每格一次 sendScrollEvent（VERT），悬停点定位到窗口。
  // notches 是 Windows 滚轮格语义（正 = 向前滚，WM_MOUSEWHEEL +120/格）；
  // winewayland 对 VERTICAL axis 取负（wayland 正 = 内容下滚），故注入时取反。
  private async injectWheel(action: InjectAction): Promise<string> {
    if (action.x === undefined || action.y === undefined) return 'wheel action missing x/y';
    const x11f = this.x11Frame();
    if (x11f) return this.x11Wheel(action, x11f);
    const notches: number = action.notches ? action.notches : 3;
    const absN: number = Math.abs(notches);
    const sign: number = notches > 0 ? -1 : 1;
    const mapped = smokeNapi.smokeMapPoint(action.x, action.y) as PointMap | null;
    if (!mapped) return 'smokeMapPoint failed (wheel)';
    const tl = testNapi.findToplevelAt(mapped.px, mapped.py) as number;
    if (!tl || tl <= 0) return `no toplevel at physical (${mapped.px},${mapped.py})`;
    this.lastInjectTl = tl;
    for (let i = 0; i < absN; i++) {
      testNapi.sendScrollEvent(tl, 0, sign, 40, mapped.px, mapped.py);
      await new Promise<void>((r) => setTimeout(r, 60));
    }
    return '';
  }

  // ── x11 注入分支 (D28): C 型注入按路线分叉。wayland 走 sendPointerEvent/
  // sendKeyEvent (toplevel 像素坐标, 上方各 inject*); x11 走 displayRouteNapi
  // 控制面注入 (帧归一化坐标, 桥内 topmost_at 命中 → 窗口局部坐标; 虚拟桌面
  // 里命中桌面顶层窗后由 wine 自行路由子窗事件)。x11 无 toplevel 语义,
  // lastInjectTl 不参与——键盘焦点由先行 mouse 动作的 motion enter 建立
  // (display_input.c 悬停即激活 FocusClient), 故 type/key 仍要求 mouse 先行。

  // x11 帧尺寸 (guest 桌面坐标系) + 路线判定; 链未起或非 x11 返回 null。
  // 尺寸不缓存: D10 旋转会改帧几何, 缓存会注入到过期坐标系。
  private x11Frame(): FrameSize | null {
    if (!displayRouteNapi.displayRouteIsX11()) {
      return null;
    }
    const size = displayRouteNapi.displayRouteFrameSize() as FrameSize;
    if (!size || size.w <= 0 || size.h <= 0) {
      return null;
    }
    return size;
  }

  private async x11Mouse(action: InjectAction, f: FrameSize): Promise<string> {
    const nx: number = action.x! / f.w;
    const ny: number = action.y! / f.h;
    const btn: number = mapMouseButton(action.button === 'right' ? 2 : 1);
    // enter 建立指针/键盘焦点 (悬停即激活), motion 定位到注入点
    displayRouteNapi.injectDisplayRouteMotion(nx, ny, 0);
    displayRouteNapi.injectDisplayRouteMotion(nx, ny, 1);
    const presses: number = action.type === 'doubleClick' ? 2 : 1;
    for (let i = 0; i < presses; i++) {
      if (i > 0) await new Promise<void>((r) => setTimeout(r, 120));
      displayRouteNapi.injectDisplayRouteButton(btn, true);
      await new Promise<void>((r) => setTimeout(r, 40));
      displayRouteNapi.injectDisplayRouteButton(btn, false);
    }
    return '';
  }

  private async x11Swipe(action: InjectAction, f: FrameSize): Promise<string> {
    const steps: number = action.steps && action.steps > 0 ? action.steps : 10;
    const holdBtn: boolean = action.button === 'left' || action.button === 'right';
    const btn: number = mapMouseButton(action.button === 'right' ? 2 : 1);
    const stepDx: number = action.dx! / steps;
    const stepDy: number = action.dy! / steps;
    displayRouteNapi.injectDisplayRouteMotion(action.x! / f.w, action.y! / f.h, 0);
    displayRouteNapi.injectDisplayRouteMotion(action.x! / f.w, action.y! / f.h, 1);
    if (holdBtn) {
      displayRouteNapi.injectDisplayRouteButton(btn, true);
      await new Promise<void>((r) => setTimeout(r, 60));
    }
    for (let i = 0; i < steps; i++) {
      displayRouteNapi.injectDisplayRouteMotion(
        (action.x! + stepDx * (i + 1)) / f.w, (action.y! + stepDy * (i + 1)) / f.h, 1);
      await new Promise<void>((r) => setTimeout(r, 20));
    }
    if (holdBtn) {
      await new Promise<void>((r) => setTimeout(r, 40));
      displayRouteNapi.injectDisplayRouteButton(btn, false);
    }
    return '';
  }

  private async x11Wheel(action: InjectAction, f: FrameSize): Promise<string> {
    // wheel 按 win32 焦点窗分发 (wine server/queue.c:1827 input_shm->focus):
    // 前置 click 激活后焦点在程序窗。#93 根因 (D29, 2026-10-07) = 桥把格数
    // 当 v120 直传 (1 格被折成 1/120, DIX 凑不满增量不出 Button4/5), 已修
    // (display_input.c discrete=steps*120); 本编排无需感知单位, 保持格数语义。
    const notches: number = action.notches ? action.notches : 3;
    const absN: number = Math.abs(notches);
    // 与 wayland 分支同号公式: guest 正 notches (向前滚) → 负 discrete
    // (display_input.c discrete>0 = 滚轮向下)
    const sign: number = notches > 0 ? -1 : 1;
    const nx: number = action.x! / f.w;
    const ny: number = action.y! / f.h;
    displayRouteNapi.injectDisplayRouteMotion(nx, ny, 0);
    displayRouteNapi.injectDisplayRouteMotion(nx, ny, 1);
    for (let i = 0; i < absN; i++) {
      displayRouteNapi.injectDisplayRouteAxis(0, sign);
      await new Promise<void>((r) => setTimeout(r, 60));
    }
    return '';
  }

  private async x11Key(code: number): Promise<string> {
    displayRouteNapi.injectDisplayRouteKey(code, true);
    await new Promise<void>((r) => setTimeout(r, 30));
    displayRouteNapi.injectDisplayRouteKey(code, false);
    return '';
  }

  // ASCII 子集展开为 evdev 键序: a-z / A-Z(shift) / 0-9。
  // KEY_1=2..KEY_0=11, KEY_A=30..KEY_Z=44 (Linux include/uapi/input-event-codes.h)
  private async injectTyping(text: string): Promise<string> {
    const x11f = this.x11Frame();
    if (!x11f && !this.lastInjectTl) {
      return 'type action without a prior mouse action (no focus target)';
    }
    for (let i = 0; i < text.length; i++) {
      const ch: string = text.charAt(i);
      const code: number = ch >= 'a' && ch <= 'z' ? 30 + (ch.charCodeAt(0) - 97) :
        ch >= 'A' && ch <= 'Z' ? 30 + (ch.charCodeAt(0) - 65) :
        ch >= '1' && ch <= '9' ? 2 + (ch.charCodeAt(0) - 49) :
        ch === '0' ? 11 : ch === '.' ? 52 : ch === '_' ? 12 : -1;
      if (code < 0) return `unsupported char '${ch}' (ascii subset only)`;
      const needShift: boolean = ch >= 'A' && ch <= 'Z' || ch === '_';
      if (x11f) {
        if (needShift) {
          displayRouteNapi.injectDisplayRouteKey(42, true);  // KEY_LEFTSHIFT
        }
        await this.x11Key(code);
        if (needShift) {
          displayRouteNapi.injectDisplayRouteKey(42, false);
        }
        await new Promise<void>((r) => setTimeout(r, 30));
        continue;
      }
      const tl: number = this.lastInjectTl;
      if (needShift) testNapi.sendKeyEvent(tl, 42, true);  // KEY_LEFTSHIFT
      testNapi.sendKeyEvent(tl, code, true);
      await new Promise<void>((r) => setTimeout(r, 30));
      testNapi.sendKeyEvent(tl, code, false);
      if (needShift) testNapi.sendKeyEvent(tl, 42, false);
      await new Promise<void>((r) => setTimeout(r, 30));
    }
    return '';
  }

  private async injectMouse(action: InjectAction): Promise<string> {
    if (action.x === undefined || action.y === undefined) return 'mouse action missing x/y';
    const x11f = this.x11Frame();
    if (x11f) return this.x11Mouse(action, x11f);
    const mapped = smokeNapi.smokeMapPoint(action.x, action.y) as PointMap | null;
    if (!mapped) return 'smokeMapPoint failed (renderer/letterbox not ready)';
    const px: number = mapped.px;
    const py: number = mapped.py;
    const tl = testNapi.findToplevelAt(px, py) as number;
    if (!tl || tl <= 0) return `no toplevel at physical (${px},${py})`;
    testNapi.raiseToplevel(tl);
    this.lastInjectTl = tl;
    // ArkUI MouseButton: Left=1, Right=2 → evdev code
    const btn = mapMouseButton(action.button === 'right' ? 2 : 1);
    const presses: number = action.type === 'doubleClick' ? 2 : 1;
    for (let i = 0; i < presses; i++) {
      if (i > 0) await new Promise<void>((r) => setTimeout(r, 120));
      testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, btn);
      await new Promise<void>((r) => setTimeout(r, 40));
      testNapi.sendPointerEvent(tl, MouseAction.Release, px, py, btn);
    }
    return '';
  }

  // 结果文件 → 终态对象; 未出现 / 半写 JSON / 非终态一律 null (继续轮询)
  private static parseFinal(text: string): SmokeResultFile | null {    if (!text) return null;
    try {
      const obj = JSON.parse(text) as SmokeResultFile;
      const status = obj.status ? `${obj.status}` : '';
      return status && FINAL_STATUSES.includes(status) ? obj : null;
    } catch (_) {
      return null;
    }
  }

  private failure(request: SmokeRequest, stage: string, message: string, pid: number, testId?: string): SmokeTestResult {
    return {
      schemaVersion: 1,
      runId: request.runId,
      testId: testId || request.suite,
      status: 'FAIL',
      stage: stage,
      message: message,
      pid: pid
    };
  }

  private async finishSuite(request: SmokeRequest, results: SmokeTestResult[]): Promise<void> {
    const resultsDir: string = `${DRIVE_C}/smoke/results/${request.runId}`;
    ensureDirectory(resultsDir);
    const summary: SmokeSummary = {
      schemaVersion: 1,
      runId: request.runId,
      suite: request.suite,
      status: 'FAIL',
      tests: results
    };
    writeTextFile(`${resultsDir}/suite-summary.json`, JSON.stringify(summary));
    this.currentResult = 'FAIL';
    this.currentStatus = `Smoke ${request.suite} FAIL (${results[0] ? results[0].message : 'unknown'})`;
    hilog.error(DOMAIN, TAG, 'suite failed early suite=%{public}s cause=%{public}s',
      request.suite, results[0] ? results[0].message : '');
  }
}

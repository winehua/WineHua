import testNapi from 'libentry.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  WineEnvService, DRIVE_C, readTextIfExists,
  ensureDirectory, writeTextFile
} from './WineEnvService';
import { SmokeRequest } from '../common/SmokeTypes';

const DOMAIN = 0x0000;
const TAG = 'SMOKE';

// suites.json (assemble.sh 随载荷生成的编排定义) 的运行时视图
interface SmokeSuiteTest {
  testId: string;
  exe: string;
  env?: Record<string, string>;
  // argvMode: 默认 'std' = runner 生成 smoke 统一协议 argv
  // (--automation --run-id --test-id --result + mode/seconds);
  // 'raw' = 用 argv 字段原样 (d3d12 等自有参数的程序), <run-id>/<test-id> 占位符替换
  argvMode?: string;
  argv?: string[];
  // 'present' (默认, 带 --seconds N) / 'offscreen' (wine-vulkan offscreen 系)
  mode?: string;
  // 追加参数 (如 d3d9-cube 的 --d3d9)
  extraArgs?: string[];
  // 回归固定后端 (旧 SmokeRunner per-test 语义): 缺省 = 当前档位 (UI 手动入口用)
  d3dBackend?: string;
  dxvkBackend?: string;
  seconds?: number;
  timeoutMs?: number;
}

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

// 结果 JSON 的终态集合 (与 smoke 程序协议一致: PASS/FAIL/SKIP/UNSUPPORTED)
const FINAL_STATUSES: string[] = ['PASS', 'FAIL', 'SKIP', 'UNSUPPORTED'];
const POLL_INTERVAL_MS: number = 500;

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
      const tests: SmokeSuiteTest[] | undefined = suite ? suite.tests : undefined;
      if (!tests || tests.length === 0) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', `suite "${request.suite}" 无定义 (suites.json)`, -1)
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
    // seconds/timeoutMs: 缺失用默认; -1 = 取请求的 longSeconds (dxvk-long 系列)
    const unresolved: number = test.seconds === undefined ? 0 : test.seconds;
    const seconds: number = unresolved < 0 ? (request.longSeconds || 3600) : unresolved;
    const timeoutMs: number = test.timeoutMs === undefined ? 120000 :
      (test.timeoutMs < 0 ? seconds * 1000 + 90000 : test.timeoutMs);
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
      const std = ['--automation', '--run-id', request.runId,
        '--test-id', test.testId, '--result', `${resultPath.replace(/\//g, '\\')}`];
      if (test.mode === 'offscreen') {
        argv.push(...std, '--offscreen');
      } else {
        argv.push(...std, '--present', '--seconds', `${seconds}`);
      }
      const extra: string[] = test.extraArgs || [];
      for (const arg of extra) {
        argv.push(arg);
      }
    }
    // d3d 后端: suites 显式声明 (回归跑固定后端), 缺省 = 当前档位 (UI 手动 core)
    const d3dBackend: string = test.d3dBackend ? test.d3dBackend : envSvc.getD3dBackend();
    const winPath: string = 'C:\\smoke\\' + test.exe.replace(/\//g, '\\');
    hilog.info(DOMAIN, TAG, 'spawn test=%{public}s exe=%{public}s seconds=%{public}d timeoutMs=%{public}d back=%{public}s',
      test.testId, winPath, seconds, timeoutMs, d3dBackend);
    const process = testNapi.runWineProgram({
      windowsExePath: winPath,
      argv: argv,
      environment: environment,
      workingDirectory: 'C:\\smoke',
      d3dBackend: d3dBackend,
      dxvkBackend: test.dxvkBackend ? test.dxvkBackend : envSvc.getDxvkBackend()
    }) as RunProgramResult;
    if (!process || process.pid <= 0) {
      return this.failure(request, 'startup', `runWineProgram failed: ${winPath}`, -1, test.testId);
    }
    // 轮询结果 JSON: 文件出现且 status 为终态即采; 超时记 FAIL(timeout)
    const deadline = Date.now() + timeoutMs;
    let lastText: string = '';
    while (Date.now() < deadline) {
      await new Promise<void>((r) => setTimeout(r, POLL_INTERVAL_MS));
      lastText = readTextIfExists(resultPath);
      if (lastText) {
        try {
          const obj = JSON.parse(lastText) as SmokeResultFile;
          const status = obj.status ? `${obj.status}` : '';
          if (status && FINAL_STATUSES.includes(status)) {
            return {
              schemaVersion: 1,
              runId: request.runId,
              testId: test.testId,
              status: status,
              stage: obj.stage ? `${obj.stage}` : 'complete',
              pid: process.pid
            };
          }
        } catch (_) {
          // 半写 JSON: 继续轮询
        }
      }
    }
    hilog.error(DOMAIN, TAG, 'timeout test=%{public}s lastText=%{public}s',
      test.testId, lastText.substring(0, 200));
    return this.failure(request, 'timeout', `轮询超时 ${timeoutMs}ms (结果文件: ${resultPath})`, process.pid, test.testId);
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

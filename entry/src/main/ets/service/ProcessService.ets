import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineEnvService } from './WineEnvService';

const DOMAIN = 0x0000;

export interface ProcessInfo {
  pid: number;
  name: string;
  path: string;
  state: string;
}

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

  private listeners: Array<() => void> = [];
  private envBound: boolean = false;

  // 极简订阅: 状态变化后逐个调用
  subscribe(listener: () => void): void {
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 原 refreshProcessList
  refresh = (): void => {
    try {
      this.processList = testNapi.getProcessList() as ProcessInfo[];
    } catch (e) {
      this.processList = [];
    }
    this.notify();
  }

  // 原 doKillProcess
  kill = (pid: number): void => {
    try {
      testNapi.killProcess(pid);
      setTimeout(() => this.refresh(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'killProcess error: %{public}s', JSON.stringify(e));
    }
  }

  // 清空列表 (WineEnvService.doReset 用, 原 this.processList = [])
  clear(): void {
    this.processList = [];
    this.notify();
  }

  // 订阅 WineEnvService: state 冒号后缀为 'wine-running'/'exited'/'wine-failed' 时自动 refresh,
  // 忠实复刻原 setStateCallback 这三个分支里调 refreshProcessList 的时序
  bindEnv(env: WineEnvService): void {
    if (this.envBound) {
      return;
    }
    this.envBound = true;
    env.subscribe(() => {
      const s = env.lastStateMsg;
      const colonIdx = s.indexOf(':');
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;
      if (stateMsg == 'wine-running' || stateMsg == 'exited' || stateMsg == 'wine-failed') {
        this.refresh();
      }
    });
  }
}

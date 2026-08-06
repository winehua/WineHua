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

// wine 内部系统进程 (broker 已全量登记): 运行中页只显示用户程序,
// 隐藏 wineserver/services 等服务进程。AddProcess 登记的是 basename。
const SYSTEM_PROC_BLACKLIST: string[] = [
  'wineserver', 'services.exe', 'csrss.exe', 'winedevice.exe',
  'plugplay.exe', 'svchost.exe', 'rpcss.exe', 'wineboot.exe',
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

  private listeners: Array<() => void> = [];
  private envBound: boolean = false;
  // 刷新节流: wine 内部进程高频进出时会触发大量 process-updated,
  // 合并到 300ms 内一次刷新, 避免 UI 反复重建列表 (master 同款策略)
  private refreshPending: boolean = false;
  private refreshTimer: number = -1;

  // 极简订阅: 状态变化后逐个调用
  subscribe(listener: () => void): void {
    this.listeners.push(listener);
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

  // 节流刷新: 状态消息驱动 (process-updated 高频), 手动 ↻ 走 refresh 直刷
  private refreshThrottled = (): void => {
    if (this.refreshPending) {
      return;
    }
    this.refreshPending = true;
    this.refreshTimer = setTimeout(() => {
      this.refreshPending = false;
      this.refreshTimer = -1;
      this.refresh();
    }, 300);
  }

  // 订阅 WineEnvService: 进程相关状态 (含 broker 登记的 process-updated)
  // 到达时节流刷新; 复刻 master refreshProcessList 的触发消息集 + 300ms 合并
  bindEnv(env: WineEnvService): void {
    if (this.envBound) {
      return;
    }
    this.envBound = true;
    env.subscribe(() => {
      const s = env.lastStateMsg;
      const colonIdx = s.indexOf(':');
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;
      if (stateMsg == 'wine-running' || stateMsg == 'exited' || stateMsg == 'wine-failed' ||
        stateMsg == 'process-updated') {
        this.refreshThrottled();
      }
    });
  }
}

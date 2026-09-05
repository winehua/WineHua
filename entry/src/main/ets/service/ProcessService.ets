import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineEnvService } from './WineEnvService';

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
// 不过滤它进程列表永不为空, "全部退出" 判定永远不成立
const SYSTEM_PROC_BLACKLIST: string[] = [
  'wineserver', 'services.exe', 'csrss.exe', 'winedevice.exe',
  'plugplay.exe', 'svchost.exe', 'rpcss.exe', 'wineboot.exe',
  'winehua_keep.exe',
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
  // 刷新节流: wine 内部进程高频进出时会触发大量 proc-updated 事件,
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
      const all = testNapi.getProcessList() as object as ProcessInfo[];
      this.processList = all.filter((p: ProcessInfo) => SYSTEM_PROC_BLACKLIST.indexOf(p.name) < 0);
    } catch (e) {
      this.processList = [];
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
    }, 300);
  }

  // 订阅引擎事件: 进程相关事件 (含 broker 登记的 proc-updated) 到达时节流刷新;
  // 复刻 master refreshProcessList 的触发集 + 300ms 合并
  bindEnv(env: WineEnvService): void {
    if (this.envBound) {
      return;
    }
    this.envBound = true;
    env.subscribeEvents((evt: string) => {
      if (evt.startsWith('proc-') || evt.startsWith('launch-')) {
        this.refreshThrottled();
      }
    });
  }
}

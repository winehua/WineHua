export const setHostShadowProfile: (profile: string) => boolean;
export const configureX11FrameProvider: (enabled: boolean) => boolean;
export const matchX11DesktopProcess: (launcherPid: number, clientPid: number) => boolean;
export interface WineX11DesktopSize { width: number; height: number; }
export const launchClient: (exePath: string, argv: string[], sockPath: string, libPath: string,
  homeDir: string, d3dBackend?: string, dxvkBackend?: string, wineLang?: string, displayEnvironment?: string[],
  x11DesktopSize?: WineX11DesktopSize) => number;
export const stopClient: () => void;
export const stopAll: (stopGraphics?: boolean) => void;
export const setStateCallback: (cb: (state: string) => void) => void;
export const runWineExe: (binDir: string, sockPath: string, libPath: string, exePath: string, homeDir: string) => void;
export interface WineProgramOptions {
  windowsExePath: string;
  argv: string[];
  environment: Record<string, string>;
  workingDirectory: string;
  d3dBackend: string;
  dxvkBackend?: string;
  // 不传时 native 按 d3dBackend 派生 (DerivePresentBackend); native 侧当前无此字段
  presentBackend?: string;
}
export interface WineProcessHandle {
  found: boolean;
  pid: number;
  status: string;
  startTimestamp: number;
  endTimestamp: number;
  exitCode: number | null;
  exitCodeSource: string;
}
export const runWineProgram: (options: WineProgramOptions) => WineProcessHandle;
export const queryWineProcess: (pid: number) => WineProcessHandle;
export const terminateWineProcess: (pid: number) => boolean;
export const checkWinePrefix: () => boolean;
export const resetWinePrefix: () => boolean;
export const setPhoneMode: (enabled: boolean) => void;
export const getProcessList: () => Array<{pid: number, name: string, path: string, state: string, desktopShell: boolean}>;
export const killProcess: (pid: number) => boolean;
export const termRun: (cols: number, rows: number, cb: (data: ArrayBuffer) => void, onExit: () => void) => number;
export const termSend: (data: ArrayBuffer) => void;
export const termResize: (cols: number, rows: number) => void;
export const termClose: () => void;

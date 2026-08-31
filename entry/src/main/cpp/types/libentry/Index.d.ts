export const startServer: (sockPath: string) => boolean;
export const setHostShadowProfile: (profile: string) => boolean;
export const launchClient: (exePath: string, argv: string[], sockPath: string, libPath: string,
  homeDir: string, automationMode?: boolean, prefixMode?: string, d3dBackend?: string,
  dxvkBackend?: string, wineLang?: string) => number;
export const stopClient: () => void;
export const stopAll: () => void;
export const setStateCallback: (cb: (state: string) => void) => void;
export const setToplevelCallback: (cb: (id: number, event: string, data: string) => void) => void;
export const setImeCallback: (cb: (active: number, x: number, y: number, w: number, h: number) => void) => void;
export const sendImeCommit: (text: string) => void;
export const sendImePreedit: (text: string, start: number, end: number) => void;
export const imeBackspace: () => void;
export const setPendingToplevel: (id: number) => void;
export const getCurrentToplevelId: () => number;
export const destroyToplevel: (id: number) => void;
export const sendToplevelClose: (id: number) => void;
export const runWineExe: (binDir: string, sockPath: string, libPath: string, exePath: string, homeDir: string) => void;
export interface WineProgramOptions {
  windowsExePath: string;
  argv: string[];
  environment: Record<string, string>;
  workingDirectory: string;
  prefixMode: string;
  d3dBackend: string;
  dxvkBackend?: string;
  presentBackend: string;
  automationMode: boolean;
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
export interface GuestProgramOptions {
  executablePath: string;
  argv: string[];
  environment: Record<string, string>;
  workingDirectory: string;
  automationMode: boolean;
}
export const runGuestProgram: (options: GuestProgramOptions) => WineProcessHandle;
export interface HostProgramOptions {
  executablePath: string;
  argv: string[];
  environment: Record<string, string>;
  workingDirectory: string;
  automationMode: boolean;
}
export const runHostProgram: (options: HostProgramOptions) => WineProcessHandle;
export const runHostReplay: (options: HostProgramOptions) => boolean;
export const isHostReplayRunning: () => boolean;
export const queryWineProcess: (pid: number) => WineProcessHandle;
export const terminateWineProcess: (pid: number) => boolean;
export const checkWinePrefix: (prefixMode?: string) => boolean;
export const resetWinePrefix: (prefixMode?: string) => boolean;
export const stageExperimentPayload: (experimentId: string, names: string[], hashes: string[],
  prefixMode: string, sourceUrl?: string) => boolean;
export const runHostVulkanProbe: (surfaceId: bigint, runId: string) => boolean;
export const stopHostVulkanProbe: () => boolean;
export const setOutputSize: (w: number, h: number) => void;
export const setDisplayScale: (scale: number) => void;
export const setDesktopMode: (enabled: boolean) => void;
export const setPhoneMode: (enabled: boolean) => void;
export const findToplevelAt: (px: number, py: number) => number;
export const raiseToplevel: (toplevelId: number) => void;
export const createRenderer: (toplevelId: number, surfaceId: BigInt) => void;
export const resizeRenderer: (toplevelId: number, width: number, height: number) => void;
export const destroyRenderer: (toplevelId: number) => void;
export const sendPointerEvent: (toplevelId: number, action: number, px: number, py: number, button: number) => void;
export const sendKeyEvent: (toplevelId: number, evdevCode: number, pressed: boolean) => void;
export const sendScrollEvent: (toplevelId: number, axis: number, value: number, scrollStep: number, px: number, py: number) => void;
export const notifyToplevelResize: (toplevelId: number, w: number, h: number) => void;
export const takeWindowMask: (toplevelId: number) => { w: number, h: number, buffer: ArrayBuffer } | null;
export const setToplevelVisible: (toplevelId: number, visible: boolean) => void;
export const getProcessList: () => Array<{pid: number, name: string, path: string, state: string, desktopShell: boolean}>;
export const killProcess: (pid: number) => boolean;
export const termRun: (cols: number, rows: number, cb: (data: ArrayBuffer) => void, onExit: () => void) => number;
export const termSend: (data: ArrayBuffer) => void;
export const termResize: (cols: number, rows: number) => void;
export const termClose: () => void;

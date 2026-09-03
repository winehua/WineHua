export const startServer: (sockPath: string) => boolean;
export const setHostShadowProfile: (profile: string) => boolean;
export const launchClient: (exePath: string, argv: string[], sockPath: string, libPath: string,
  homeDir: string, d3dBackend?: string, dxvkBackend?: string, wineLang?: string) => number;
export const stopClient: () => void;
export const stopAll: () => void;
export const setStateCallback: (cb: (state: string) => void) => void;
export const setToplevelCallback: (cb: (id: number, event: string, data: string) => void) => void;
export const setImeCallback: (cb: (active: number, x: number, y: number, w: number, h: number) => void) => void;
export const registerHostWindow: (windowId: number) => void;
export const setPointerLockCallback: (cb: (locked: boolean, toplevelId: number) => void) => void;
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
  d3dBackend: string;
  dxvkBackend?: string;
  presentBackend: string;
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
export const setOutputSize: (w: number, h: number) => void;
export const setDisplayScale: (scale: number) => void;
export const setDesktopMode: (enabled: boolean) => void;
export const setPhoneMode: (enabled: boolean) => void;
export const findToplevelAt: (px: number, py: number) => number;
export const raiseToplevel: (toplevelId: number) => void;
export const createRenderer: (toplevelId: number, surfaceId: BigInt) => void;
export const resizeRenderer: (toplevelId: number, width: number, height: number) => void;
export const destroyRenderer: (toplevelId: number) => void;
export const sendPointerEvent: (toplevelId: number, action: number, px: number, py: number, button: number, rawDeltaX?: number, rawDeltaY?: number, fromMouse?: boolean) => void;
export const sendKeyEvent: (toplevelId: number, evdevCode: number, pressed: boolean) => void;
export const sendScrollEvent: (toplevelId: number, axis: number, value: number, scrollStep: number, px: number, py: number) => void;
export const notifyToplevelResize: (toplevelId: number, w: number, h: number) => void;
export const takeWindowMask: (toplevelId: number) => { w: number, h: number, buffer: ArrayBuffer } | null;
export const setToplevelVisible: (toplevelId: number, visible: boolean) => void;
export const getProcessList: () => Array<{pid: number, name: string, path: string, state: string, desktopShell: boolean}>;
export const killProcess: (pid: number) => boolean;
export const initGameController: () => number;
export const cleanupGameController: () => void;
export const isGamepadConnected: () => boolean;
export const getGamepadCount: () => number;
export const setGamepadButtonCallback: (
  callback: (buttonCode: number, pressed: boolean) => void) => void;
export const setGamepadAxisCallback: (
  callback: (axisType: number, x: number, y: number) => void) => void;
export const setGamepadDeviceCallback: (callback: (connected: boolean) => void) => void;
export const setGamepadRumbleCallback: (
  callback: (low: number, high: number, durationMs: number) => void) => void;
/** Controller Hub (Touch source + WHGP). source: 0=Touch 1=Physical 2=Keyboard */
export const controllerSetEnabled: (enabled: boolean) => void;
export const controllerSetButton: (source: number, slot: number, button: number, pressed: boolean) => void;
/** stick: 0=left 1=right. x/y already canonical (right+, up+). */
export const controllerSetStick: (source: number, slot: number, stick: number, x: number, y: number) => void;
/** trigger: 0=LT 1=RT. value 0..1. */
export const controllerSetTrigger: (source: number, slot: number, trigger: number, value: number) => void;
export const controllerSetHat: (source: number, slot: number, x: number, y: number) => void;
export const controllerResetSource: (source: number) => void;
export const controllerGetState: (slot: number) => {
  buttons: number; lx: number; ly: number; rx: number; ry: number;
  lt: number; rt: number; hatX: number; hatY: number; sequence: number;
};
export const controllerGetStateText: (slot: number) => string;
export const controllerStartBridge: (socketPath?: string) => boolean;
export const controllerStopBridge: () => void;
export const controllerGetSocketPath: () => string;
export const controllerSetOutputMode: (mode: string) => void;
export const controllerGetOutputMode: () => string;
export const termRun: (cols: number, rows: number, cb: (data: ArrayBuffer) => void, onExit: () => void) => number;
export const termSend: (data: ArrayBuffer) => void;
export const termResize: (cols: number, rows: number) => void;
export const termClose: () => void;

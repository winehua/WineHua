export const startServer: (sockPath: string) => boolean;
export const setHostShadowProfile: (profile: string) => boolean;
export const launchClient: (exePath: string, argv: string[], sockPath: string, libPath: string,
  homeDir: string, d3dBackend?: string, dxvkBackend?: string, wineLang?: string) => number;
export const stopClient: () => void;
export const stopAll: () => void;
export const setStateCallback: (cb: (state: string) => void) => void;
export const setOpenTargetCallback: (cb: (target: string) => void) => void;
/** guest 复制文本 -> ArkTS (写系统剪贴板)。 */
export const setClipboardUpdateCallback: (cb: (text: string) => void) => void;
/** ArkTS 系统剪贴板文本 -> guest (设 X CLIPBOARD selection)。 */
export const setClipboardText: (text: string) => void;
/** 系统打开开关 (broker OPEN 门禁, 即时生效; 默认关 = 上游行为)。 */
export const setSystemOpenEnabled: (enabled: boolean) => void;
export const setToplevelCallback: (cb: (id: number, event: string, data: string) => void) => void;
export const setImeCallback: (cb: (active: number, x: number, y: number, w: number, h: number) => void) => void;
export const registerHostWindow: (windowId: number) => void;
export const dumpWindowLayout: (displayId?: number) => void;
export const setPointerLockCallback: (cb: (locked: boolean, toplevelId: number) => void) => void;
export const sendImeCommit: (text: string) => void;
export const sendImePreedit: (text: string, start: number, end: number) => void;
export const imeBackspace: () => void;
export const imeRearm: () => void;
export const setPendingToplevel: (id: number) => void;
export const cancelPendingToplevel: (id: number) => void;
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
export const setOutputSize: (w: number, h: number) => void;
export const setDisplayScale: (scale: number) => void;
/** Wine DPI (96 × WINE_CONTENT_SCALE=2.0 = 192): 呈现 1:1 时按物理分辨率渲染字形。非设备密度。 */
export const setWineLogPixels: (dpi: number) => void;
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
export const notifyToplevelResize: (toplevelId: number, w: number, h: number, resizing?: boolean) => void;
export const notifyToplevelResizeEnd: (toplevelId: number) => void;
// 任务 #135: 承载窗全局位置上报 (guest 逻辑坐标 = windowRect 原点 / effectiveScale)。
// 仅 x11 路线生效 (wayland 无绝对位置语义, 静默 no-op)。
export const notifyToplevelMove: (toplevelId: number, x: number, y: number) => void;
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

// Runtime APIs added on feature/vpp-runtime; upstream declarations above are preserved.
export const setFrameLoopDiagnostics: (enabled: boolean) => void;
export interface PhoneSharedBufferProbeResult {
  gate: string;
  status: string;
  stage: string;
  lastChildStage: string;
  loaderPath: string;
  deviceName: string;
  childStderr: string;
  parentPid: number;
  pid: number;
  forkServerPid: number;
  launchCode: number;
  childExitCode: number;
  childWaitStatus: number;
  childSignal: number;
  childReaped: boolean;
  childExitObserved: boolean;
  localParcelRoundtrip: boolean;
  preserveLowMappings: boolean;
  standardFork: boolean;
  systemStart: boolean;
  guestExport: boolean;
  childImageAllocations: number;
  childNativeBufferExports: number;
  childRealReleaseFenceImports: number;
  childRealRenderFenceExports: number;
  hostRealRenderFenceImports: number;
  hostRealReleaseFenceExports: number;
  childKilledByProbe: boolean;
  childDeviceCreated: boolean;
  vkResult: number;
  systemError: number;
  parentFdBefore: number;
  parentFdAfter: number;
  backingFds: number;
  originalFieldCount: number;
  framesVerified: number;
  childImageImports: number;
  childReleaseFenceImports: number;
  childRenderFenceExports: number;
  hostImageImports: number;
  hostImageReuses: number;
  hostRenderFenceImports: number;
  hostReleaseFenceExports: number;
  diagnosticCpuReadBytes: number;
  imageCpuReadBytes: number;
  imageCpuUploadBytes: number;
  imageGpuCopyCount: number;
}
export const runPhoneSharedBufferProbe: (preserveLowMappings?: boolean, standardFork?: boolean,
  systemStart?: boolean, guestExport?: boolean) => Promise<PhoneSharedBufferProbeResult>;
export interface ProductDirectProbeResult {
  supported: boolean;
  status: string;
  stage: string;
  deviceName: string;
  framesVerified: number;
}
export const probeProductDirectSupport: () => Promise<ProductDirectProbeResult>;
export interface ExternalImageProbeResult {
  handleType: number;
  format: number;
  tiling: number;
  usage: number;
  result: number;
  features: number;
  compatibleHandleTypes: number;
  exportFromImportedHandleTypes: number;
}
export interface DirectVulkanProbeResult {
  nativeBufferMemory?: string;
  gate: string;
  status: string;
  launchMode: string;
  stage: string;
  pid: number;
  callbackStatus: number;
  parentFdCount: number;
  parentRssKiB: number;
  ncpStatus: number;
  vkResult: number;
  loaderVersion: number;
  requestedApiVersion: number;
  instanceExtensionCount: number;
  deviceExtensionCount: number;
  nativeCapabilities: number;
  externalImages: ExternalImageProbeResult[];
  opaqueFdBufferQueried: boolean;
  opaqueFdBufferFeatures: number;
  apiVersion: number;
  icdEnvironment: string;
  pixelCheck: boolean;
  elapsedMs: number;
  loaderPath: string;
  deviceName: string;
}
export const runDirectVulkanProbe: (earlyPhoneFork?: boolean) => Promise<DirectVulkanProbeResult>;
export const runDirectVulkanInlineProbe: () => Promise<DirectVulkanProbeResult>;
export const preparePhoneDirectForkServer: () => number;
export const runDirectVulkanForkServerProbe: () => Promise<DirectVulkanProbeResult>;
export const runDirectVulkanCreateProbe: (nativeMemory?: boolean) => Promise<DirectVulkanProbeResult>;
export const runDirectVulkanSystemCreateProbe: () => Promise<DirectVulkanProbeResult>;
export interface DirectWineIpcProbeResult {
  gate: string;
  status: string;
  stage: string;
  pid: number;
  replyPid: number;
  fdCount: number;
  launchCode: number;
  callbackCode: number;
  ipcCode: number;
  parentFdsOpen: number;
  deathReceived: number;
  registryExited: number;
}
export const runDirectWineIpcProbe: () => Promise<DirectWineIpcProbeResult>;
export const runDirectWineBrokerIpcProbe: () => Promise<DirectWineIpcProbeResult>;
export interface DirectWineSurfaceIpcProbeResult {
  gate: string;
  status: string;
  stage: string;
  pid: number;
  launchCode: number;
  firstAttach: number;
  directRoute: number;
  firstQuery: number;
  staleRejected: number;
  staleAttachAccepted: number;
  firstQueryAfterStale: number;
  resized: number;
  oldGenerationRejected: number;
  detach: number;
  detachedQueryRejected: number;
  deathReceived: number;
}
export const runDirectWineSurfaceIpcProbe: () => Promise<DirectWineSurfaceIpcProbeResult>;
export interface DirectSurfaceProbeResult {
  gate: string;
  status: string;
  stage: string;
  pid: number;
  framesPassed: number;
  width: number;
  height: number;
  queueSize: number;
  lastBufferSeq: number;
  launchCode: number;
  callbackCode: number;
  killCode: number;
  abortMode: boolean;
  parentFdCount: number;
  parentRssKiB: number;
  pixelCheck: boolean;
  importCheck: boolean;
  importCount: number;
  reuseCount: number;
  importVkResult: number;
}
export const runDirectSurfaceProbe: () => Promise<DirectSurfaceProbeResult>;
export const runDirectSurfaceAbortProbe: () => Promise<DirectSurfaceProbeResult>;
export const runDirectGpuSurfaceProbe: () => Promise<DirectSurfaceProbeResult>;
export const runDirectGpuImportProbe: () => Promise<DirectSurfaceProbeResult>;
export interface WineLaunchResult {
  pid: number;
  sessionId: string;
  reused: boolean;
}
export const activateToplevel: (toplevelId: number) => void;
export const moveExternalHost: (source: number, host: number, resizeEdges?: number) => boolean;
export const isExternalHostResizing: (host: number) => boolean;
export const bindExternalToplevel: (source: number, host: number, sourcePid: number,
  hostPid: number, x: number, y: number, width: number, height: number) => boolean;
export const unbindExternalToplevel: (host: number) => void;
export const getKeyboardFocusedToplevel: () => number;
export const getWineStartupState: () => string;
export const initAppLog: (path: string) => void;
export const clearNativeLog: () => void;
export const setHostGraphicsBackend: (backend: string) => boolean;
export const getHostGpuName: () => string;
export const getDesktopRootId: () => number;
export interface NativeWineSessionInfo {
  pid: number; sessionId: string; path: string; state: string; toplevelId: number;
}
export const getWineSession: (sessionId: string) => NativeWineSessionInfo | null;
export const activateWineSession: (sessionId: string) => boolean;
export const stopWineSession: (sessionId: string) => boolean;
export const refreshRenderer: (toplevelId: number) => void;
export const setRendererPaused: (toplevelId: number, paused: boolean) => void;
export const getDisplayFps: (toplevelId?: number) => number;
export const wineTextInputCommit: (text: string) => boolean;
export const wineTextInputPreedit: (text: string) => boolean;
export const wineTextInputEnabled: () => boolean;
export const wineTextInputActive: () => boolean;
export const wineTextInputFocusedToplevel: () => number;
export const wineTextInputSetArmed: (armed: boolean) => void;
export const runHostVulkanProbe: (surfaceId: BigInt, runId: string) => boolean;
export const stopHostVulkanProbe: () => boolean;
export const setToplevelIsolated: (id: number, isolated: boolean) => void;

import { common, Want } from '@kit.AbilityKit';
import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { deviceInfo, BusinessError } from '@kit.BasicServicesKit';
import { pointer } from '@kit.InputKit';
import testNapi from 'libentry.so';
import { PopupWindowManager } from './PopupWindowManager';
import { ArgbWindowManager } from './ArgbWindowManager';
import { DesktopModeService } from './DesktopModeService';

const DOMAIN = 0x0000;
const TAG = 'WineWM';

// 单个 Wine toplevel 在 ArkTS 侧的聚合状态 (参照 PopupEntry/ArgbEntry 模式)。
// 'created' 事件 (或首个提前到达的事件) 时经 ensureEntry 建档,
// ctx/win/stage 由 WineWindowAbility 三步回填 (registerAbility/Window/WindowStage),
// unregisterAbility 一次性整体删除。
class WineWindowEntry {
  ctx: common.UIAbilityContext | null = null;        // terminateSelf 用
  win: window.Window | null = null;                  // resize/title 直接操作
  stage: window.WindowStage | null = null;           // popup 子窗口宿主
  starting: boolean = false;                         // startAbility 飞行中 (防重复)
  events: Array<[string, string]> = [];              // win 就绪前积压的事件
  lastResizeTargetH: number = 0;                     // 最近 resize 目标 VP 高度 (titleBarH 校准)
  lastResizeWineDim: [number, number] | null = null; // 最近 resize 的 wine 原始尺寸
  title: string = '';                                // 最近标题 (loadContent 后重置需重 apply)
  everMaximized: boolean = false;                    // 曾进入 maximize (诊断 OHOS 状态机冲突)
  moveGrabPending: boolean = false;                  // Wine move_grab 等待 startMoving()
}

export class WineWindowManager {
  private static instance: WineWindowManager | null = null;
  private windowStage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private uiAbilityContext: common.UIAbilityContext | null = null;
  // 设备显示密度 + 屏幕尺寸 (用于缩放)
  private displayScale: number = 1.0;
  private scaleBoost: number = 1.0;  // 放大系数, 使 effectiveScale = displayScale × boost = 2.0
  // 标题栏 + 边框总高度 (VP), 用于窗口 resize
  // = getWindowDecorHeight() + decorBorderOffset
  private measuredTitleBarH: number = 56;
  // API 返回的纯标题栏高度 (loadContent 后获取)
  private apiTitleBarH: number = 37;
  // 边框/阴影等 API 不涵盖的额外装饰 (VP), surface 校准自动测量
  private decorBorderOffset: number = 19;  // 56 - 37
  // 校准计数: 前几轮不做 EMA 平滑, 快速收敛
  private barCalibrationCount: number = 0;

  // -- Desktop 模式 (Tablet / PC 虚拟桌面) --
  isDesktopMode: boolean = false;  // 公开: WineEnvService 镜像
  // PC 虚拟桌面: 桌面 root surface 承载在独立 VirtualDesktopAbility 窗口
  // (master 的 tablet/phone 走 DesktopAbility 全屏窗口)。由 applyModePolicy 派生。
  desktopSeparateWindow: boolean = false;
  // PC 虚拟桌面窗口状态 (root 单例, 不用 entries Map; 参照 tablet
  // DesktopAbility 的 registerDesktopAbility/desktopAbilityCtx 模式):
  //   desktopWinStarting  — startAbility 飞行中 (防重复)
  //   desktopWindowActive — 窗口存活 (Index「打开桌面」按钮据此显隐)
  //   desktopWinCtx       — VDA onCreate 注册, root 销毁时 terminateSelf 关窗
  private desktopWinStarting: boolean = false;
  private desktopWindowActive: boolean = false;
  private desktopWinCtx: common.UIAbilityContext | null = null;
  // 启动器可见性镜像 (DesktopLayer 读): 启动器可见时桌面窗口退出全屏,
  // XComponent 高度波动, 桌面 root 不应跟随 resize (否则热重启桌面高度
  // 残留错误值)。PC 虚拟桌面窗口 (VirtualDesktop) 保持默认 true →
  // DesktopLayer 跳过 root resize, 窗口只是显示口 (renderer letterbox)。
  desktopLauncherVisible: boolean = true;
  // -- 光标隐藏门禁 (dinput 相对模式驱动, 见 applyPointerVisibility) --
  // pointerLocked: wine 最近的真实"相对模式锁定"请求 (不受展示形态门禁过滤,
  // 只是原始事实); effectiveHidden: 我们实际对系统光标做的显隐。两者分离,
  // 以便"悬浮窗 ↔ 完整桌面"切换时按 pointerLocked × 是否允许隐藏重算 — 否则
  // 悬浮态被抑制的隐藏, 切到完整桌面后没有新事件通知 → 光标残留显示 (游戏里
  // 光标浮出)。default=false, 与融合模式"只拦桌面"的判据兼容。
  private pointerLocked: boolean = false;
  private effectiveHidden: boolean = false;
  // 触发相对模式的约束 surface 的 toplevelId (解锁时=0)。用于区分"桌面 shell
  // 自身藏光标 (启动瞬时 toplevelId==desktopRootId)"与"游戏子窗口真相对模式"。
  private pointerLockToplevel: number = 0;
  private desktopRootId: number = 0;
  private desktopAbilityRunning: boolean = false;
  private desktopAbilityCtx: common.UIAbilityContext | null = null;
  // 桌面 root 存在信号 (WineEnvService 镜像 → sessionAlive 判定):
  // desktop_root 事件置 true, root 销毁置 false
  desktopActive: boolean = false;
  // 桌面 root 的 XComponent surface 校准完成（renderer 就绪）才置 true。
  desktopSurfaceReady: boolean = false;
  // 主窗口 windowStage (ARGB 异型子窗口/popup 回退宿主)
  private mainWindowStage: window.WindowStage | null = null;

  // 极简订阅 (与 WineEnvService 同款): 桌面状态变化后逐个调用
  private listeners: Array<() => void> = [];

  subscribe(listener: () => void): void {
    if (!this.listeners.includes(listener)) {
      this.listeners.push(listener);
    }
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // toplevelId -> 聚合状态 (WineWindowEntry)
  private entries: Map<number, WineWindowEntry> = new Map();
  // toplevelId -> 已销毁标记 (WineWindowAbility.onCreate 竞态拦截)。
  // 墓碑生命期严格长于 entry, 设计上永不清理, 独立于 entries 保留
  private destroyed: Set<number> = new Set();

  // entry 建档: 'created' / 提前到达的排队事件 / move_start / register 回填
  private ensureEntry(id: number): WineWindowEntry {
    let e = this.entries.get(id);
    if (!e) { e = new WineWindowEntry(); this.entries.set(id, e); }
    return e;
  }

  static getInstance(): WineWindowManager {
    if (!WineWindowManager.instance) {
      WineWindowManager.instance = new WineWindowManager();
    }
    return WineWindowManager.instance;
  }

  getMainWindow(): window.Window | null {
    return this.mainWindow;
  }

  // -- dump 当前状态 (诊断用) --
  dumpState(): void {
    hilog.info(DOMAIN, TAG, '[MW-DUMP] state: entries=%{public}d destroyed=%{public}d',
      this.entries.size, this.destroyed.size);
    this.entries.forEach((e, id) => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   entry #%{public}d: win=%{public}s starting=%{public}s events=%{public}d',
        id, e.win ? 'yes' : 'no', e.starting ? 'yes' : 'no', e.events.length);
    });
  }

  // 桌面模式判定统一入口 (init / 引擎启动前共用):
  //   Pad/phone → 恒桌面合成器 (DesktopAbility 全屏窗口, 现状)
  //   PC + 设置虚拟桌面 → 桌面合成器 + 独立 VirtualDesktopAbility 窗口
  //   PC + 设置融合 (默认) → managed 多窗口
  applyModePolicy(): void {
    const isPadOrPhone = deviceInfo.deviceType === 'tablet' || deviceInfo.deviceType === 'phone';
    this.isDesktopMode =
      isPadOrPhone || DesktopModeService.getInstance().mode === 'virtual';
    this.desktopSeparateWindow = this.isDesktopMode && !isPadOrPhone;
    testNapi.setDesktopMode(this.isDesktopMode);
    hilog.info(DOMAIN, TAG,
      '[MW] mode policy: type=%{public}s → %{public}s (separateWindow=%{public}s)',
      deviceInfo.deviceType,
      this.isDesktopMode ? 'DESKTOP MODE' : 'MANAGED MODE',
      this.desktopSeparateWindow ? 'yes' : 'no');
  }

  async init(windowStage: window.WindowStage, abilityContext: common.UIAbilityContext): Promise<void> {
    this.windowStage = windowStage;
    this.mainWindowStage = windowStage;
    this.mainWindow = windowStage.getMainWindowSync();
    this.uiAbilityContext = abilityContext;

    hilog.info(DOMAIN, TAG, '[MW] init: mainWindow=%{public}s ctx=%{public}s',
      this.mainWindow ? 'ok' : 'NULL', this.uiAbilityContext ? 'ok' : 'NULL');

    // 桌面模式判定: Pad/phone 恒桌面合成器; PC 按设置 (融合默认, 虚拟桌面可切)。
    // applyModePolicy 读 DesktopModeService (Preferences), 引擎每次启动前重判
    DesktopModeService.getInstance().init(this.uiAbilityContext);
    this.applyModePolicy();

    // 获取设备显示密度 + 主窗口可用区域
    try {
      const d = display.getDefaultDisplaySync();
      this.displayScale = d.densityPixels;
      this.scaleBoost = 2.0 / this.displayScale;  // 使 effectiveScale = 2.0
      hilog.info(DOMAIN, TAG, '[MW] display %{public}dx%{public}d deviceDPI=%{public}s boost=%{public}s -> effective=2.0',
        d.width, d.height, this.displayScale.toFixed(2), this.scaleBoost.toFixed(2));
      testNapi.setDisplayScale(this.displayScale * this.scaleBoost);

      // Wine 输出尺寸: 沉浸模式下 avoid area 为 0, 直接用显示尺寸
      const s = this.displayScale * this.scaleBoost;
      const outW = Math.round(d.width / s);
      const outH = Math.round(d.height / s);
      hilog.info(DOMAIN, TAG, '[MW] display %{public}dx%{public}d → output %{public}dx%{public}d',
        d.width, d.height, outW, outH);
      testNapi.setOutputSize(outW, outH);
    } catch (_) {
      this.displayScale = 1.61;
      this.scaleBoost = 2.0 / 1.61;
      hilog.warn(DOMAIN, TAG, '[MW] display query failed, using displayScale=1.61 boost=1.24');
      testNapi.setDisplayScale(this.displayScale * this.scaleBoost);
    }

    hilog.info(DOMAIN, TAG, '[MW] registering toplevel callback...');
    testNapi.setToplevelCallback((id: number, event: string, data: string) => {
      hilog.info(DOMAIN, TAG, '[MW]  cb toplevel #%{public}d event=%{public}s data=%{public}s',
        id, event, data);
      this.onToplevelEvent(id, event, data);
    });
    hilog.info(DOMAIN, TAG, '[MW] toplevel callback registered OK');

    // dinput 相对模式 (游戏隐藏光标无限转视角) 的 host 侧配套:
    // 注册主窗口 id 供 native OH_WindowManager_LockCursor 冻结光标;
    // 锁定状态回调里隐藏/恢复系统光标 (隐藏能力只有 ArkTS API)
    if (this.mainWindow) {
      testNapi.registerHostWindow(this.mainWindow.getWindowProperties().id);
    }
    testNapi.setPointerLockCallback((locked: boolean, fromToplevel: number) => {
      // 记录 wine 真实锁定请求与来源 toplevel (不受门禁过滤), 再由
      // applyPointerVisibility 按展示形态 + 来源决定是否 setPointerVisible —
      // 悬浮窗态抑制; 完整桌面态: 桌面 root surface (shell 自身) 也抑制, 仅
      // 子窗口 (真游戏) 跟随。解锁 (locked=false) 必然走 SHOW 分支。
      this.pointerLocked = locked;
      this.pointerLockToplevel = fromToplevel;
      this.applyPointerVisibility();
    });
    // A launcher resume recreates EntryAbility while the Wine desktop root can
    // still be alive.  No new desktop_root event is emitted in that case, so
    // explicitly bring the existing desktop compositor back to the foreground.
    if (this.isDesktopMode && this.desktopRootId > 0) {
      hilog.info(DOMAIN, TAG, '[MW] restoring existing DesktopAbility root=%{public}d',
        this.desktopRootId);
      this.startDesktopAbility(this.desktopRootId);
    }
  }

  // -- WineWindowAbility 调用: 注册 / 注销 --

  registerAbility(toplevelId: number, ctx: common.UIAbilityContext): void {
    this.ensureEntry(toplevelId).ctx = ctx;
    hilog.info(DOMAIN, TAG, '[MW]  registerAbility #%{public}d (entries=%{public}d)',
      toplevelId, this.entries.size);
  }

  registerWindow(toplevelId: number, win: window.Window): void {
    const e = this.ensureEntry(toplevelId);
    e.win = win;
    hilog.info(DOMAIN, TAG, '[MW]  registerWindow #%{public}d (entries=%{public}d, starting=%{public}s)',
      toplevelId, this.entries.size, e.starting ? 'yes' : 'no');
  }

  registerWindowStage(toplevelId: number, stage: window.WindowStage): void {
    this.ensureEntry(toplevelId).stage = stage;
    hilog.info(DOMAIN, TAG, '[MW]  registerWindowStage #%{public}d (entries=%{public}d)',
      toplevelId, this.entries.size);
  }

  getWindowStage(toplevelId: number): window.WindowStage | null {
    return this.entries.get(toplevelId)?.stage ?? null;
  }

  // effectiveScale = displayScale × scaleBoost (Wine 逻辑坐标 → 物理像素)
  getEffectiveScale(): number {
    return this.displayScale * this.scaleBoost;
  }

  getDesktopMode(): boolean {
    return this.isDesktopMode;
  }

  // 最近通过 resize 事件报告给 ArkTS 的 Wine 内容尺寸 (PC 模式防环用:
  // onSurfaceChanged 反馈 configure 前比对, 一致说明窗口尺寸由我们按
  // 内容尺寸设置, 不必再通知 Wine, 避免反馈环把窗口模式游戏推到
  // 非内容尺寸)
  getLastResizeWineDim(toplevelId: number): [number, number] | null {
    return this.entries.get(toplevelId)?.lastResizeWineDim ?? null;
  }

  unregisterAbility(toplevelId: number): void {
    // 级联销毁该窗口的全部 popup 子窗口
    PopupWindowManager.getInstance().destroyPopupsForToplevel(toplevelId);
    const had = this.entries.delete(toplevelId);
    hilog.info(DOMAIN, TAG, '[MW]  unregisterAbility #%{public}d entry=%{public}s',
      toplevelId, had ? 'removed' : 'absent');
  }

  isDestroyed(toplevelId: number): boolean {
    const result = this.destroyed.has(toplevelId);
    if (result) {
      hilog.warn(DOMAIN, TAG, '[MW] isDestroyed #%{public}d = true', toplevelId);
    }
    return result;
  }

  // Wine xdg_toplevel.move 触发 → 标记等待 startMoving()
  // WineWindow 的 onMouse 在下一帧 MOVE 时检查此标记并调用 startMoving()
  shouldStartMoving(toplevelId: number): boolean {
    return this.entries.get(toplevelId)?.moveGrabPending ?? false;
  }

  getWindow(toplevelId: number): window.Window | undefined {
    return this.entries.get(toplevelId)?.win ?? undefined;
  }

  // -- loadContent 回调: 窗口装饰就绪后校准标题栏高度 + 重设标题 --
  onAfterLoadContent(toplevelId: number): void {
    const e = this.entries.get(toplevelId);
    const win = e?.win;
    if (!e || !win) return;

    // 1. 通过 API 获取标题栏高度 (必须在 loadContent 之后)
    //    getWindowDecorHeight 只返回标题栏文本区, 不含窗口边框。
    //    实际总装饰 = apiTitleBarH + decorBorderOffset
    try {
      const decorH = win.getWindowDecorHeight();
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight = %{public}d', toplevelId, decorH);
      if (decorH >= 37 && decorH <= 112) {
        this.apiTitleBarH = decorH;
        const newBarH = decorH + this.decorBorderOffset;
        if (Math.abs(newBarH - this.measuredTitleBarH) >= 2) {
          this.measuredTitleBarH = newBarH;
          hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: barH=%{public}d (API=%{public}d + offset=%{public}d)',
            toplevelId, newBarH, decorH, this.decorBorderOffset);
          const wd = e.lastResizeWineDim;
          if (wd) {
            this.applyEvent(toplevelId, 'resize', JSON.stringify({ w: wd[0], h: wd[1] }));
          }
        }
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight failed', toplevelId);
    }

    // 2.  方形窗口 (必须在 loadContent 之后, API 23 需要 FLOATING 模式)
    try {
      win.setWindowCornerRadius(0);
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius(0) done', toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius failed: %{public}s',
        toplevelId, JSON.stringify(e));
    }

    // 2. loadContent 会把窗口标题重置为默认值, 需重新应用 Wine 标题
    const title = e.title;
    if (title) {
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: reapply title "%{public}s"', toplevelId, title);
      win.setWindowTitle(title);
    }
  }

  // -- 兼容旧接口 --
  reapplyTitle(toplevelId: number): void {
    this.onAfterLoadContent(toplevelId);
  }

  // -- WineWindowAbility 在 onWindowStageCreate 中调用: 应用积压的事件 --
  flushPendingEvents(toplevelId: number): void {
    const e = this.entries.get(toplevelId);
    // popup 积压事件 (windowStage 就绪后补建)
    if (e?.stage && e?.win) {
      PopupWindowManager.getInstance().flushPending(toplevelId, e.stage, e.win, this.getEffectiveScale());
    }

    if (!e || e.events.length === 0) {
      hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d: none', toplevelId);
      return;
    }
    const pending = e.events;
    e.events = [];  // 先清后迭代: 等价旧 "先 delete 再迭代", 防 applyEvent 重入时丢事件

    hilog.info(DOMAIN, TAG, '[MW]  flushPendingEvents #%{public}d: %{public}d items', toplevelId, pending.length);
    for (let i = 0; i < pending.length; i++) {
      hilog.info(DOMAIN, TAG, '[MW]   flush[%{public}d/%{public}d] event=%{public}s data=%{public}s',
        i + 1, pending.length, pending[i][0], pending[i][1]);
      this.applyEvent(toplevelId, pending[i][0], pending[i][1]);
    }
    hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d done', toplevelId);
  }

  // -- 排队/直发样板收敛: win 就绪直发; 墓碑丢弃; 否则建档积压 --
  private queueOrApply(id: number, event: string, data: string): void {
    if (this.entries.get(id)?.win) {
      hilog.info(DOMAIN, TAG, '[MW] -> applyEvent directly #%{public}d %{public}s', id, event);
      this.applyEvent(id, event, data);
      return;
    }
    if (this.destroyed.has(id)) {
      // 墓碑拦截: unregister 之后迟到的事件不再为死 id 重建积压队列
      hilog.warn(DOMAIN, TAG, '[MW] drop %{public}s for destroyed #%{public}d', event, id);
      return;
    }
    const e = this.ensureEntry(id);
    if (event === 'title') {
      try {
        const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
        if (obj.title) e.title = obj.title as string;
      } catch (_) { /* applyEvent will report malformed title data if a window is created */ }
    }
    e.events.push([event, data]);
    hilog.info(DOMAIN, TAG, '[MW] -> queue pending #%{public}d %{public}s (window not ready, queue=%{public}d)',
      id, event, e.events.length);
  }

  // -- toplevel 事件分发 --
  private onToplevelEvent(id: number, event: string, data: string): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelEvent #%{public}d event=%{public}s hasWindow=%{public}s',
      id, event, this.entries.get(id)?.win ? 'yes' : 'no');

    switch (event) {
      case 'created':
        if (this.isDesktopMode) {
          hilog.info(DOMAIN, TAG, '[MW] Desktop mode: toplevel #%{public}d created, skipping Ability', id);
        } else {
          hilog.info(DOMAIN, TAG, '[MW] -> startWineWindowAbility(#%{public}d)', id);
          this.startWineWindowAbility(id, data);
        }
        break;
      case 'desktop_root':
        // C++ 识别到 explorer desktop → 桌面激活
        this.desktopRootId = id;
        this.desktopSurfaceReady = false;   // 新 root: renderer 尚未就绪
        this.desktopActive = true;          // 桌面激活信号 (WineEnvService 镜像)
        // PC 虚拟桌面: root 承载在独立 VirtualDesktopAbility 窗口, 自动打开
        // (用户决策: master 无悬浮窗中间态)。tablet/phone: DesktopAbility 全屏窗口。
        if (this.desktopSeparateWindow) {
          hilog.info(DOMAIN, TAG, '[MW] desktop_root #%{public}d → openDesktopWindow (separate)', id);
          this.openDesktopWindow();
        } else {
          hilog.info(DOMAIN, TAG, '[MW] desktop_root toplevel #%{public}d → start DesktopAbility', id);
          this.startDesktopAbility(id);
        }
        this.notify();
        break;
      case 'destroyed':
        hilog.info(DOMAIN, TAG, '[MW] -> onToplevelDestroyed(#%{public}d)', id);
        this.onToplevelDestroyed(id);
        break;
      case 'surface':
        // Desktop 模式: 非 root toplevel 事件跳过
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // C++ 回报 XComponent surface 的物理像素尺寸, 用于动态校准标题栏高度
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const sw = obj.w as number;
          const sh = obj.h as number;
          if (sw && sh && this.displayScale > 0) {
            // 桌面 root surface 有有效尺寸 = renderer 校准完成（WL_Plugin
            // [MW-Resize] renderer resized OK 的 JS 侧回报）→ 桌面真正可显示
            if (id === this.desktopRootId && !this.desktopSurfaceReady) {
              this.desktopSurfaceReady = true;
              this.notify();
            }
            hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: physical=%{public}dx%{public}d scale=%{public}s',
              id, sw, sh, this.displayScale.toFixed(2));

            //  动态校准 decorBorderOffset:
            //    totalBarH = lastResizeTargetH - surfacePhysicalH/scale
            //    decorBorderOffset = totalBarH - apiTitleBarH
            const entry = this.entries.get(id);
            const lastH = entry?.lastResizeTargetH;
            let calibrated = false;
            if (lastH) {
              const contentVPH = Math.round(sh / this.displayScale);
              const totalBarH = lastH - contentVPH;
              if (totalBarH > 0 && totalBarH < 200) {
                // 用 API 值计算边框偏移量
                const newOffset = totalBarH - this.apiTitleBarH;
                const oldOffset = this.decorBorderOffset;
                const oldBarH = this.measuredTitleBarH;
                // 前两轮直接用, 之后 EMA 平滑
                this.barCalibrationCount++;
                if (this.barCalibrationCount <= 2) {
                  this.decorBorderOffset = newOffset;
                } else {
                  this.decorBorderOffset = Math.round(oldOffset * 0.7 + newOffset * 0.3);
                }
                this.measuredTitleBarH = this.apiTitleBarH + this.decorBorderOffset;
                calibrated = (Math.abs(this.measuredTitleBarH - oldBarH) >= 1);
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: calibrate round#%{public}d totalBar=%{public}d offset=%{public}d->%{public}d barH=%{public}d (lastH=%{public}d contentVPH=%{public}d API=%{public}d)',
                  id, this.barCalibrationCount, totalBarH, oldOffset, this.decorBorderOffset,
                  this.measuredTitleBarH, lastH, contentVPH, this.apiTitleBarH);
              }
            }
            // 如果校准后 titleBarH 变了, 用保存的 wine 尺寸重新 resize
            if (calibrated && entry?.win) {
              const wd = entry.lastResizeWineDim;
              if (wd) {
                const newData = JSON.stringify({ w: wd[0], h: wd[1] });
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: re-resize with calibrated bar=%{public}d', id, this.measuredTitleBarH);
                this.applyEvent(id, 'resize', newData);
              }
            }
          }
        } catch (_) { /* ignore parse error */ }
        break;
      case 'move_start':
        this.ensureEntry(id).moveGrabPending = true;
        hilog.info(DOMAIN, TAG, '[MW] move_grab_start #%{public}d → pending startMoving()', id);
        break;
      case 'move_end': {
        const me = this.entries.get(id);
        if (me) me.moveGrabPending = false;
        hilog.info(DOMAIN, TAG, '[MW] move_grab_end #%{public}d', id);
        break;
      }
      case 'title':
      case 'resize':
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        if (ArgbWindowManager.getInstance().has(id)) {
          // ARGB 异型窗口: resize 调整子窗口尺寸; title 不需要 (子窗口无标题栏)。
          // 注意不能进积压队列 — ARGB 窗口永远不会注册 ability, 永不 flush
          if (event === 'resize') {
            ArgbWindowManager.getInstance().onResize(id, data, this.getEffectiveScale());
          }
          break;
        }
        this.queueOrApply(id, event, data);
        break;
      case 'minimized':
      case 'restored':
      case 'maximized':
      case 'unmaximized':
      case 'limits':
      case 'argb':
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // ARGB 异型窗口: 这些事件无意义且会泄漏进积压队列 (永不注册 ability)
        if (ArgbWindowManager.getInstance().has(id)) break;
        this.queueOrApply(id, event, data);
        break;
      case 'argb_created':
      case 'argb_move':
      case 'mask_dirty':
        // ARGB 异型窗口 (子窗口 + setWindowMask 路线, desktop 模式不涉及)
        if (this.isDesktopMode) return;
        if (!this.mainWindowStage) return;
        {
          const argb = ArgbWindowManager.getInstance();
          const s = this.getEffectiveScale();
          if (event === 'argb_created') {
            argb.onCreated(id, data, this.mainWindowStage!, s);
          } else if (event === 'argb_move') {
            argb.onMove(id, data, s);
          } else {
            argb.onMaskDirty(id, s);
          }
        }
        break;
      case 'popup_show':
      case 'popup_move':
      case 'popup_resize':
      case 'popup_hide':
        // PC 模式 popup 子窗口 (desktop 模式 C++ 不会发这些事件)
        // 父窗口可能是普通 ability 窗口, 也可能是 ARGB 异型子窗口 (回退)
        if (this.isDesktopMode) return;
        // 父窗口 Wine 尺寸传给 PopupWindowManager: 全屏游戏把渲染 surface 放
        // popup (popup 尺寸==模拟分辨率==父窗口尺寸), 需判定并拉伸到宿主屏幕;
        // 普通菜单 popup 不受影响。
        PopupWindowManager.getInstance().onPopupEvent(
          id, event, data,
          this.getWindowStage(id) ?? this.mainWindowStage,
          this.getWindow(id) ?? ArgbWindowManager.getInstance().getWindow(id),
          this.getEffectiveScale(),
          this.entries.get(id)?.lastResizeWineDim ?? null);
        break;
      case 'fullscreen':
      case 'unfullscreen':
        // 桌面模式: 全屏由 C++ 合成器直接处理 (缩放绘制 + 输入映射), ArkTS 无需动作
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // PC 模式: 游戏 xdg fullscreen → OHOS 沉浸式全屏 (盖住 dock/任务栏)。
        // 游戏画面由 popup (全屏拉伸) 承载, 主窗口最大化保证无系统 UI 露出。
        this.queueOrApply(id, event, data);
        break;
      default:
        hilog.warn(DOMAIN, TAG, '[MW] unknown event #%{public}d %{public}s', id, event);
        break;
    }
  }

  // -- apply 单个事件到 window.Window --
  private applyEvent(toplevelId: number, event: string, data: string): void {
    const e = this.entries.get(toplevelId);
    const win = e?.win;
    if (!e || !win) {
      hilog.warn(DOMAIN, TAG, '[MW] applyEvent #%{public}d: window NOT FOUND (unregistered?)', toplevelId);
      return;
    }

    switch (event) {
      case 'title':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          if (obj.title) {
            const titleStr = obj.title as string;
            e.title = titleStr;
            hilog.info(DOMAIN, TAG, '[MW] setWindowTitle #%{public}d -> "%{public}s"', toplevelId, titleStr);
            win.setWindowTitle(titleStr);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent title parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'resize':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const w = obj.w as number;
          const h = obj.h as number;
          if (w && h) {
            const barH = 0; //  标题栏已通过 setWindowDecorVisible(false) 隐藏
            const s = this.displayScale * this.scaleBoost;  // effective scale = 2.0
            const winW = Math.round(w * s);
            const winH = Math.round(h * s + barH);
            //  保存目标尺寸, 供 surface 事件校准 titleBarH + 重新 resize
            e.lastResizeTargetH = winH;
            e.lastResizeWineDim = [w, h];
            hilog.info(DOMAIN, TAG, '[MW-RESIZE] tl=%{public}d wine=%{public}dx%{public}d window=%{public}dx%{public}d scale=%{public}s',
              toplevelId, w, h, winW, winH, s.toFixed(1));
            win.resize(winW, winH);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent resize parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'limits':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const minW = obj.minW as number;
          const minH = obj.minH as number;
          const maxW = obj.maxW as number;
          const maxH = obj.maxH as number;
          const s = this.displayScale * this.scaleBoost;
          const limits: window.WindowLimits = {};
          if (minW > 0) limits.minWidth = Math.round(minW * s);
          if (minH > 0) limits.minHeight = Math.round(minH * s);
          if (maxW > 0) limits.maxWidth = Math.round(maxW * s);
          if (maxH > 0) limits.maxHeight = Math.round(maxH * s);
          const maxInFlight = e.everMaximized;
          hilog.info(DOMAIN, TAG, '[MW-LIMITS] tl=%{public}d wine=[%{public}d,%{public}d-%{public}d,%{public}d] → ohos=[%{public}d,%{public}d-%{public}d,%{public}d] maxInFlight=%{public}s',
            toplevelId, minW, minH, maxW, maxH,
            limits.minWidth ?? 0, limits.minHeight ?? 0,
            limits.maxWidth ?? 0, limits.maxHeight ?? 0,
            maxInFlight ? 'yes' : 'no');
          win.setWindowLimits(limits).then(() => {
            hilog.info(DOMAIN, TAG, '[MW-LIMITS] setWindowLimits OK tl=%{public}d', toplevelId);
          }).catch((e: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] setWindowLimits #%{public}d failed: %{public}s',
              toplevelId, JSON.stringify(e));
          });
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent limits parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'minimized':
        hilog.info(DOMAIN, TAG, '[MW] minimizeWindow #%{public}d', toplevelId);
        win.minimize().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] minimize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'restored':
        hilog.info(DOMAIN, TAG, '[MW] showWindow #%{public}d', toplevelId);
        win.showWindow().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] showWindow #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'maximized':
        {
          const alreadyInFlight = e.everMaximized;
          const preDim = e.lastResizeWineDim;
          hilog.info(DOMAIN, TAG, '[MW] maximize #%{public}d alreadyInFlight=%{public}s preMax=%{public}s',
            toplevelId, alreadyInFlight ? 'yes' : 'no', JSON.stringify(preDim));
          e.everMaximized = true;
          hilog.info(DOMAIN, TAG, '[MW] → win.maximize() START #%{public}d', toplevelId);
          // EXIT_IMMERSIVE: maximize() 不带参数时默认 ENTER_IMMERSIVE (进入全屏
          // 沉浸式), 窗口会铺满整个屏幕、盖住系统 dock/任务栏 (hover 底部才浮现),
          // 不符合桌面语义 (最大化应只占工作区)。EXIT_IMMERSIVE 让 OHOS 最大化到
          // 避让任务栏后的工作区, 随后 onSurfaceChanged 把实际尺寸回传 Wine。
          win.maximize(window.MaximizePresentation.EXIT_IMMERSIVE).then(() => {
            hilog.info(DOMAIN, TAG, '[MW] win.maximize() OK #%{public}d', toplevelId);
          }).catch((e: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] maximize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
          });
        }
        break;
      case 'unmaximized':
        hilog.info(DOMAIN, TAG, '[MW] recover #%{public}d (unmaximize)', toplevelId);
        win.recover().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] unmaximize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'fullscreen':
        // PC 模式游戏 xdg fullscreen: OHOS 沉浸式全屏 (铺满屏幕、盖住 dock)。
        // 用 ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER (API 14+): 进入沉浸
        // 全屏且鼠标悬停屏幕边缘热区时不唤出标题栏/Dock — 默认 ENTER_IMMERSIVE
        // 会 hover 底部浮现 dock, 抢走游戏边缘的鼠标事件 (FPS 转向/边缘 UI
        // 失效)。画面由 popup (全屏拉伸) 承载。
        try {
          hilog.info(DOMAIN, TAG, '[MW] fullscreen #%{public}d → win.maximize() (DISABLE_TITLE_AND_DOCK_HOVER)', toplevelId);
          win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER).then(() => {
            hilog.info(DOMAIN, TAG, '[MW] fullscreen OK #%{public}d', toplevelId);
          }).catch((err: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] fullscreen #%{public}d failed: %{public}s', toplevelId, JSON.stringify(err));
          });
        } catch (err) {
          hilog.warn(DOMAIN, TAG, '[MW] fullscreen #%{public}d exception: %{public}s', toplevelId, JSON.stringify(err));
        }
        break;
      case 'unfullscreen':
        // 退出沉浸式全屏, 恢复普通窗口; 尺寸由 C++ 侧 configure(preFs)
        // → resize 事件恢复 (tl_unset_fullscreen 用 preFs 尺寸)
        try {
          hilog.info(DOMAIN, TAG, '[MW] unfullscreen #%{public}d → win.maximize(EXIT_IMMERSIVE)', toplevelId);
          win.maximize(window.MaximizePresentation.EXIT_IMMERSIVE).then(() => {
            hilog.info(DOMAIN, TAG, '[MW] unfullscreen OK #%{public}d', toplevelId);
          }).catch((err: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] unfullscreen #%{public}d failed: %{public}s', toplevelId, JSON.stringify(err));
          });
        } catch (err) {
          hilog.warn(DOMAIN, TAG, '[MW] unfullscreen #%{public}d exception: %{public}s', toplevelId, JSON.stringify(err));
        }
        break;
      case 'argb':
        /*
         * layered/shaped (异型) 窗口标记, 仅记录状态 (后续子窗口方案使用)。
         * 已验证 2in1 主窗口无法做到透视:
         * - XComponent surface 无 alpha 通道 (EGL_ALPHA_SIZE=0, SET_FORMAT 无效)
         * - 窗口背景色 alpha 被系统钳制 ('#80FF0000' 显示为不透明红)
         * - setWindowMask 仅限子窗/全局悬浮窗 (主窗口 1300004)
         * 故不做窗口背景色设置 (无效操作)。
         */
        try {
          const obj: Record<string, number> = JSON.parse(data) as Record<string, number>;
          const isArgb = obj.argb === 1;
          hilog.info(DOMAIN, TAG, '[MW] argb state #%{public}d = %{public}s (record only)',
            toplevelId, isArgb ? 'ARGB' : 'XRGB');
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent argb parse error: %{public}s', JSON.stringify(e));
        }
        break;
    }
  }

  // -- startAbility 启动 multiton 实例 --
  private startWineWindowAbility(id: number, data: string): void {
    const e = this.ensureEntry(id);
    if (e.starting || e.ctx) {
      hilog.warn(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: already in flight/alive, skip', id);
      return;
    }
    e.starting = true;
    hilog.info(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: starting', id);

    // 解析初始参数
    let title = '';
    let initW = 0;
    let initH = 0;
    try {
      const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
      if (obj.title) title = obj.title as string;
      // created 事件的 Wine 内容尺寸 (w/h): 传入 Ability 设置窗口初始尺寸,
      // 避免 OHOS 默认窗口尺寸 → onSurfaceChanged 反馈 → 窗口模式游戏
      // 接受 configure resize → 内容不缩放 (画面缩在左上角 + 反馈环)
      initW = Number(obj.w) || 0;
      initH = Number(obj.h) || 0;
    } catch (_) { /* use defaults */ }

    hilog.info(DOMAIN, TAG, '[MW]  startAbility(WineWindowAbility) #%{public}d title="%{public}s"',
      id, title);

    if (!this.uiAbilityContext) {
      hilog.error(DOMAIN, TAG, '[MW] FATAL: no uiAbilityContext, cannot start ability');
      e.starting = false;
      return;
    }

    const want: Want = {
      bundleName: 'app.hackeris.winehua',
      abilityName: 'WineWindowAbility',
      parameters: { toplevelId: id, title: title, w: initW, h: initH }
    };

    hilog.info(DOMAIN, TAG, '[MW] startAbility want: bundle=%{public}s ability=%{public}s params=%{public}s',
      want.bundleName ?? '', want.abilityName ?? '', JSON.stringify(want.parameters));

    this.uiAbilityContext.startAbility(want).then(() => {
      hilog.info(DOMAIN, TAG, '[MW] OK startAbility #%{public}d resolved (AMS accepted)', id);
      // entry 保留 (等 WWA 三步回填), 仅清飞行标记 —
      // 消除旧实现 "pendingStarts 已删但 abilityContexts 未 set" 的判定间隙
      e.starting = false;
    }).catch((err: Error) => {
      hilog.error(DOMAIN, TAG, '[MW] ERR startAbility #%{public}d REJECTED: %{public}s',
        id, JSON.stringify(err));
      // 启动失败: 整体清档 (含已积压事件, 旧实现会遗留 pendingEvents 永不 flush)
      this.entries.delete(id);
      // 通知 Wine 窗口创建失败
      hilog.info(DOMAIN, TAG, '[MW] -> sendToplevelClose #%{public}d (fail recovery)', id);
      testNapi.sendToplevelClose(id);
    });
  }

  // -- Wine toplevel destroyed -> terminateSelf --
  private onToplevelDestroyed(id: number): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d', id);
    this.destroyed.add(id);

    // Desktop 模式: root 销毁 → 关闭桌面窗口, 回到主页面
    if (this.isDesktopMode && id === this.desktopRootId) {
      hilog.info(DOMAIN, TAG, '[MW] desktop root #%{public}d destroyed', id);
      this.desktopRootId = 0;
      this.desktopSurfaceReady = false;
      this.desktopActive = false;
      if (this.desktopSeparateWindow) {
        // PC 虚拟桌面: 关闭独立 VirtualDesktopAbility 窗口 (root 销毁=会话结束)
        if (this.desktopWinCtx) {
          const ctx = this.desktopWinCtx;
          this.desktopWinCtx = null;
          hilog.info(DOMAIN, TAG, '[MW] → terminateSelf(VirtualDesktopAbility)');
          ctx.terminateSelf().catch((err: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf desktop window: %{public}s', JSON.stringify(err));
          });
        }
        this.desktopWindowActive = false;
        this.desktopWinStarting = false;
      } else {
        // tablet/phone: 关闭 DesktopAbility (现状)
        if (this.desktopAbilityCtx) {
          this.desktopAbilityCtx.terminateSelf().catch((err: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf DesktopAbility: %{public}s', JSON.stringify(err));
          });
          this.desktopAbilityCtx = null;
        }
        this.desktopAbilityRunning = false;
      }
      this.notify();
      return;
    }
    // Desktop 模式: 非 root toplevel 销毁, C++ 自动停止合成
    if (this.isDesktopMode) {
      this.unregisterAbility(id);
      return;
    }

    // PC: ARGB 异型子窗口销毁 (级联销毁其 popup)
    if (ArgbWindowManager.getInstance().has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d: destroy ARGB subwindow', id);
      ArgbWindowManager.getInstance().destroy(id);
      testNapi.destroyToplevel(id);
      this.unregisterAbility(id);
      return;
    }

    // PC: 原逻辑 (entry 从 created 存活到 unregister, 覆盖旧
    // "startAbility 已 resolve 但 WWA.onCreate 未回填" 的判定间隙)
    if (!this.entries.has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d: already cleaned up by ability, skip', id);
      return;
    }
    hilog.info(DOMAIN, TAG, '[MW] -> destroyToplevel #%{public}d', id);
    testNapi.destroyToplevel(id);
    const ctx = this.entries.get(id)?.ctx;
    if (ctx) {
      hilog.info(DOMAIN, TAG, '[MW] -> terminateSelf #%{public}d', id);
      ctx.terminateSelf().catch((err: Error) => {
        hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf #%{public}d failed: %{public}s',
          id, JSON.stringify(err));
      });
    }
    this.unregisterAbility(id);
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d done', id);
  }

  // Desktop 模式: 打开 DesktopAbility
  private startDesktopAbility(rootId: number): void {
    // singleton 模式: 重复 startAbility 不会创建新实例, 只会触发 onNewWant
    if (!this.desktopAbilityRunning) {
      this.desktopAbilityRunning = true;
    }
    try {
      const want: Want = {
        bundleName: 'app.hackeris.winehua',
        abilityName: 'DesktopAbility',
        parameters: { toplevelId: rootId }
      };
      this.uiAbilityContext!.startAbility(want).catch((err: Error) => {
        hilog.error(DOMAIN, TAG, '[MW] startAbility(DesktopAbility) failed: %{public}s',
          JSON.stringify(err));
      });
      hilog.info(DOMAIN, TAG, '[MW] startAbility(DesktopAbility) #%{public}d OK', rootId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MW] startDesktopAbility error: %{public}s', JSON.stringify(e));
    }
  }

  restoreDesktopIfRunning(): void {
    if (!this.isDesktopMode || this.desktopRootId <= 0) return;
    // A new Want foregrounds EntryAbility even though the desktop compositor
    // remains alive. Re-starting the singleton moves that existing desktop
    // back to the foreground without recreating or stopping the Wine session.
    hilog.info(DOMAIN, TAG, '[MW] refocusing DesktopAbility root=%{public}d', this.desktopRootId);
    this.startDesktopAbility(this.desktopRootId);
  }

  getDesktopRootId(): number { return this.desktopRootId; }

  // -- PC 虚拟桌面独立窗口 (VirtualDesktopAbility) --

  isSeparateDesktopWindow(): boolean {
    return this.desktopSeparateWindow;
  }

  isDesktopWindowActive(): boolean {
    return this.desktopWindowActive;
  }

  // VDA onCreate 调: 注册 ctx + 窗口存活标记 (对齐 tablet registerDesktopAbility)
  registerDesktopWindow(ctx: common.UIAbilityContext): void {
    this.desktopWinCtx = ctx;
    this.desktopWindowActive = true;
    this.notify();
    // VDA 窗口存活 = 桌面进入"完整"形态: 若 wine 已处于相对模式 (pointerLocked),
    // 从"悬浮禁止隐藏"重算为"完整允许隐藏" → 立即藏 (否则进完整桌面光标残留)。
    this.applyPointerVisibility();
  }

  // VDA 关窗 (WINDOW_DESTROYED / onDestroy) 调: 引擎会话后台继续, 仅清窗口
  // 标记 (对齐 tablet unregisterDesktopAbility 语义 + notify 让 Index
  // 「打开桌面」按钮复现 — 与 tablet onDestroy 杀 root 语义不同: PC 虚拟
  // 桌面要求"关窗后台运行")
  markDesktopWindowClosed(): void {
    this.desktopWinCtx = null;
    this.desktopWindowActive = false;
    this.notify();
    // 关窗回到悬浮窗形态: 即使 wine 相对模式仍存活 (pointerLocked) 也禁止隐藏
    // → 重算恢复系统光标, 避免 VDA 关窗后光标残留隐藏。
    this.applyPointerVisibility();
  }

  // 主动打开虚拟桌面独立窗口 (Index「打开桌面」按钮; desktop_root 自动调用)
  openDesktopWindow(): void {
    this.startVirtualDesktopAbility();
  }

  // -- startAbility 拉起独立桌面窗口 (root 单例, 防重: starting/active 双守卫) --
  private startVirtualDesktopAbility(): void {
    if (this.desktopWinStarting || this.desktopWindowActive) {
      hilog.info(DOMAIN, TAG, '[MW] desktop window already in flight/alive, skip');
      return;
    }
    if (!this.uiAbilityContext) {
      hilog.error(DOMAIN, TAG, '[MW] FATAL: no uiAbilityContext, cannot start desktop window');
      return;
    }
    this.desktopWinStarting = true;
    hilog.info(DOMAIN, TAG, '[MW] startAbility(VirtualDesktopAbility)');
    const want: Want = {
      bundleName: 'app.hackeris.winehua',
      abilityName: 'VirtualDesktopAbility'
    };
    this.uiAbilityContext.startAbility(want).then(() => {
      hilog.info(DOMAIN, TAG, '[MW] OK startAbility(VirtualDesktopAbility) resolved');
      this.desktopWinStarting = false;
    }).catch((err: Error) => {
      hilog.error(DOMAIN, TAG, '[MW] ERR startAbility(VirtualDesktopAbility) REJECTED: %{public}s',
        JSON.stringify(err));
      this.desktopWinStarting = false;
    });
  }

  // 沉浸全屏还原: 经 VDA ctx 取主窗口 recover() 回普通窗口 (唯一还原入口
  // 是 VirtualDesktop 页面的悬浮「退出全屏」按钮 — DISABLE_HOVER 禁用了
  // 系统 hover 还原)
  async recoverDesktopWindow(): Promise<void> {
    if (!this.desktopWinCtx) {
      hilog.warn(DOMAIN, TAG, '[MW] recoverDesktopWindow: no desktop ctx');
      return;
    }
    try {
      const win = await window.getLastWindow(this.desktopWinCtx);
      if (win) {
        await win.recover();
        hilog.info(DOMAIN, TAG, '[MW] recoverDesktopWindow OK');
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] recoverDesktopWindow failed: %{public}s', JSON.stringify(e));
    }
  }

  // 启动器可见性写回 (Index 调用; PC 虚拟桌面模式 Index 不挂 DesktopLayer,
  // 该值归独立窗口所有 — VirtualDesktop 页面保持默认 true 跳过 root resize)
  setDesktopLauncherVisible(visible: boolean): void {
    this.desktopLauncherVisible = visible;
    // Pad 悬浮↔完整桌面切换: 光标隐藏意图可能因形态翻转而改变 (悬浮=禁止,
    // 完整=允许), 但 wine 的相对指针对象未必销毁 (无新锁定回调) → 重算一次
    this.applyPointerVisibility();
  }

  // 光标隐藏门禁判据 (fromToplevel = 触发相对模式的约束 surface 所属 toplevel):
  //   融合模式   → 允许 (每个 app 独立窗口, setPointerVisible 按"当前窗口"
  //                作用, 只有发起相对模式的应用窗口隐藏, 天然满足"其它窗口
  //                不隐藏") — 不拦。
  //   桌面模式   → 悬浮窗形态禁止; 完整桌面形态仅"非桌面 root surface"允许。
  //                PC: 悬浮 = Index 小窗预览 (VDA 未存活 desktopWindowActive=false);
  //                    完整桌面 = VDA 独立窗口存活 (desktopWindowActive=true)
  //                Pad: 未激活 = desktopActive=false (desktop root 未出现);
  //                    完整桌面 = desktopActive=true (desktop_root 事件)
  //               完整桌面下 fromToplevel == desktopRootId → 桌面 shell 自身藏
  //               光标 (启动瞬时) → 禁止; 子窗口 (真实游戏/程序) → 允许。
  private canHideCursor(fromToplevel: number): boolean {
    if (!this.isDesktopMode) return true;
    if (this.desktopSeparateWindow) {
      if (!this.desktopWindowActive) return false;
    } else if (!this.desktopActive) {
      // Pad 门禁: 桌面未激活 (desktop root 未出现) 禁止隐藏。旧实现读
      // desktopLauncherVisible (恒 true 死字段 — 全局无写入方), 致 Pad 上
      // "光标锁定但未隐藏" (C++ 冻结生效、ArkTS 恒拒绝) — 已改。
      return false;
    }
    return fromToplevel !== this.desktopRootId;
  }

  // 统一重算并应用光标显隐: shouldHide = pointerLocked × canHideCursor()。
  // 与 effectiveHidden 比对, 变了才调 setPointerVisible — 保持"相对模式 ×
  // 展示形态"始终一致, 悬浮时被抑制的隐藏不会因切换失同步 (要么漏藏要么残留)。
  private applyPointerVisibility(): void {
    const canHide = this.canHideCursor(this.pointerLockToplevel);
    const shouldHide = this.pointerLocked && canHide;
    // 每次调用都打决策 (含 no-op), 实测才能确认门禁走向 (锁定/解锁、
    // 悬浮↔完整切换均会触发, 频率低不刷屏)
    hilog.info(DOMAIN, TAG,
      '[MW]PtrVis → %{public}s (locked=%{public}s canHide=%{public}s tl=%{public}d mode=%{public}s sep=%{public}s vda=%{public}s da=%{public}s)',
      shouldHide ? 'HIDE' : 'SHOW',
      String(this.pointerLocked), String(canHide), this.pointerLockToplevel,
      String(this.isDesktopMode), String(this.desktopSeparateWindow),
      String(this.desktopWindowActive), String(this.desktopActive));
    if (shouldHide === this.effectiveHidden) return;
    this.effectiveHidden = shouldHide;
    pointer.setPointerVisible(!shouldHide).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, '[MW] setPointerVisible failed: %{public}s', JSON.stringify(e));
    });
  }

  // DesktopAbility 注册/注销其 UIAbilityContext, 用于 root 销毁时 terminateSelf
  registerDesktopAbility(ctx: common.UIAbilityContext): void {
    hilog.info(DOMAIN, TAG, '[MW] registerDesktopAbility');
    this.desktopAbilityCtx = ctx;
  }

  unregisterDesktopAbility(): void {
    hilog.info(DOMAIN, TAG, '[MW] unregisterDesktopAbility');
    this.desktopAbilityCtx = null;
    this.desktopAbilityRunning = false;
  }
}

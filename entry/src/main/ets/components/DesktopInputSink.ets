import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import displayRouteNapi from 'libdisplay_route_napi.so';
import { DisplayRouteService } from '../service/DisplayRouteService';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputDeviceMapper } from '../service/InputDeviceMapper';
import { DesktopPointerService, desktopKeyboardTarget } from '../service/DesktopPointerService';

// 桌面输入 seam (PC 虚拟桌面承载统一, 2026-10-17): 按功能语义把「桌面输入」
// 拆成 共性判定 + 路线差异注入 两层, 消灭 DesktopLayer/DesktopWindow 双承载
// 时代的状态机副本分裂。
//
// ── 共性 (DesktopLayer 手势状态机, 单份) ──────────────────────────────
//   触控: PENDING → 拖拽 / 长按右键 / 双击 / 双指滚轮 判定; 取消不点击;
//         双击第二击按首击坐标重放 (wine ±1px 双击判据, #137)。
//   物理鼠标: 左键真实时序直通 + mouseLeftActive 抑制合成 touch; 右/中键
//         按下捕获-释放配对 (捕获句柄由状态机持有; x11 beginStroke 恒返回
//         1, 捕获门恒过 — 与 x11 无捕获语义自洽)。
//   触控板: 相对位移虚拟光标 + 点按/长按/拖拽/双指滚动。
//   键盘: OH2EVDEV 映射 + 回车补发兜底。
// ── 差异 (本文件两实现) ───────────────────────────────────────────────
//   出图绑定:  wayland = createRenderer/resizeRenderer/destroyRenderer(rootId,
//              created 拍建, rootId 缓存配对销毁);
//              x11 = startDisplayRouteChain (changed 首拍; 链已起时 native
//              晚到槽挂载, D8) / stopDisplayRouteChain (销毁拍)。分辨率解耦:
//              窗口只是显示口, 不随画布 resize output — letterbox 呈现与
//              wayland VDA 同语义。
//   命中/激活: wayland = findToplevelAt + raiseToplevel (合成器 tl);
//              x11 = 无 tl 概念, Xwayland 侧按 motion 命中即激活 (D36),
//              beginStroke 恒返回 1 → 状态机 skip 门恒过 (画布全幅皆桌面)。
//   指针注入:  wayland = sendPointerEvent(tl, action, 物理坐标);
//              x11 = injectDisplayRouteMotion/Button (surface 物理尺寸
//              归一化 0..1), 按下前先 motion 锚定位置。
//   键盘/滚轮: wayland = sendKeyEvent(rootId)/sendScrollEvent(tl,...);
//              x11 = injectDisplayRouteKey/Axis (一次轴事件 = 1 步)。
//
// wayland 实现与 DesktopLayer 原内联调用逐字等价 (隔离红线); x11 实现的
// 语义从 DesktopWindow x11 分支 (D15 手势层 incumbent, 设备验证过) 搬迁。

export interface DesktopInputSink {
  /** XComponent created 拍: 建出图 (wayland renderer; x11 仅登记画布) */
  surfaceCreated(surfaceId: string): void;
  /** XComponent changed 拍 (每拍调用): 首拍绑定出图, 后续拍更新尺寸。
   *  lw/lh = 逻辑尺寸 (物理/呈现倍率, 链起参数); pw/ph = 物理尺寸。 */
  surfaceChanged(surfaceId: string, lw: number, lh: number,
    pw: number, ph: number): void;
  /** XComponent destroyed 拍: wayland 停 renderer (rootId 缓存配对);
   *  x11 只解绑呈现面 — 链归 applyRoute 单点, 关窗后台运行语义不停链 */
  surfaceDestroyed(surfaceId?: string): void;
  /** 键盘/滚轮注入门 (wayland = root 已映射; x11 = 链已绑定) */
  inputReady(): boolean;

  /** 触控/鼠标 Down: 目标句柄 (0 = 无目标, 状态机丢弃本击)。
   *  wayland = findToplevelAt + raiseToplevel; x11 = motion 锚定, 恒 1 */
  beginStroke(px: number, py: number): number;
  /** 纯命中查询 (不激活; 触控板 hover/滚轮定位用; x11 恒 1) */
  hitTest(px: number, py: number): number;
  /** 决策时点的目标身份 (#139 双击同一性)。wayland = 点位活命中;
   *  x11 = 最近 drain motion 的 X window id (判据见 display_input.h) */
  targetIdentityAt(px: number, py: number): number;
  /** PENDING 未过阈值的移动 (wayland 不发; x11 motion 直通 hover) */
  strokeHover(px: number, py: number): void;
  /** 拖拽随动移动 */
  strokeMove(handle: number, px: number, py: number): void;
  /** 按下 (hardware = 物理鼠标真实时序, 仅 wayland 消费) */
  pressAt(handle: number, px: number, py: number, btn: number, hardware: boolean): void;
  /** 抬起 */
  releaseAt(handle: number, px: number, py: number, btn: number): void;
  /** hover 移动 (物理鼠标; handle = 按下捕获句柄, 0 = 无捕获) */
  hoverMove(handle: number, px: number, py: number,
    rawDx: number, rawDy: number, deviceId: number): void;
  /** 滚轮目标句柄 (双指中点/物理滚轮位置; x11 恒 1) */
  beginScroll(px: number, py: number): number;
  /** 滚轮: which 0=纵向 1=横向, steps = 离散步 (正=向下), stepPx = 步长
   *  (物理像素; 双指手势恒 SCROLL_STEP_PX, 物理滚轮 = 轴事件 scrollStep) */
  scrollAt(handle: number, which: number, steps: number, stepPx: number,
    px: number, py: number): void;
  /** 键盘 (evdev 键码) */
  keyEvent(code: number, pressed: boolean, target?: number): void;
}

// ── wayland: 合成器 toplevel API (DesktopLayer 原内联调用, 逐字等价) ──
class WaylandSink implements DesktopInputSink {
  // 缓存 created 拍的 rootId 配对销毁 (原 DesktopController.lastRootId 语义):
  // destroyed 拍 WWM 的 desktopRootId 可能已被清零 (桌面退出先清 id 再卸载
  // 组件), 用缓存值才能配对销毁 renderer — 否则旧 root 的 renderer 泄漏,
  // 热重启后新旧 renderer 抢同一 root 的合成帧 (画面「滞后一拍」)
  private lastRootId: number = 0;
  // root resize 节流 (main-ui 悬浮窗语义经 seam 保留): onSurfaceChanged 连续
  // 触发期间 (悬浮窗放大, 沉浸生效前后两拍波动 2584x1644→1828) 只记最新
  // 逻辑尺寸, 停止 150ms 后一次性 notifyToplevelResize — 中间值不进 Wine
  // 桌面, 避免 Wine 帧与 surface 中间值不匹配的左右黑边
  private pendingRootLw: number = 0;
  private pendingRootLh: number = 0;
  private rootResizeTimer: number = -1;
  private static readonly ROOT_RESIZE_DEBOUNCE_MS: number = 150;
  private surfaceId: bigint = 0n;

  surfaceCreated(surfaceId: string): void {
    this.surfaceId = BigInt(surfaceId);
    this.lastRootId = WineWindowManager.getInstance().getDesktopRootId();
    if (this.lastRootId > 0) {
      try { testNapi.createRenderer(this.lastRootId, BigInt(surfaceId)); } catch (_) {}
    }
  }

  surfaceChanged(surfaceId: string, lw: number, lh: number,
    pw: number, ph: number): void {
    // wayland 无首拍/后续之分: 每拍 resize renderer (原 onSurfaceChanged 语义)
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId <= 0) return;
    try {
      testNapi.resizeRenderer(rootId, pw, ph, BigInt(surfaceId));
      // 启动器可见 (窗口退出全屏) 时 surface 高度随系统栏波动 (1840→1683):
      // 跳过 root resize 通知, 否则中间态高度会缩小 Wine 桌面, 且退出后残留
      // 进 output, 热重启桌面高度出错 (上下被裁)
      if (WineWindowManager.getInstance().desktopLauncherVisible ||
          WineWindowManager.getInstance().hasFixedDesktopResolution()) {
        this.cancelDeferredRootResize();
        return;
      }
      this.deferRootResize(rootId, lw, lh);
    } catch (_) {}
  }

  // 挂起 root resize 节流: 连续变化只记最新值, 静止后一次性下发 (rootId 用
  // 挂起拍值 — flush 时 root 可能已重建, 用当前值会notify到错误会话)
  private deferRootResize(rootId: number, lw: number, lh: number): void {
    this.pendingRootLw = lw;
    this.pendingRootLh = lh;
    if (this.rootResizeTimer >= 0) {
      clearTimeout(this.rootResizeTimer);
    }
    this.rootResizeTimer = setTimeout(() => {
      this.rootResizeTimer = -1;
      if (rootId > 0) {
        testNapi.notifyToplevelResize(rootId, this.pendingRootLw, this.pendingRootLh);
      }
    }, WaylandSink.ROOT_RESIZE_DEBOUNCE_MS);
  }

  private cancelDeferredRootResize(): void {
    if (this.rootResizeTimer >= 0) {
      clearTimeout(this.rootResizeTimer);
      this.rootResizeTimer = -1;
    }
  }

  surfaceDestroyed(surfaceId?: string): void {
    const cached = this.lastRootId;
    const surface = surfaceId ? BigInt(surfaceId) : this.surfaceId;
    if (surface === this.surfaceId) this.cancelDeferredRootResize();
    if (surface === this.surfaceId) { this.lastRootId = 0; this.surfaceId = 0n; }
    if (cached > 0) {
      try { testNapi.destroyRenderer(cached, surface); } catch (_) {}
    }
  }

  inputReady(): boolean {
    return WineWindowManager.getInstance().getDesktopRootId() > 0;
  }

  beginStroke(px: number, py: number): number {
    const tl = testNapi.findToplevelAt(px, py) as number;
    if (tl <= 0) return 0;
    testNapi.raiseToplevel(tl);
    return tl;
  }

  hitTest(px: number, py: number): number {
    return testNapi.findToplevelAt(px, py) as number;
  }

  targetIdentityAt(px: number, py: number): number {
    return testNapi.findToplevelAt(px, py) as number;
  }

  strokeHover(_px: number, _py: number): void {
    // wayland PENDING 未过阈值不发任何事件 (等 Up 判定) — 原行为
  }

  strokeMove(handle: number, px: number, py: number): void {
    testNapi.sendPointerEvent(handle, MouseAction.Move, px, py, 0);
  }

  pressAt(handle: number, px: number, py: number, btn: number, hardware: boolean): void {
    if (hardware) {
      testNapi.sendPointerEvent(handle, MouseAction.Press, px, py, btn, 0, 0, true);
    } else {
      testNapi.sendPointerEvent(handle, MouseAction.Press, px, py, btn);
    }
  }

  releaseAt(handle: number, px: number, py: number, btn: number): void {
    testNapi.sendPointerEvent(handle, MouseAction.Release, px, py, btn);
  }

  hoverMove(handle: number, px: number, py: number,
    rawDx: number, rawDy: number, deviceId: number): void {
    // 按键按住拖动: 发给按下捕获的 tl; 否则按当前位置找目标
    // (跨窗口 enter/leave 由注入层 ACT_MOVE 的焦点判定处理)
    let tl = handle;
    if (tl <= 0) {
      tl = testNapi.findToplevelAt(px, py) as number;
      if (tl <= 0) return;
    }
    // rawDelta 按设备校准 (与 WineWindow 同规则: 鼠标 2.5 / 触控板 0.75)
    // — 收口 ArkTS 时此处漏乘, 桌面模式相对模式增量只有融合窗口的
    // 1/2.5, 同一鼠标两种手感; 绝对模式有差分自标定, 不受影响
    let dx = rawDx;
    let dy = rawDy;
    if (deviceId > 0) {
      const s = InputDeviceMapper.getInstance().getScale(deviceId);
      dx *= s;
      dy *= s;
    }
    testNapi.sendPointerEvent(tl, MouseAction.Move, px, py, 0, dx, dy);
  }

  beginScroll(px: number, py: number): number {
    return (testNapi.findToplevelAt(px, py) as number) ?? 0;
  }

  scrollAt(handle: number, which: number, steps: number, stepPx: number,
    px: number, py: number): void {
    testNapi.sendScrollEvent(handle, which, steps, stepPx, px, py);
  }

  keyEvent(code: number, pressed: boolean, target?: number): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId <= 0) return;
    testNapi.sendKeyEvent(target && target > 0 ? target : desktopKeyboardTarget(rootId), code, pressed);
  }
}

// ── x11: X 桥直连注入 (语义从 DesktopWindow x11 分支搬迁) ──────────────
class X11Sink implements DesktopInputSink {
  private static readonly TAG = 'X11SINK';
  // 归一化基准 = 活 surface 物理尺寸 (与 present fit / display_input 逆映射
  // 同基准, 见 ohos_egl_import.c wl_ohos_egl_canvas_size); 0 = 未绑定, 丢弃
  private surfW: number = 0;
  private surfH: number = 0;
  private bound: boolean = false;

  surfaceCreated(surfaceId: string): void {
    // D5: 无条件登记画布 surface (SmokeRunner 画布优先启动链消费); w/h 待
    // changed 首拍回填 (与 DesktopWindow x11 incumbent 同时机)
    WineWindowManager.getInstance().registerDesktopCanvas(surfaceId, 0, 0);
  }

  surfaceChanged(surfaceId: string, lw: number, lh: number,
    pw: number, ph: number): void {
    WineWindowManager.getInstance().registerDesktopCanvas(surfaceId, pw, ph);
    if (this.bound) {
      // 分辨率解耦 (与 wayland VDA 同语义): 不 resizeDisplayRouteOutput,
      // 只更新输入归一化基准 — 呈现 letterbox 由 present 层按活 surface fit
      if (this.surfW != pw || this.surfH != ph) {
        // 尺寸真变 (最大化/拖拽/旋转): 通知链强帧 —— wayland 臂的
        // resizeRenderer 事件链对应物。系统会随窗口重写 surface buffer,
        // 静态桌面 damage 断流, 不通知则旧 buffer 被非等比拉伸
        // (egl_renderer.h:84-92 同款事故)。
        this.surfW = pw;
        this.surfH = ph;
        try { displayRouteNapi.displayRouteCanvasResized(); } catch (_) {}
      }
      return;
    }
    // 首拍绑定出图: 第 5 参 true = 画布 EGL swap 呈现 (该承载窗对手工
    // FlushBuffer 冻结, 语义见 ohos_output.h)。链已起 (applyRoute 早启)
    // 时 native 走晚到槽挂载 (D8); 未起时本调用即链首启 (first-caller,
    // smoke 画布优先同款)。
    try {
      displayRouteNapi.startDisplayRouteChain(BigInt(surfaceId), false,
        lw, lh, true,
        WineWindowManager.getInstance().shouldUseMultiwindow());
      this.bound = true;
      this.surfW = pw;
      this.surfH = ph;
    } catch (e) {
      hilog.error(0x0000, X11Sink.TAG, 'chain start failed: %{public}s', String(e));
    }
  }

  surfaceDestroyed(): void {
    WineWindowManager.getInstance().unregisterDesktopCanvas();
    // 只解绑呈现面, 不停链: PC 虚拟桌面语义 = 关窗后台运行 (引擎会话继续),
    // 链 = 整个 X 会话 (合成器 + Xwayland), 停链 = 会话死 — 与 Pad
    // DesktopWindow「surface 没了 ⇒ 合成器停机」的退出桌面语义不同。
    // 链生命周期归 applyRoute 单点 (切路线拆链 / 引擎重启沿用)。重开窗口
    // 后新 surface 经 changed 首拍再次晚到挂载 (D8), 画面自动恢复。
    this.bound = false;
  }

  inputReady(): boolean {
    return this.bound;
  }

  // 归一化坐标 (0..1); 未绑定时 surfW/H = 0 → 返回 -1, 调用方丢弃 (除零防护)
  private nx(px: number): number {
    return this.surfW > 0 ? px / this.surfW : -1;
  }

  private ny(py: number): number {
    return this.surfH > 0 ? py / this.surfH : -1;
  }

  // motion 注入 (phase=1); 未绑定丢弃并告警 — 对齐 DesktopWindow 的
  // 零尺寸防护 (x11W/H <= 0 drop)
  private motion(px: number, py: number): boolean {
    const x = this.nx(px);
    const y = this.ny(py);
    if (x < 0 || y < 0) {
      hilog.warn(0x0000, X11Sink.TAG, 'dropped (surface %{public}dx%{public}d)',
        this.surfW, this.surfH);
      return false;
    }
    displayRouteNapi.injectDisplayRouteMotion(x, y, 1);
    return true;
  }

  beginStroke(px: number, py: number): number {
    // Down 只发 motion (悬停激活, X 路线焦点按命中走); button 缓发等 Up 判定。
    // 恒返回 1: 状态机 skip 门 (句柄<=0) 恒过, 无「桌外」概念。
    this.motion(px, py);
    return 1;
  }

  hitTest(_px: number, _py: number): number {
    return 1;
  }

  targetIdentityAt(_px: number, _py: number): number {
    // 决策时点 (Up) 距本 tap Down 的 anchor motion 已隔整个手势时长,
    // 队列 drain (ms 级) 早已完成 → 活槽 = 本 tap 命中的 X window id。
    // 点位参数忽略: 槽随最后一次 motion 走 (PENDING 抖动 hover 与 Down
    // 点同窗, 30px 阈值内跨窗属边缘情形, 与 wine ±2px 判据同量级)。
    return displayRouteNapi.getLastPtrWindowId() as number;
  }

  strokeHover(px: number, py: number): void {
    // PENDING 未过阈值 motion 直通 (hover 菜单/高亮) — x11 incumbent 行为
    this.motion(px, py);
  }

  strokeMove(_handle: number, px: number, py: number): void {
    this.motion(px, py);
  }

  pressAt(_handle: number, px: number, py: number, btn: number,
    _hardware: boolean): void {
    // 按下前先 motion 锚定位置 (单击语义 = 点击按下点; 双击重放时调用方
    // 传首击坐标) — x11 incumbent 同款。
    // btn 口径: 状态机产出 = mapMouseButton() 返回值 = **evdev 码**
    // (BTN_LEFT 0x110..., 见 MouseMap.ets) — inject_button 的 wire 语义
    // 正是 evdev, 直接透传。⚠ 曾在此按「1/2/4 口径」再映射一次, 0x110
    // 不匹配任何分支 → 注入 evdev=0x0, wine 收不到任何按钮事件
    // (2026-10-18 实测「点击无反应」的直接根因; wayland 分支原样消费
    // mapMouseButton 值, 唯 x11 错配)。
    if (btn <= 0) return;
    if (!this.motion(px, py)) return;
    displayRouteNapi.injectDisplayRouteButton(btn, true);
  }

  releaseAt(_handle: number, _px: number, _py: number, btn: number): void {
    // 不发 leave (phase 2): leave 会打断 wine 菜单跟踪 (2026-10-06 实测),
    // 下次 Down 的 motion 重新锚定
    if (btn <= 0) return;
    displayRouteNapi.injectDisplayRouteButton(btn, false);
  }

  hoverMove(_handle: number, px: number, py: number,
    _rawDx: number, _rawDy: number, _deviceId: number): void {
    // 无 rawDelta/设备校准概念 (归一化绝对坐标, 桥内差分); 手势进行中的
    // 双发守卫由调用方骨架负责 (与 wayland 同一门)
    this.motion(px, py);
  }

  beginScroll(_px: number, _py: number): number {
    return 1;
  }

  scrollAt(_handle: number, which: number, steps: number, _stepPx: number,
    _px: number, _py: number): void {
    // OHOS 轴值符号与 discrete 同约定 (正=向下), 一次事件 = 1 步;
    // 方向判据链见 display_input.c inject_axis 注释
    if (steps === 0) return;
    displayRouteNapi.injectDisplayRouteAxis(which, steps > 0 ? 1 : -1);
  }

  keyEvent(code: number, pressed: boolean): void {
    // evdev 键码直投 (Xwayland +8 对 XKB keymap); 无 rootId 概念 —
    // 焦点在 Xwayland 侧
    try { displayRouteNapi.injectDisplayRouteKey(code, pressed); } catch (_) {}
  }
}

export function createDesktopInputSink(): DesktopInputSink {
  if (DisplayRouteService.getInstance().isX11()) {
    return new X11Sink();
  }
  return new WaylandSink();
}

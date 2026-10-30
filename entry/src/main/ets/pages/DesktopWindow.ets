import { DesktopPointerService, sendDesktopPointerEvent, desktopKeyboardTarget } from '../service/DesktopPointerService';
import { DesktopPointerOverlay } from '../components/DesktopPointerOverlay';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { KeyboardOverlay } from '../components/KeyboardOverlay';
import { PHONE_DESKTOP_BOTTOM_VP } from '../common/PhoneDesktopLayout';
import { PerformanceOverlay } from '../components/PerformanceOverlay';
import { InputSettingsService } from '../service/InputSettingsService';
import { DesktopTouchpad } from '../common/DesktopTouchpad';
import { GameTouchRouter } from '../common/GameTouchRouter';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  private lastRootId: number = 0;
  onSurfaceCreated(surfaceId: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    this.lastRootId = rootId;
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceCreated rootId=%{public}d surfaceId=%{public}s', rootId, surfaceId);
    if (rootId > 0) {
      try { testNapi.createRenderer(rootId, BigInt(surfaceId)); } catch (_) {}
    }
  }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceChanged rootId=%{public}d %{public}dx%{public}d', rootId, rect.surfaceWidth, rect.surfaceHeight);
    if (rootId > 0) {
      try {
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight, BigInt(surfaceId));
        if (WineWindowManager.getInstance().hasFixedDesktopResolution()) return;
        // 统一经 getEffectiveScale, 不再硬编码 2.0
        const s = WineWindowManager.getInstance().getEffectiveScale();
        testNapi.notifyToplevelResize(rootId,
          Math.round(rect.surfaceWidth / s), Math.round(rect.surfaceHeight / s));
      } catch (_) {}
    }
  }
  onSurfaceDestroyed(surfaceId: string): void {
    const rootId = this.lastRootId;
    this.lastRootId = 0;
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed rootId=%{public}d', rootId);
    if (rootId > 0) {
      try { testNapi.destroyRenderer(rootId, BigInt(surfaceId)); } catch (_) {}
    }
  }
}

@Entry
@Component
struct DesktopPage {
  controller: DesktopController = new DesktopController();
  // Direct touch holds the left button from Down to Up, even without motion.
  private directFinger: number = -1;
  // 双指滚动
  private twoFingerActive: boolean = false;
  private twoFingerLastMidX: number = 0;
  private twoFingerLastMidY: number = 0;
  private twoFingerTl: number = 0;
  private scrollLogCounter: number = 0;
  private twoFingerAccumY: number = 0;
  private twoFingerAccumX: number = 0;
  private static readonly SCROLL_STEP_PX: number = 40; // 物理像素/滚动步, winewayland.drv 只处理 discrete
  // 物理鼠标右键: Press 时捕获目标 tl, Release 发给同一 tl (按下捕获语义)
  private rightBtnTl: number = 0;
  // 物理鼠标中键: 同右键的按下捕获 (OHOS 只对左键合成 touch, 右/中键
  // 仅经 onMouse 到达, 互不干扰, 故共用同一套捕获-配对状态机)
  private midBtnTl: number = 0;
  // 物理鼠标左键: 同右键的按下捕获; mouseLeftActive 用于抑制 OHOS 为左键
  // 合成派发的 touch 事件 (见 onTouch 注释 — 合成 touch 会把手势状态机的
  // "等 Up 再补发 Press+Release" 作用到鼠标上, 静止点击被压成 0ms 脉冲,
  // dinput 轮询类游戏 (PAL2) 看不见这次点击)
  private leftBtnTl: number = 0;
  private mouseLeftActive: boolean = false;
  // hover 移动抽样日志计数
  private hoverLogCounter: number = 0;
  private gameTouch = new GameTouchRouter((target: number, action: number, x: number, y: number, button: number) => {
    if (action !== MouseAction.Move) hilog.info(DOMAIN, 'TouchControls', 'paired touch tl=%{public}d action=%{public}d', target, action);
    sendDesktopPointerEvent(target, action, x, y, button);
  }, MouseAction.Press, MouseAction.Move, MouseAction.Release, mapMouseButton(1));

  @State private touchpadOn: boolean = false;
  private input: InputSettingsService = InputSettingsService.getInstance();
  private touchpad: DesktopTouchpad = new DesktopTouchpad();
  private awaitFreshTouch: boolean = false;

  aboutToAppear(): void {
    this.input.subscribe(this.mirrorInput);
    this.mirrorInput();
  }

  aboutToDisappear(): void {
    this.input.unsubscribe(this.mirrorInput);
    this.cancelTouchInput();
  }

  private cancelTouchInput(): void {
    this.touchpad.reset();
    this.gameTouch.cancel();
    this.directFinger = -1;
    this.twoFingerActive = false;
    this.twoFingerTl = 0;
    this.twoFingerAccumX = 0;
    this.twoFingerAccumY = 0;
    this.awaitFreshTouch = true;
  }

  private mirrorInput = (): void => {
    const enabled = this.input.touchpadEnabled;
    if (enabled !== this.touchpadOn) {
      this.cancelTouchInput();
      this.touchpadOn = enabled;
      if (enabled) this.touchpad.activate();
    }
    this.touchpad.cursorSpeed = this.input.cursorSpeed;
  }

  build() {
    Stack() {
      // The parent supplies black bars; keep the native EGL surface visible.
      XComponent({
        id: 'wine_desktop_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      }).width('100%').height('100%').focusable(true).focusOnTouch(true)
      .onAreaChange((_old: Area, area: Area) => {
        this.touchpad.setBounds(vp2px(Number(area.width)), vp2px(Number(area.height)));
        if (this.touchpadOn) this.touchpad.activate();
      })
      .onVisibleAreaChange([0], (visible: boolean) => { if (!visible) this.cancelTouchInput(); })

      // ── 触控处理: 单指→左键/右键/拖拽, 双指→滚轮 ──
      .onTouch((ev: TouchEvent) => {
        try {
          if (ev.type === TouchType.Cancel) {
            this.cancelTouchInput();
            return;
          }
          if (this.mouseLeftActive) return;
          if (this.awaitFreshTouch) {
            if (ev.type !== TouchType.Down) return;
            this.awaitFreshTouch = false;
          }
          if (this.touchpadOn) {
            this.touchpad.handle(ev);
            return;
          }
          // ── 双指滚动 ──
          if (ev.touches.length >= 2) {
            this.gameTouch.cancel();
            this.directFinger = -1;

            // 计算双指中点
            let midX = 0, midY = 0;
            for (let i = 0; i < ev.touches.length; i++) {
              midX += vp2px(ev.touches[i].x);
              midY += vp2px(ev.touches[i].y);
            }
            midX = Math.round(midX / ev.touches.length);
            midY = Math.round(midY / ev.touches.length);

            if (!this.twoFingerActive) {
              // 进入双指模式
              this.twoFingerActive = true;
              this.twoFingerLastMidX = midX;
              this.twoFingerLastMidY = midY;
              this.twoFingerAccumY = 0;
              this.twoFingerAccumX = 0;
              this.twoFingerTl = testNapi.findToplevelAt(midX, midY) as number ?? 0;
              this.scrollLogCounter = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-START tl=%{public}d fingers=%{public}d',
                this.twoFingerTl, ev.touches.length);
            } else {
              // 累积位移 → 超过阈值发送离散步
              this.twoFingerAccumY += this.twoFingerLastMidY - midY;  // 向上=正=向下滚
              this.twoFingerAccumX += midX - this.twoFingerLastMidX;  // 向右=正=向右滚
              this.twoFingerLastMidX = midX;
              this.twoFingerLastMidY = midY;

              if (this.twoFingerTl > 0) {
                // 垂直滚动
                while (Math.abs(this.twoFingerAccumY) >= DesktopPage.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumY > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 0, sign, DesktopPage.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumY -= sign * DesktopPage.SCROLL_STEP_PX;
                  if (++this.scrollLogCounter % 5 === 0) {
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-V tl=%{public}d',
                      this.twoFingerTl);
                  }
                }
                // 水平滚动
                while (Math.abs(this.twoFingerAccumX) >= DesktopPage.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumX > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 1, sign, DesktopPage.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumX -= sign * DesktopPage.SCROLL_STEP_PX;
                }
              }
            }
            return;
          }

          // ── 单指模式: 恢复双指状态 ──
          if (this.twoFingerActive) {
            this.twoFingerActive = false;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-END');
            return;
          }

          const t = ev.changedTouches.find((touch: TouchObject) =>
            this.directFinger < 0 || touch.id === this.directFinger);
          if (!t) return;
          const px = Math.round(vp2px(t.x));
          const py = Math.round(vp2px(t.y));
          if (ev.type === TouchType.Down) {
            if (this.directFinger >= 0) return;
            const tl = testNapi.findToplevelAt(px, py) as number;
            if (tl <= 0) return;
            this.directFinger = t.id;
            testNapi.raiseToplevel(tl);
            this.gameTouch.begin(tl, px, py);
          } else if (this.directFinger >= 0) {
            if (ev.type === TouchType.Move) this.gameTouch.move(px, py);
            else if (ev.type === TouchType.Up) {
              this.gameTouch.end(px, py);
              this.directFinger = -1;
            }
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // Physical pointer buttons keep their own capture and timing.
      .onMouse((ev: MouseEvent) => {
        try {
          DesktopPointerService.getInstance().move(vp2px(ev.x), vp2px(ev.y));
          // 诊断 (临时): 非 Move 事件在处理器入口记录硬件时间戳 — 同一物理
          // 事件被框架重复派发时 timestamp 相同, 独立事件则不同 (排查 explorer
          // 单击变双击: 日志出现 ~150ms 间隔的同坐标 MOUSE-LEFT press 对)
          if (ev.action !== MouseAction.Move) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-RAW a=%{public}d btn=%{public}d ts=%{public}d',
              ev.action, ev.button, Math.round(ev.timestamp % 1000000000));
          }
          // ── hover 移动直通 (指针跟随/高亮/tooltip/光标形状依赖 WM_MOUSEMOVE) ──
          if (ev.action === MouseAction.Move) {
            // touch 手势进行中: 左键按住移动等由 onTouch 状态机发 motion
            // (OHOS 对左键同时派发合成 touch 和 mouse 事件, 此处再发会双发)
            if (this.gameTouch.active || this.twoFingerActive) return;
            const px = Math.round(vp2px(ev.x));
            const py = Math.round(vp2px(ev.y));
            // 按键按住拖动: 发给按下捕获的 tl; 否则按当前位置找目标
            // (跨窗口 enter/leave 由注入层 ACT_MOVE 的焦点判定处理)
            let tl = this.leftBtnTl || this.rightBtnTl || this.midBtnTl;
            if (tl <= 0) {
              tl = testNapi.findToplevelAt(px, py) as number;
              if (tl <= 0) return;
            }
            if (++this.hoverLogCounter % 120 === 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-MOVE tl=%{public}d px=(%{public}d,%{public}d) n=%{public}d',
                tl, px, py, this.hoverLogCounter);
            }
            sendDesktopPointerEvent(tl, MouseAction.Move, px, py, 0, ev.rawDeltaX, ev.rawDeltaY);
            return;
          }
          if (ev.action !== MouseAction.Press && ev.action !== MouseAction.Release) return;
          const px = Math.round(vp2px(ev.x));
          const py = Math.round(vp2px(ev.y));
          // ── 物理鼠标左键: 真实时序直通 (按下即转发, 不等手势判定) ──
          // Suppress the platform's synthesized touch while left mouse is held.
          if (ev.button === MouseButton.Left) {
            const btnLeft = mapMouseButton(1);
            if (ev.action === MouseAction.Press) {
              this.mouseLeftActive = true;
              // A synthesized touch can arrive before the physical mouse.
              this.gameTouch.cancel();
              this.directFinger = -1;
              this.touchpad.reset();
              const tl: number = testNapi.findToplevelAt(px, py) as number;
              if (tl <= 0) {
                this.leftBtnTl = 0;
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT skipped (no toplevel at %{public}d,%{public}d)', px, py);
                return;
              }
              this.leftBtnTl = tl;
              testNapi.raiseToplevel(tl);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT press tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
              sendDesktopPointerEvent(tl, MouseAction.Press, px, py, btnLeft, 0, 0, true);
            } else {
              if (this.leftBtnTl <= 0) {
                this.mouseLeftActive = false;
                return;
              }
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT release tl=%{public}d px=(%{public}d,%{public}d)', this.leftBtnTl, px, py);
              sendDesktopPointerEvent(this.leftBtnTl, MouseAction.Release, px, py, btnLeft);
              this.leftBtnTl = 0;
              this.mouseLeftActive = false;
            }
            return;
          }
          // ── 右键 / 中键: 共用按下捕获-释放配对 ──
          // OHOS 只对左键合成 touch, 右/中键在 onTouch 无对应事件, 不会双发;
          // 两键各自独立捕获 tl, 同按 (罕见) 也互不串扰
          if (ev.button !== MouseButton.Right && ev.button !== MouseButton.Middle) return;
          const isRight = ev.button === MouseButton.Right;
          const btnCode = mapMouseButton(isRight ? 2 : 4);
          const btnName = isRight ? 'RIGHT' : 'MIDDLE';
          if (ev.action === MouseAction.Press) {
            const tl: number = testNapi.findToplevelAt(px, py) as number;
            if (tl <= 0) {
              if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s skipped (no toplevel at %{public}d,%{public}d)', btnName, px, py);
              return;
            }
            if (isRight) this.rightBtnTl = tl; else this.midBtnTl = tl;
            testNapi.raiseToplevel(tl);
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s press tl=%{public}d px=(%{public}d,%{public}d)', btnName, tl, px, py);
            sendDesktopPointerEvent(tl, MouseAction.Press, px, py, btnCode, 0, 0, true);
          } else {
            const btnTl = isRight ? this.rightBtnTl : this.midBtnTl;
            if (btnTl <= 0) return;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s release tl=%{public}d px=(%{public}d,%{public}d)', btnName, btnTl, px, py);
            sendDesktopPointerEvent(btnTl, MouseAction.Release, px, py, btnCode);
            if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // ── 键盘事件 ──
      .onKeyEvent((ev: KeyEvent) => {
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0) return;
        ev.stopPropagation();
        const code = OH2EVDEV[ev.keyCode] ?? 0;
        if (code <= 0) return;
        testNapi.sendKeyEvent(desktopKeyboardTarget(rootId), code, ev.type === KeyType.Down);
      })

      // ── 滚轮事件 ──
      .onAxisEvent((ev?: AxisEvent) => {
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0 || !ev) return;
        const wpx = Math.round(vp2px(ev.x));
        const wpy = Math.round(vp2px(ev.y));
        const vert = ev.getVerticalAxisValue();
        const horiz = ev.getHorizontalAxisValue();
        const step = ev.scrollStep ?? 0;
        const tl: number = testNapi.findToplevelAt(wpx, wpy) as number;
        if (vert !== 0) testNapi.sendScrollEvent(tl, 0, vert, step, wpx, wpy);
        if (horiz !== 0) testNapi.sendScrollEvent(tl, 1, horiz, step, wpx, wpy);
      })

      // 自定义虚拟键盘 (独立桌面窗口同样可用)
      DesktopPointerOverlay()
      KeyboardOverlay({ targetId: 0 })
      PerformanceOverlay()
    }
    .width('100%').height('100%').backgroundColor('#000')
    .padding({ bottom: PHONE_DESKTOP_BOTTOM_VP })
  }
}

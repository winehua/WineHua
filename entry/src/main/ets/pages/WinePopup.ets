import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

// PC 模式 popup (菜单/tooltip/下拉框) 子窗口页面。
// 与 WineWindow 同模式: 自定义 Controller 通过 surfaceId 管理渲染器。
// 区别: 不调 notifyToplevelResize (popup 无 xdg_toplevel, 不能发 configure),
//       不处理键盘 (焦点留在父窗口), 不处理 startMoving。
// 触控: 移植 WineWindow 手势状态机 (模拟器鼠标合成 touch, onMouse 收不到;
// 无状态机则菜单点击全丢 — 与主窗口不对称的根因)。
class PopupXComponentController extends XComponentController {
  popupId: number = 0;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceCreated popup=%{public}d surfaceId=%{public}s',
      this.popupId, surfaceId);
    try {
      testNapi.createRenderer(this.popupId, BigInt(surfaceId));
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceCreated createRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceChanged popup=%{public}d rect=%{public}s',
      this.popupId, JSON.stringify(rect));
    try {
      testNapi.resizeRenderer(this.popupId, rect.surfaceWidth, rect.surfaceHeight);
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceChanged resizeRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'WinePopup', '[PopupCtrl] onSurfaceDestroyed popup=%{public}d', this.popupId);
    try {
      testNapi.destroyRenderer(this.popupId);
    } catch (e) {
      hilog.error(DOMAIN, 'WinePopup', 'ERR onSurfaceDestroyed destroyRenderer: %{public}s', JSON.stringify(e));
    }
  }
}

@Entry
@Component
struct WinePopup {
  controller: PopupXComponentController = new PopupXComponentController();
  @State popupId: number = 0;

  // ── 触控状态机 (移植自 WineWindow.ets, 单窗口简化) ──
  // 单指: 短按→左键点击(含双击序列) / 长按→右键 / 移动超阈值→按住左键拖拽; 双指→滚轮
  // 物理鼠标左键的合成 touch: onMouse 已按真实时序直通, mouseLeftActive 抑制
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private mouseLeftActive: boolean = false;
  private motionLogCounter: number = 0;
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  // 双击检测
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;
  // 双指滚动
  private twoFingerActive: boolean = false;
  private twoFingerLastMidX: number = 0;
  private twoFingerLastMidY: number = 0;
  private twoFingerAccumY: number = 0;
  private twoFingerAccumX: number = 0;
  private static readonly SCROLL_STEP_PX: number = 40; // 物理像素/滚动步, winewayland.drv 只处理 discrete
  private scrollLogCounter: number = 0;

  aboutToAppear(): void {
    // loadContent 同步调用, 与 setPendingToplevel 配对 (同 WineWindow)
    this.popupId = testNapi.getCurrentToplevelId() as number;
    this.controller.popupId = this.popupId;
    hilog.info(DOMAIN, 'WinePopup', 'aboutToAppear popup=%{public}d', this.popupId);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_popup_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      })
        .width('100%')
        .height('100%')
        // 背景透明: ARGB popup (圆角菜单/异型 tooltip) 需要透过 per-pixel alpha
        .backgroundColor('#00000000')
        .focusable(false)
        // -- 触摸事件 (移植自 WineWindow 手势状态机; 模拟器鼠标合成 touch,
        //    无此处理菜单点击被丢弃) --
        .onTouch((ev: TouchEvent) => {
          try {
            if (this.popupId <= 0) return;

            // ── Cancel 统一前置处理 (双指滚动中 Cancel 不清理则 twoFingerActive 滞留) ──
            if (ev.type === TouchType.Cancel) {
              if (this.isDragging) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-CANCEL-DRAG popup=%{public}d px=(%{public}d,%{public}d)',
                  this.popupId, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.popupId, MouseAction.Release,
                  this.pendingPx, this.pendingPy, mapMouseButton(1));
              } else {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-CANCEL popup=%{public}d (no event sent)', this.popupId);
              }
              this.touchPending = false;
              this.isDragging = false;
              this.twoFingerActive = false;
              return;
            }

            // 物理鼠标左键按下期间 (mouseLeftActive): 忽略所有触摸 (含双指分支)
            if (this.mouseLeftActive) return;

            // ── 双指滚动 (中点位移累积 → 离散步) ──
            if (ev.touches.length >= 2) {
              if (this.touchPending || this.isDragging) {
                if (this.isDragging) {
                  testNapi.sendPointerEvent(this.popupId, MouseAction.Release,
                    this.pendingPx, this.pendingPy, mapMouseButton(1));
                }
                this.touchPending = false;
                this.isDragging = false;
              }
              let midX = 0, midY = 0;
              for (let i = 0; i < ev.touches.length; i++) {
                midX += vp2px(ev.touches[i].x);
                midY += vp2px(ev.touches[i].y);
              }
              midX = Math.round(midX / ev.touches.length);
              midY = Math.round(midY / ev.touches.length);
              if (!this.twoFingerActive) {
                this.twoFingerActive = true;
                this.twoFingerLastMidX = midX;
                this.twoFingerLastMidY = midY;
                this.twoFingerAccumY = 0;
                this.twoFingerAccumX = 0;
                this.scrollLogCounter = 0;
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-SCROLL-START popup=%{public}d fingers=%{public}d',
                  this.popupId, ev.touches.length);
              } else {
                this.twoFingerAccumY += this.twoFingerLastMidY - midY;  // 向上=正=向下滚
                this.twoFingerAccumX += midX - this.twoFingerLastMidX;  // 向右=正=向右滚
                this.twoFingerLastMidX = midX;
                this.twoFingerLastMidY = midY;
                while (Math.abs(this.twoFingerAccumY) >= WinePopup.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumY > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.popupId, 0, sign, WinePopup.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumY -= sign * WinePopup.SCROLL_STEP_PX;
                  if (++this.scrollLogCounter % 5 === 0) {
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-SCROLL-V popup=%{public}d', this.popupId);
                  }
                }
                while (Math.abs(this.twoFingerAccumX) >= WinePopup.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumX > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.popupId, 1, sign, WinePopup.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumX -= sign * WinePopup.SCROLL_STEP_PX;
                }
              }
              return;
            }

            // ── 单指模式: 恢复双指状态 ──
            if (this.twoFingerActive) {
              this.twoFingerActive = false;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-SCROLL-END');
              return;
            }

            const t = ev.changedTouches[0];
            if (!t) return;
            const px = Math.round(vp2px(t.x));
            const py = Math.round(vp2px(t.y));

            if (ev.type === TouchType.Down) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-TOUCH popup=%{public}d px=(%{public}d,%{public}d)',
                this.popupId, px, py);
              this.touchPending = true;
              this.isDragging = false;
              this.touchStartTime = Date.now();
              this.touchStartPx = px;
              this.touchStartPy = py;
              this.pendingPx = px;
              this.pendingPy = py;
              this.motionLogCounter = 0;

            } else if (ev.type === TouchType.Move) {
              if (!this.touchPending && !this.isDragging) return;
              const dist = Math.abs(px - this.touchStartPx) + Math.abs(py - this.touchStartPy);
              if (this.touchPending && dist >= WinePopup.DRAG_THRESHOLD) {
                // 进入拖拽模式: 补发 Press (Down 时未发)
                const btnLeft = mapMouseButton(1);
                this.touchPending = false;
                this.isDragging = true;
                this.lastTapTime = 0; // 打断双击序列 (拖拽不是点击)
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-DRAG-START popup=%{public}d px=(%{public}d,%{public}d) dist=%{public}d',
                  this.popupId, px, py, dist);
                testNapi.sendPointerEvent(this.popupId, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                testNapi.sendPointerEvent(this.popupId, MouseAction.Move, px, py, btnLeft);
              } else if (this.isDragging) {
                testNapi.sendPointerEvent(this.popupId, MouseAction.Move, px, py, 0);
                if (++this.motionLogCounter % 10 === 0) {
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-DRAG-MOVE popup=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                    this.popupId, px, py, this.motionLogCounter);
                }
              }

            } else if (ev.type === TouchType.Up) {
              if (this.isDragging) {
                const btnLeft = mapMouseButton(1);
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-DRAG-END popup=%{public}d px=(%{public}d,%{public}d)',
                  this.popupId, px, py);
                testNapi.sendPointerEvent(this.popupId, MouseAction.Release, px, py, btnLeft);
                this.isDragging = false;
              } else if (this.touchPending) {
                this.touchPending = false;
                const elapsed = Date.now() - this.touchStartTime;
                if (elapsed >= WinePopup.LONG_PRESS_MS) {
                  // 长按 → 右键
                  const btnRight = mapMouseButton(2);
                  this.lastTapTime = 0; // 长按打断双击序列
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-LONGPRESS->RIGHTCLICK popup=%{public}d elapsed=%{public}dms px=(%{public}d,%{public}d)',
                    this.popupId, elapsed, this.pendingPx, this.pendingPy);
                  testNapi.sendPointerEvent(this.popupId, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
                  testNapi.sendPointerEvent(this.popupId, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
                } else {
                  // 短按 → 左键 或 双击
                  const btnLeft = mapMouseButton(1);
                  const now = Date.now();
                  const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
                  const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < WinePopup.DOUBLE_TAP_MS
                                      && dist < WinePopup.DOUBLE_TAP_DIST);
                  if (isDoubleTap) {
                    this.lastTapTime = 0;
                    testNapi.sendPointerEvent(this.popupId, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
                    testNapi.sendPointerEvent(this.popupId, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-DOUBLE-TAP popup=%{public}d px=(%{public}d,%{public}d)',
                      this.popupId, this.lastTapPx, this.lastTapPy);
                  } else {
                    testNapi.sendPointerEvent(this.popupId, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                    testNapi.sendPointerEvent(this.popupId, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                    this.lastTapTime = now;
                    this.lastTapPx = this.pendingPx;
                    this.lastTapPy = this.pendingPy;
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-TAP->LEFTCLICK popup=%{public}d px=(%{public}d,%{public}d)',
                      this.popupId, this.pendingPx, this.pendingPy);
                  }
                }
              }
            }
          } catch (e) {
            hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-TOUCH handler exception: %{public}s', JSON.stringify(e));
          }
        })
        // -- 滚轮事件 (菜单滚动条等) --
        // 轴事件不冒泡, 必须挂在命中链顶端的 XComponent 上 (同 WineWindow)
        .onAxisEvent((event?: AxisEvent) => {
          if (this.popupId <= 0 || !event) return;
          const px = vp2px(event.x);
          const py = vp2px(event.y);
          const vert = event.getVerticalAxisValue();
          const horiz = event.getHorizontalAxisValue();
          const step = event.scrollStep ?? 0;
          if (vert !== 0) {
            testNapi.sendScrollEvent(this.popupId, 0, vert, step, px, py);
          }
          if (horiz !== 0) {
            testNapi.sendScrollEvent(this.popupId, 1, horiz, step, px, py);
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#00000000')
    .focusable(false)

    // -- 鼠标事件 --
    .onMouse((event: MouseEvent) => {
      if (this.popupId <= 0) return;
      // 触摸手势进行中: 手指触摸的合成 mouse 事件由 onTouch 状态机负责
      // (OHOS 对左键同时派发 mouse 和合成 touch, 双发会重复注入)
      if (event.action === MouseAction.Move &&
        (this.touchPending || this.isDragging || this.twoFingerActive)) return;
      // 物理鼠标左键: 真实时序直通, 以 mouseLeftActive 抑制合成 touch 进状态机
      if (event.action === MouseAction.Release) {
        // 复位不限定 Left: 跨窗口/失焦释放时左键 Release 到不了本窗口,
        // 若只认 Left 会悬挂 mouseLeftActive, 本窗口触控永久失效
        this.mouseLeftActive = false;
      }
      if (event.button === MouseButton.Left && event.action === MouseAction.Press) {
        this.mouseLeftActive = true;
        // 竞态: 合成 touch 可能已先进 DRAG 并注入 Press → 补 Release 配对
        if (this.isDragging) {
          testNapi.sendPointerEvent(this.popupId, MouseAction.Release,
            this.pendingPx, this.pendingPy, mapMouseButton(1));
        }
        this.touchPending = false;
        this.isDragging = false;
        this.twoFingerActive = false;
      }
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        // btn 用十进制 (同 WineWindow: ArkTS hilog %{public}x 渲染乱码)
        // evdev: left=272 right=273 middle=274
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] POPUP-MOUSE popup=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=%{public}d',
          this.popupId, event.action, Math.round(px), Math.round(py), btn);
      }
      testNapi.sendPointerEvent(this.popupId, event.action, px, py, btn, event.rawDeltaX, event.rawDeltaY, true);
    })
  }
}

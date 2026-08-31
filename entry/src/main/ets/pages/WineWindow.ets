import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { EVDEV_KEY_ENTER, OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;

// 自定义 XComponentController: 通过 surfaceId 管理 XComponent 生命周期
// 不再使用 libraryname='entry', 从根本上消除多窗口 XComponent exports 冲突
class WineXComponentController extends XComponentController {
  toplevelId: number = 0;

  onSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceCreated tl=%{public}d surfaceId=%{public}s',
      this.toplevelId, surfaceId);
    try {
      testNapi.createRenderer(this.toplevelId, BigInt(surfaceId));
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceCreated createRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceChanged tl=%{public}d surfaceId=%{public}s rect=%{public}s',
      this.toplevelId, surfaceId, JSON.stringify(rect));
    try {
      testNapi.resizeRenderer(this.toplevelId, rect.surfaceWidth, rect.surfaceHeight);
      // surface 是物理像素, Wine 需要逻辑坐标 (统一经 getEffectiveScale, 不再硬编码 2.0)
      const s = WineWindowManager.getInstance().getEffectiveScale();
      const w = Math.round(rect.surfaceWidth / s);
      const h = Math.round(rect.surfaceHeight / s);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceChanged tl=%{public}d → notifyToplevelResize(%{public}d, %{public}d)',
        this.toplevelId, w, h);
      testNapi.notifyToplevelResize(this.toplevelId, w, h);
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceChanged resizeRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceDestroyed tl=%{public}d surfaceId=%{public}s',
      this.toplevelId, surfaceId);
    try {
      testNapi.destroyRenderer(this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceDestroyed destroyRenderer: %{public}s', JSON.stringify(e));
    }
  }
}

@Entry
@Component
struct WineWindow {
  controller: WineXComponentController = new WineXComponentController();
  @State toplevelId: number = 0;
  @State surfacePxW: number = 1;
  @State surfacePxH: number = 1;

  // ── 触控状态机 (移植自 DesktopWindow.ets 桌面模式, 单窗口简化) ──
  // 单指: 短按→左键点击 / 长按→右键 / 移动超阈值→按住左键拖拽; 双指→滚轮
  // 物理鼠标左键的合成 touch: onMouse 已按真实时序直通, mouseLeftActive 抑制
  // (若进状态机, 静止点击会被压成 Press+Release 同刻脉冲, dinput 轮询不可见)
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
  // 回车 down 时间戳 (毫秒): 软键盘/IME 场景系统吞 KeyDown 只派发 KeyUp,
  // onKeyEvent 收到无配对 up 时补发完整 down+up, 保证 Wine 触发按键
  private lastEnterDownTs: number = 0;
  // 无配对 up 的判定窗口: 窗口内收到 up 视为正常配对的抬起, 不补发
  private static readonly ENTER_SYNTH_WINDOW_MS: number = 1000;
  private twoFingerAccumX: number = 0;
  private static readonly SCROLL_STEP_PX: number = 40; // 物理像素/滚动步, winewayland.drv 只处理 discrete
  private scrollLogCounter: number = 0;

  aboutToAppear(): void {
    // 关键: aboutToAppear 是 loadContent 的同步调用, 在下一个 Ability 覆盖前读取
    this.toplevelId = testNapi.getCurrentToplevelId() as number;
    // 同步设置给 controller: 保证 onSurfaceCreated 拿到正确的 toplevelId
    this.controller.toplevelId = this.toplevelId;
    hilog.info(DOMAIN, 'WineWindow', 'aboutToAppear tl=%{public}d', this.toplevelId);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_sub_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
        // 不再设置 libraryname: 使用自定义 Controller 的 surfaceId 回调
      })
        .width('100%')
        .height('100%')
        // 背景透明: 与 '#000' 等价 (surface 不透明覆盖全部区域),
        // 为后续 ARGB 异型窗口方案 (子窗口路线) 预留
        .backgroundColor('#00000000')
        .focusable(false)
        // -- 触摸事件 (参考 DesktopWindow 桌面模式手势状态机, 单窗口无目标查找) --
        .onTouch((ev: TouchEvent) => {
          try {
            if (this.toplevelId <= 0) return;

            // ── Cancel 统一前置处理 ──
            // 系统取消触摸 (手势/通知/失焦)。不依赖 touches/changedTouches 数量:
            // 双指滚动中 Cancel 若不清理, twoFingerActive 滞留 true, 下一次单指
            // Down 被 SCROLL-END 分支吞掉, 整次点击丢失; changedTouches 为空时
            // 原尾部 Cancel 分支也到不了 (被 `if (!t) return` 提前退出)
            if (ev.type === TouchType.Cancel) {
              if (this.isDragging) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL-DRAG tl=%{public}d px=(%{public}d,%{public}d)',
                  this.toplevelId, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release,
                  this.pendingPx, this.pendingPy, mapMouseButton(1));
              } else {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL tl=%{public}d (no event sent)', this.toplevelId);
              }
              this.touchPending = false;
              this.isDragging = false;
              this.twoFingerActive = false;
              return;
            }

            // 物理鼠标左键按下期间 (mouseLeftActive): 忽略所有触摸 (含双指分支,
            // 守卫必须前置 — OHOS 为左键合成的 touch 已由 onMouse 真实时序直通,
            // 若在单指分支才拦, 鼠标按住 + 双指触屏会误入滚动)
            if (this.mouseLeftActive) return;

            // ── 双指滚动 (中点位移累积 → 离散步) ──
            if (ev.touches.length >= 2) {
              // 取消单指状态
              if (this.touchPending || this.isDragging) {
                if (this.isDragging) {
                  testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release,
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
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-START tl=%{public}d fingers=%{public}d',
                  this.toplevelId, ev.touches.length);
              } else {
                // 累积位移 → 超过阈值发送离散步
                this.twoFingerAccumY += this.twoFingerLastMidY - midY;  // 向上=正=向下滚
                this.twoFingerAccumX += midX - this.twoFingerLastMidX;  // 向右=正=向右滚
                this.twoFingerLastMidX = midX;
                this.twoFingerLastMidY = midY;
                while (Math.abs(this.twoFingerAccumY) >= WineWindow.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumY > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.toplevelId, 0, sign, WineWindow.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumY -= sign * WineWindow.SCROLL_STEP_PX;
                  if (++this.scrollLogCounter % 5 === 0) {
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-V tl=%{public}d', this.toplevelId);
                  }
                }
                while (Math.abs(this.twoFingerAccumX) >= WineWindow.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumX > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.toplevelId, 1, sign, WineWindow.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumX -= sign * WineWindow.SCROLL_STEP_PX;
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

            const t = ev.changedTouches[0];
            if (!t) return;
            const px = Math.round(vp2px(t.x));
            const py = Math.round(vp2px(t.y));

            if (ev.type === TouchType.Down) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH tl=%{public}d px=(%{public}d,%{public}d)',
                this.toplevelId, px, py);
              // 进入 PENDING 状态: 暂不发送事件, 等判定长按/拖拽/点击
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
              if (this.touchPending && dist >= WineWindow.DRAG_THRESHOLD) {
                // 进入拖拽模式: 补发 Press (Down 时未发)
                const btnLeft = mapMouseButton(1);
                this.touchPending = false;
                this.isDragging = true;
                this.lastTapTime = 0; // 打断双击序列 (拖拽不是点击)
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-START tl=%{public}d px=(%{public}d,%{public}d) dist=%{public}d',
                  this.toplevelId, px, py, dist);
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Move, px, py, btnLeft);
              } else if (this.isDragging) {
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Move, px, py, 0);
                if (++this.motionLogCounter % 10 === 0) {
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-MOVE tl=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                    this.toplevelId, px, py, this.motionLogCounter);
                }
              }
              // PENDING 且未超阈值: 不做任何事 (等 Up 判定长按/点击)

            } else if (ev.type === TouchType.Up) {
              if (this.isDragging) {
                const btnLeft = mapMouseButton(1);
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-END tl=%{public}d px=(%{public}d,%{public}d)',
                  this.toplevelId, px, py);
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release, px, py, btnLeft);
                this.isDragging = false;
              } else if (this.touchPending) {
                this.touchPending = false;
                const elapsed = Date.now() - this.touchStartTime;
                if (elapsed >= WineWindow.LONG_PRESS_MS) {
                  // 长按 → 右键
                  const btnRight = mapMouseButton(2);
                  this.lastTapTime = 0; // 长按打断双击序列
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] LONGPRESS->RIGHTCLICK tl=%{public}d elapsed=%{public}dms px=(%{public}d,%{public}d)',
                    this.toplevelId, elapsed, this.pendingPx, this.pendingPy);
                  testNapi.sendPointerEvent(this.toplevelId, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
                  testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
                } else {
                  // 短按 → 左键 或 双击
                  const btnLeft = mapMouseButton(1);
                  const now = Date.now();
                  const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
                  const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < WineWindow.DOUBLE_TAP_MS
                                      && dist < WineWindow.DOUBLE_TAP_DIST);
                  if (isDoubleTap) {
                    // 双击: 第一次点击已由上一个单击路径发出, 这里只补发第二次
                    this.lastTapTime = 0;
                    testNapi.sendPointerEvent(this.toplevelId, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
                    testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DOUBLE-TAP tl=%{public}d px=(%{public}d,%{public}d)',
                      this.toplevelId, this.lastTapPx, this.lastTapPy);
                  } else {
                    testNapi.sendPointerEvent(this.toplevelId, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                    testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                    this.lastTapTime = now;
                    this.lastTapPx = this.pendingPx;
                    this.lastTapPy = this.pendingPy;
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TAP->LEFTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
                      this.toplevelId, this.pendingPx, this.pendingPy);
                  }
                }
              }
            }
          } catch (e) {
            hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
          }
        })
        // -- 滚轮事件 (轴事件, API 17+) --
        // 轴事件不冒泡 (AxisEvent.propagation 需显式激活), 挂外层 Stack 收不到,
        // 必须挂在命中链顶端的 XComponent 上
        .onAxisEvent((event?: AxisEvent) => {
          if (this.toplevelId <= 0 || !event) return;
          const px = vp2px(event.x);
          const py = vp2px(event.y);
          const vert = event.getVerticalAxisValue();
          const horiz = event.getHorizontalAxisValue();
          const step = event.scrollStep ?? 0;
          if (vert !== 0) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL tl=%{public}d vert=%{public}d step=%{public}d px=(%{public}d,%{public}d)',
              this.toplevelId, Math.round(vert), step, Math.round(px), Math.round(py));
            testNapi.sendScrollEvent(this.toplevelId, 0, vert, step, px, py);
          }
          if (horiz !== 0) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL tl=%{public}d horiz=%{public}d step=%{public}d px=(%{public}d,%{public}d)',
              this.toplevelId, Math.round(horiz), step, Math.round(px), Math.round(py));
            testNapi.sendScrollEvent(this.toplevelId, 1, horiz, step, px, py);
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#00000000')
    .focusable(true)
    .defaultFocus(true)

    // -- 鼠标事件 --
    .onMouse((event: MouseEvent) => {
      if (this.toplevelId <= 0) return;
      // Wine 发起 xdg_toplevel.move → 下一个 MOVE 时调用 OHOS 原生 startMoving()
      // 必须优先于手势守卫: 守卫本意是拦"手指触摸的合成 mouse Move",
      // 若在守卫前不处理, 窗口移动请求的 Move 也被吞, moveGrabPending 悬挂
      // (触摸拖拽标题栏: OHOS 原生拖拽动画由这里接管, 不转发给 Wine)
      if (event.action === MouseAction.Move && WineWindowManager.getInstance().shouldStartMoving(this.toplevelId)) {
        try {
          const win = WineWindowManager.getInstance().getWindow(this.toplevelId);
          if (win) {
            win.startMoving();
            return; // OHOS 接管拖拽, 不再转发给 Wine
          }
        } catch (e) { /* fall through to normal path */ }
      }
      // 触摸手势进行中: 手指触摸的合成 mouse 事件由 onTouch 状态机负责
      // (OHOS 对左键同时派发 mouse 和合成 touch, 双发会重复注入)
      if (event.action === MouseAction.Move &&
        (this.touchPending || this.isDragging || this.twoFingerActive)) return;
      // 物理鼠标左键: 真实时序直通, 以 mouseLeftActive 抑制合成 touch 进状态机
      // (合成 touch 可能先一步进入 PENDING/DRAG, 静默清除防双发)
      if (event.action === MouseAction.Release) {
        // 复位不限定 Left: 跨窗口/失焦释放时左键 Release 到不了本窗口,
        // 若只认 Left 会悬挂 mouseLeftActive, 本窗口触控永久失效。
        // 权衡: 左键按住中滚动中键时会被误复位 — 罕见(需同时手指触屏)
        // 且下次左键 Press 重新置位, 优于悬挂
        this.mouseLeftActive = false;
      }
      if (event.button === MouseButton.Left && event.action === MouseAction.Press) {
        this.mouseLeftActive = true;
        // 竞态: 合成 touch 可能已先进 DRAG 并注入 Press → 补 Release 配对
        // (否则 Wine 收到无配对的 Press, 左键悬挂; PENDING 未注入过, 无需补)
        if (this.isDragging) {
          testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release,
            this.pendingPx, this.pendingPy, mapMouseButton(1));
        }
        // 双指滚动中按鼠标: twoFingerActive 也须清除 — 否则合成 touch 被
        // mouseLeftActive 拦掉后恢复分支到不了, 悬挂期间鼠标 Move 全被吞。
        // 双指模式未注入按钮事件, 直接清无配对问题
        this.touchPending = false;
        this.isDragging = false;
        this.twoFingerActive = false;
      }
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE tl=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=0x%{public}x',
          this.toplevelId, event.action, Math.round(px), Math.round(py), btn);
      }
      testNapi.sendPointerEvent(this.toplevelId, event.action, px, py, btn);
    })

    // -- 键盘事件 --
    .onKeyEvent((event: KeyEvent) => {
      hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] onKeyEvent keyCode=%{public}d type=%{public}d tl=%{public}d',
        event.keyCode, event.type, this.toplevelId);
      if (this.toplevelId <= 0) return;
      event.stopPropagation();
      const evdevCode = OH2EVDEV[event.keyCode] ?? 0;
      if (evdevCode === 0) {
        hilog.warn(DOMAIN, 'KBD-PIPE', '[ArkTS] KEY unmapped ohosCode=%{public}d', event.keyCode);
        return;
      }
      const pressed = (event.type === KeyType.Down);
      hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] KEY tl=%{public}d ohos=%{public}d->evdev=%{public}d %{public}s',
        this.toplevelId, event.keyCode, evdevCode, pressed ? 'DOWN' : 'UP');
      // 回车补发: 软键盘/IME 场景系统吞掉 KeyDown 只派发 KeyUp (onKeyEvent
      // 无配对 down) → Wine 收不到按下不触发。收到回车 up 且窗口内无 down
      // 记录时补发完整 down+up (sendFunctionKey 注入已移除, 统一在此兜底)
      if (evdevCode === EVDEV_KEY_ENTER) {
        if (pressed) {
          this.lastEnterDownTs = Date.now();
        } else if (Date.now() - this.lastEnterDownTs > WineWindow.ENTER_SYNTH_WINDOW_MS) {
          hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] ENTER SYNTH down (missing down)');
          testNapi.sendKeyEvent(this.toplevelId, EVDEV_KEY_ENTER, true);
        }
      }
      testNapi.sendKeyEvent(this.toplevelId, evdevCode, pressed);
    })

    .onAreaChange((_o: Area, n: Area) => {
      this.surfacePxW = vp2px(n.width as number);
      this.surfacePxH = vp2px(n.height as number);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onAreaChange tl=%{public}d w=%{public}d h=%{public}d',
        this.toplevelId, Math.round(this.surfacePxW), Math.round(this.surfacePxH));
    })
  }
}

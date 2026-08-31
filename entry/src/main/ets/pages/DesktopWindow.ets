import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
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
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight);
        // 统一经 getEffectiveScale, 不再硬编码 2.0
        const s = WineWindowManager.getInstance().getEffectiveScale();
        testNapi.notifyToplevelResize(rootId,
          Math.round(rect.surfaceWidth / s), Math.round(rect.surfaceHeight / s));
      } catch (_) {}
    }
  }
  onSurfaceDestroyed(_s: string): void {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed rootId=%{public}d', rootId);
    if (rootId > 0) {
      try { testNapi.destroyRenderer(rootId); } catch (_) {}
    }
  }
}

@Entry
@Component
struct DesktopPage {
  controller: DesktopController = new DesktopController();
  // 触控状态机: IDLE → PENDING → (DRAGGING | 长按 | 点击)
  // 双指进入 SCROLLING 模式, 覆盖单指状态
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingTl: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private motionLogCounter: number = 0;
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  // 双击检测
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private lastTapTl: number = 0;
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;
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

  build() {
    Stack() {
      XComponent({
        id: 'wine_desktop_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      }).width('100%').height('100%').backgroundColor('#000').focusable(true).focusOnTouch(true)

      // ── 触控处理: 单指→左键/右键/拖拽, 双指→滚轮 ──
      .onTouch((ev: TouchEvent) => {
        try {
          // ── 双指滚动 ──
          if (ev.touches.length >= 2) {
            // 取消单指状态
            if (this.touchPending || this.isDragging) {
              if (this.isDragging) {
                const btnLeft = mapMouseButton(1);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
              }
              this.touchPending = false;
              this.isDragging = false;
            }

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

          const t = ev.changedTouches[0];
          if (!t) return;
          // 物理鼠标左键的合成 touch: 已由 .onMouse 以真实时序直通, 此处忽略。
          // 若让它进手势状态机, 静止点击会被压成 Press+Release 同刻脉冲
          // (dinput 轮询不可见), 长按还会被误判成右键 — 鼠标有自己的右键。
          if (this.mouseLeftActive) return;
          const px = Math.round(vp2px(t.x));
          const py = Math.round(vp2px(t.y));

          if (ev.type === TouchType.Down) {
            // 查找目标 toplevel (px,py 为物理坐标, findToplevelAt 内部通过 viewport 转逻辑坐标)
            const tl: number = testNapi.findToplevelAt(px, py) as number;
            if (tl <= 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH skipped (no toplevel at %{public}d,%{public}d)', px, py);
              return;
            }
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
            testNapi.raiseToplevel(tl);
            // 进入 PENDING 状态: 暂不发送事件, 等判定长按/拖拽/点击
            this.touchPending = true;
            this.isDragging = false;
            this.touchStartTime = Date.now();
            this.touchStartPx = px;
            this.touchStartPy = py;
            this.pendingTl = tl;
            this.pendingPx = px;
            this.pendingPy = py;
            this.motionLogCounter = 0;

          } else if (ev.type === TouchType.Move) {
            if (!this.touchPending && !this.isDragging) return;

            const dist = Math.abs(px - this.touchStartPx) + Math.abs(py - this.touchStartPy);

            if (this.touchPending && dist >= DesktopPage.DRAG_THRESHOLD) {
              // 进入拖拽模式: 补发 Press (Down 时未发)
              const btnLeft = mapMouseButton(1);
              this.touchPending = false;
              this.isDragging = true;
              // 打断双击序列 (拖拽不是点击)
              this.lastTapTime = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-START tl=%{public}d px=(%{public}d,%{public}d) dist=%{public}d',
                this.pendingTl, px, py, dist);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, btnLeft);
            } else if (this.isDragging) {
              // 拖拽移动: 持续发送 Motion
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, 0);
              if (++this.motionLogCounter % 10 === 0) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-MOVE tl=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                  this.pendingTl, px, py, this.motionLogCounter);
              }
            }
            // PENDING 且未超过阈值: 不做任何事 (等 Up 判定长按/点击)

          } else if (ev.type === TouchType.Up) {
            if (this.isDragging) {
              // 拖拽结束
              const btnLeft = mapMouseButton(1);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-END tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
              this.isDragging = false;

            } else if (this.touchPending) {
              // PENDING 状态: 按按下时长判定长按 vs 点击
              this.touchPending = false;
              const elapsed = Date.now() - this.touchStartTime;

              if (elapsed >= DesktopPage.LONG_PRESS_MS) {
                // 长按 → 右键
                const btnRight = mapMouseButton(2);
                this.lastTapTime = 0; // 长按打断双击序列
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] LONGPRESS->RIGHTCLICK tl=%{public}d elapsed=%{public}dms px=(%{public}d,%{public}d)',
                  this.pendingTl, elapsed, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
              } else {
                // 短按 → 左键 或 双击
                const btnLeft = mapMouseButton(1);
                const now = Date.now();
                const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
                const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < DesktopPage.DOUBLE_TAP_MS
                                    && dist < DesktopPage.DOUBLE_TAP_DIST && this.pendingTl === this.lastTapTl);

                if (isDoubleTap) {
                  // 双击: 第一次点击已由上一个单击路径发出, 这里只补发
                  // 第二次点击 (Press+Release) 构成完整的双击序列,
                  // 避免总共 3 次点击导致 explorer 同时触发"打开"和"重命名"
                  this.lastTapTime = 0;
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DOUBLE-TAP tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.lastTapPx, this.lastTapPy);
                } else {
                  // 单击
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                  this.lastTapTime = now;
                  this.lastTapPx = this.pendingPx;
                  this.lastTapPy = this.pendingPy;
                  this.lastTapTl = this.pendingTl;
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TAP->LEFTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.pendingPx, this.pendingPy);
                }
              }
            }
          } else if (ev.type === TouchType.Cancel) {
            // 系统取消触摸 (手势/通知等): 如果正在拖拽则补发 Release
            if (this.isDragging) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL-DRAG tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              const btnLeft = mapMouseButton(1);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
            } else {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL tl=%{public}d (no event sent)', this.pendingTl);
            }
            this.touchPending = false;
            this.isDragging = false;
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // ── 物理鼠标: hover 移动 + 左右键真实时序直通 ──
      // 左键: OHOS 会同时派发 mouse 事件和合成 touch 事件 — 本处理器直通
      // mouse 事件, 合成 touch 由 onTouch 里的 mouseLeftActive 守卫丢弃
      // (手势状态机的"等 Up 再补发"只服务于手指, 作用于鼠标会把静止点击
      // 压成 0ms 脉冲)。右键不合成 touch 事件, 没有本处理器则完全丢失
      // (触屏长按合成右键不受影响)。
      .onMouse((ev: MouseEvent) => {
        try {
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
            if (this.touchPending || this.isDragging || this.twoFingerActive) return;
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
            testNapi.sendPointerEvent(tl, MouseAction.Move, px, py, 0, ev.rawDeltaX, ev.rawDeltaY);
            return;
          }
          if (ev.action !== MouseAction.Press && ev.action !== MouseAction.Release) return;
          const px = Math.round(vp2px(ev.x));
          const py = Math.round(vp2px(ev.y));
          // ── 物理鼠标左键: 真实时序直通 (按下即转发, 不等手势判定) ──
          // OHOS 对左键同时派发 mouse 事件和合成 touch 事件; 合成 touch 走
          // onTouch 手势状态机会把静止点击压成 Press+Release 同刻脉冲
          // (dinput 轮询类游戏 PAL2 因此看不见静止点击), 长按还会被误判为
          // 右键。这里直通真实时序, 并用 mouseLeftActive 抑制合成 touch。
          if (ev.button === MouseButton.Left) {
            const btnLeft = mapMouseButton(1);
            if (ev.action === MouseAction.Press) {
              this.mouseLeftActive = true;
              // 竞态: 合成 touch 可能先一步进入 PENDING/DRAG, 静默清除
              // (其后续 Move/Up 会被 mouseLeftActive 拦截, 不会双发)
              this.touchPending = false;
              this.isDragging = false;
              const tl: number = testNapi.findToplevelAt(px, py) as number;
              if (tl <= 0) {
                this.leftBtnTl = 0;
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT skipped (no toplevel at %{public}d,%{public}d)', px, py);
                return;
              }
              this.leftBtnTl = tl;
              testNapi.raiseToplevel(tl);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT press tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
              testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, btnLeft, 0, 0, true);
            } else {
              if (this.leftBtnTl <= 0) {
                this.mouseLeftActive = false;
                return;
              }
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT release tl=%{public}d px=(%{public}d,%{public}d)', this.leftBtnTl, px, py);
              testNapi.sendPointerEvent(this.leftBtnTl, MouseAction.Release, px, py, btnLeft);
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
            testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, btnCode, 0, 0, true);
          } else {
            const btnTl = isRight ? this.rightBtnTl : this.midBtnTl;
            if (btnTl <= 0) return;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s release tl=%{public}d px=(%{public}d,%{public}d)', btnName, btnTl, px, py);
            testNapi.sendPointerEvent(btnTl, MouseAction.Release, px, py, btnCode);
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
        testNapi.sendKeyEvent(rootId, code, ev.type === KeyType.Down);
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
    }
    .width('100%').height('100%').backgroundColor('#000')
  }
}

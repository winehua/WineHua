import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputSettingsService } from '../service/InputSettingsService';
import { InputDeviceMapper } from '../service/InputDeviceMapper';
import { EVDEV_KEY_ENTER, OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  // 缓存创建时的 rootId: onSurfaceDestroyed 回调时 WineWindowManager 的
  // desktopRootId 可能已被清零 (桌面退出流程先清 rootId 再卸载本组件),
  // 用缓存值才能配对销毁 renderer — 否则旧 root 的 renderer 泄漏, 热重启
  // 后新旧两个 renderer 抢同一 root 的合成帧, 表现为画面「滞后一拍」
  private lastRootId: number = 0;
  // 悬浮窗放大期间 root resize 节流: onSurfaceChanged 连续波动 (沉浸生效
  // 前后两拍中间值, 如 2584x1644→2584x1828) 只记最终值, 停止 150ms 后再
  // 一次性 notifyToplevelResize — 见 deferRootResize 注释
  private pendingRootW: number = 0;
  private pendingRootH: number = 0;
  private rootResizeTimer: number = -1;
  private static readonly ROOT_RESIZE_DEBOUNCE_MS: number = 150;

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
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight);
        // 启动器可见 (窗口退出全屏) 时 surface 高度随系统栏波动 (1840→1683):
        // 跳过 root resize, 否则中间态高度会缩小 Wine 桌面, 且退出后残留
        // 进 output, 热重启桌面高度出错 (上下被裁)
        if (WineWindowManager.getInstance().desktopLauncherVisible) {
          this.cancelDeferredRootResize();
          return;
        }
        // 悬浮窗放大期间 XComponent 尺寸带中间波动 (沉浸生效前后两拍,
        // 如 2584x1644→2584x1828): 逐拍下发会让 Wine 桌面被中间值 resize,
        // Wine 静止不重绘时画面停在中间值黑边 (旧帧 vs 中间 surface)。
        // 节流: 连续变化只记最新值, 停止 150ms 后一次性下发 — Wine 桌面
        // 只被最终尺寸改一次, 不经过中间值。
        this.deferRootResize(rect.surfaceWidth, rect.surfaceHeight);
      } catch (_) {}
    }
  }

  // root (Wine 桌面) resize 节流: onSurfaceChanged 连续触发期间记最新尺寸,
  // 停止 ROOT_RESIZE_DEBOUNCE_MS 后才真正 notifyToplevelResize 一次。中间值
  // 不进 Wine 桌面, 避免 Wine 帧尺寸与 surface 中间值不匹配的左右黑边。
  private deferRootResize(w: number, h: number): void {
    this.pendingRootW = w;
    this.pendingRootH = h;
    if (this.rootResizeTimer >= 0) {
      clearTimeout(this.rootResizeTimer);
    }
    this.rootResizeTimer = setTimeout(() => {
      this.rootResizeTimer = -1;
      // 节流期间 root 可能重启重建 (getDesktopRootId 变化), 用当前 root 下单
      const rootIdNow = WineWindowManager.getInstance().getDesktopRootId();
      // 统一经 getEffectiveScale, 不再硬编码 2.0
      const s = WineWindowManager.getInstance().getEffectiveScale();
      if (rootIdNow > 0) {
        hilog.info(DOMAIN, 'LIFE', '[XComp] root resize flush rootId=%{public}d %{public}dx%{public}d',
          rootIdNow, Math.round(this.pendingRootW / s), Math.round(this.pendingRootH / s));
        testNapi.notifyToplevelResize(rootIdNow,
          Math.round(this.pendingRootW / s), Math.round(this.pendingRootH / s));
      }
    }, DesktopController.ROOT_RESIZE_DEBOUNCE_MS);
  }

  private cancelDeferredRootResize(): void {
    if (this.rootResizeTimer >= 0) {
      clearTimeout(this.rootResizeTimer);
      this.rootResizeTimer = -1;
    }
  }

  onSurfaceDestroyed(_s: string): void {
    // surface 销毁 = 挂起 root resize 永不 flush (不卸 timer 会迟到补发
    // 或挂在失效 root 上)
    this.cancelDeferredRootResize();
    const cached = this.lastRootId;
    this.lastRootId = 0;
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed lastRootId=%{public}d', cached);
    if (cached > 0) {
      try { testNapi.destroyRenderer(cached); } catch (_) {}
    }
  }
}

// 原桌面页内容原样迁入 (Step 3.5: 双 Ability → 单 Ability 三层页面,
// @Entry 页 → Index 内常驻组件)。XComponent id 保持 'wine_desktop_xc' 不变。
@Component
export struct DesktopLayer {
  // 悬浮窗态 (Index 桌面层容器处于小窗缩略显示): 桌面 XComponent 所有输入
  // 一律旁路 — 点击进全屏/拖动由 Index 悬浮交互层统一处理, 本组件不转发
  // 输入给 Wine (悬浮态小窗缩略渲染 + 输入命中映射由 renderer letterbox
  // ComputeFitRect 保证, 全屏态恢复后正常)。
  // 必须 @Prop 单向同步: 用普通属性时, 悬浮↔全屏切换 (Index 容器尺寸变但
  // 本组件实例不重建) floating 停留在旧值 true, 全屏态输入被持续旁路 —
  // 表现为"桌面在更新但点不动"。
  @Prop floating: boolean = false;
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
  // 回车 down 时间戳 + 补发窗口: 与 WineWindow.ets 同款兜底 (软键盘/IME
  // 系统吞 KeyDown 只派 KeyUp → 无配对 up 时补发 down+up)
  private lastEnterDownTs: number = 0;
  private static readonly ENTER_SYNTH_WINDOW_MS: number = 1000;
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
  // 双指点击↔滚动判定阈值 (物理px): > DRAG_THRESHOLD(落指抖动) < SCROLL_STEP_PX(滚动生效步)
  private static readonly TWO_FINGER_TAP_PX: number = 20;
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

  // ── 触摸模拟鼠标 (触控板模式, 悬浮条开关; InputSettingsService 镜像) ──
  // 开启后单指手势状态机整体旁路: 滑动=相对位移移虚拟光标, 点按=左键,
  // 长按=右键, 双击后按住拖动=拖拽, 双指拖动=滚动(作用于光标处)。
  // 两套语义不能混用 — 直接触控"指哪点哪", 触控板"光标在哪点哪"。
  @State private touchpadOn: boolean = false;
  // 虚拟光标指示点位置 (vp, position 左上角语义); 屏外负值=不显示
  @State private cursorVpX: number = -100;
  @State private cursorVpY: number = -100;
  private vcX: number = 0;   // 虚拟光标 (物理 px)
  private vcY: number = 0;
  private vcInit: boolean = false;
  private scrWPx: number = 0;
  private scrHPx: number = 0;
  private cursorSpeed: number = 1.0;
  private input: InputSettingsService = InputSettingsService.getInstance();
  // 触控板手势状态 (与直接触控状态机字段互不共用)
  private tpDown: boolean = false;
  private tpDownX: number = 0;   // Down 时触摸点 (vp)
  private tpDownY: number = 0;
  private tpLastX: number = 0;
  private tpLastY: number = 0;
  private tpDownTime: number = 0;
  private tpMoved: boolean = false;
  private tpDragArmed: boolean = false;  // 双击后按住: 拖拽预备
  private tpDragging: boolean = false;
  private tpDragTl: number = 0;
  private tpLastTapTime: number = 0;
  private tpLastTapVcX: number = 0;
  private tpLastTapVcY: number = 0;
  private tpTwoFinger: boolean = false;
  private tpTwoFingerLastMidX: number = 0;
  private tpTwoFingerLastMidY: number = 0;
  private tpScrollAccumX: number = 0;
  private tpScrollAccumY: number = 0;
  private tpTwoFingerDownTime: number = 0; // 进入双指模式时刻 (TP-TWO-TAP 日志打点用)
  private tpScrollFired: boolean = false;  // 本次双指已发过滚动事件 (抬指不再判右键 — 累积会被消费, 残量可能 < 点击阈值)

  aboutToAppear(): void {
    this.input.subscribe(this.mirrorInput);
    this.mirrorInput();
  }

  private mirrorInput = (): void => {
    this.touchpadOn = this.input.touchpadEnabled;
    this.cursorSpeed = this.input.cursorSpeed;
    if (this.touchpadOn && !this.vcInit) {
      // 虚拟光标初始位置: 屏幕中心
      this.ensureScreenPx();
      this.vcX = this.scrWPx / 2;
      this.vcY = this.scrHPx / 2;
      this.vcInit = true;
      this.syncCursorIndicator();
    }
  }

  private ensureScreenPx(): void {
    if (this.scrWPx > 0) return;
    try {
      const d = display.getDefaultDisplaySync();
      this.scrWPx = d.width;
      this.scrHPx = d.height;
    } catch (_) {
      this.scrWPx = 1280;
      this.scrHPx = 800;
    }
  }

  private syncCursorIndicator(): void {
    // 指示点为 14vp 圆, position 是左上角坐标 → 减半径使圆心对正光标
    this.cursorVpX = px2vp(this.vcX) - 7;
    this.cursorVpY = px2vp(this.vcY) - 7;
  }

  private tlAtCursor(): number {
    return (testNapi.findToplevelAt(Math.round(this.vcX), Math.round(this.vcY)) as number) ?? 0;
  }

  private clickAtCursor(btn: number): void {
    const tl = this.tlAtCursor();
    if (tl <= 0) return;
    testNapi.raiseToplevel(tl);
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    testNapi.sendPointerEvent(tl, MouseAction.Press, x, y, btn);
    testNapi.sendPointerEvent(tl, MouseAction.Release, x, y, btn);
  }

  private resetTouchpadState(): void {
    if (this.tpDragging && this.tpDragTl > 0) {
      testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
        Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
    }
    this.tpDown = false;
    this.tpMoved = false;
    this.tpDragArmed = false;
    this.tpDragging = false;
    this.tpDragTl = 0;
    this.tpTwoFinger = false;
    this.tpTwoFingerDownTime = 0;
    this.tpScrollFired = false;
    this.tpLastTapTime = 0;
  }

  // 触控板模式 onTouch 总入口 (touchpadOn 时替代单/双指手势状态机)
  private handleTouchpad(ev: TouchEvent): void {
    // 物理鼠标合成 touch 旁路 (与直接触控同一守卫; onMouse Press 里负责清态)
    if (this.mouseLeftActive) return;
    this.ensureScreenPx();

    // 双指: 滚动作用于光标处 (触控板语义), 并打断单指/拖拽
    if (ev.touches.length >= 2) {
      let midX = 0, midY = 0;
      for (let i = 0; i < ev.touches.length; i++) {
        midX += vp2px(ev.touches[i].x);
        midY += vp2px(ev.touches[i].y);
      }
      midX /= ev.touches.length;
      midY /= ev.touches.length;
      if (!this.tpTwoFinger) {
        if (this.tpDragging && this.tpDragTl > 0) {
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDown = false;
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpTwoFinger = true;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        this.tpScrollAccumX = 0;
        this.tpScrollAccumY = 0;
        this.tpScrollFired = false;
        this.tpTwoFingerDownTime = Date.now();
      } else {
        this.tpScrollAccumY += this.tpTwoFingerLastMidY - midY;
        this.tpScrollAccumX += midX - this.tpTwoFingerLastMidX;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        // 累积触及滚动步立即锁定"滚动手势": 后续抬指不判右键
        if (Math.abs(this.tpScrollAccumY) >= DesktopLayer.SCROLL_STEP_PX
          || Math.abs(this.tpScrollAccumX) >= DesktopLayer.SCROLL_STEP_PX) {
          this.tpScrollFired = true;
        }
        const tl = this.tlAtCursor();
        if (tl > 0) {
          while (Math.abs(this.tpScrollAccumY) >= DesktopLayer.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumY > 0 ? 1 : -1;
            testNapi.sendScrollEvent(tl, 0, sign, DesktopLayer.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumY -= sign * DesktopLayer.SCROLL_STEP_PX;
          }
          while (Math.abs(this.tpScrollAccumX) >= DesktopLayer.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumX > 0 ? 1 : -1;
            testNapi.sendScrollEvent(tl, 1, sign, DesktopLayer.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumX -= sign * DesktopLayer.SCROLL_STEP_PX;
          }
        }
      }
      return;
    }
    if (this.tpTwoFinger) {
      this.tpTwoFinger = false;
      // 双指抬起判定: 中点净位移 < TWO_FINGER_TAP_PX → 模拟右键 (与单指长按同效果);
      // 净位移 >= 阈值 → 滚动手势, 不起右键 (累积满 40px 的已在滚动分支发过)
      const dist = Math.abs(this.tpScrollAccumX) + Math.abs(this.tpScrollAccumY);
      if (!this.tpScrollFired && dist < DesktopLayer.TWO_FINGER_TAP_PX) {
        const tl = this.tlAtCursor();
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TP-TWO-TAP→RCLICK tl=%{public}d moved=%{public}d hold=%{public}dms',
          tl, dist, Date.now() - this.tpTwoFingerDownTime);
        this.clickAtCursor(mapMouseButton(2));
      }
      return;
    }

    const t = ev.changedTouches[0];
    if (!t) return;

    if (ev.type === TouchType.Down) {
      this.tpDown = true;
      this.tpMoved = false;
      this.tpDownX = t.x;
      this.tpDownY = t.y;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      this.tpDownTime = Date.now();
      // 双击后按住 → 拖拽预备 (标准触控板 drag 手势: 第二次 Down 且光标没怎么挪)
      const dvc = Math.abs(this.vcX - this.tpLastTapVcX) + Math.abs(this.vcY - this.tpLastTapVcY);
      this.tpDragArmed = this.tpLastTapTime > 0
        && (Date.now() - this.tpLastTapTime) < DesktopLayer.DOUBLE_TAP_MS
        && dvc < DesktopLayer.DOUBLE_TAP_DIST * 2;
    } else if (ev.type === TouchType.Move) {
      if (!this.tpDown) return;
      const dxPx = vp2px(t.x - this.tpLastX) * this.cursorSpeed;
      const dyPx = vp2px(t.y - this.tpLastY) * this.cursorSpeed;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      if (dxPx == 0 && dyPx == 0) return;
      if (!this.tpMoved) {
        const moved = Math.abs(vp2px(t.x - this.tpDownX)) + Math.abs(vp2px(t.y - this.tpDownY));
        if (moved >= DesktopLayer.DRAG_THRESHOLD) {
          this.tpMoved = true;
        }
      }
      this.vcX = Math.min(Math.max(this.vcX + dxPx, 0), this.scrWPx);
      this.vcY = Math.min(Math.max(this.vcY + dyPx, 0), this.scrHPx);
      this.syncCursorIndicator();
      if (this.tpDragArmed && !this.tpDragging && this.tpMoved) {
        // 拖拽开始: 在光标处按下左键
        this.tpDragging = true;
        this.tpDragTl = this.tlAtCursor();
        if (this.tpDragTl > 0) {
          testNapi.raiseToplevel(this.tpDragTl);
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Press,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
      }
      const moveTl = this.tpDragging ? this.tpDragTl : this.tlAtCursor();
      if (moveTl > 0) {
        testNapi.sendPointerEvent(moveTl, MouseAction.Move,
          Math.round(this.vcX), Math.round(this.vcY), 0);
      }
    } else if (ev.type === TouchType.Up) {
      if (!this.tpDown) return;
      this.tpDown = false;
      if (this.tpDragging) {
        if (this.tpDragTl > 0) {
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpLastTapTime = 0;
        return;
      }
      this.tpDragArmed = false;
      if (!this.tpMoved) {
        const elapsed = Date.now() - this.tpDownTime;
        if (elapsed >= DesktopLayer.LONG_PRESS_MS) {
          this.clickAtCursor(mapMouseButton(2));
          this.tpLastTapTime = 0;
        } else {
          this.clickAtCursor(mapMouseButton(1));
          this.tpLastTapTime = Date.now();
          this.tpLastTapVcX = this.vcX;
          this.tpLastTapVcY = this.vcY;
        }
      }
    } else if (ev.type === TouchType.Cancel) {
      this.resetTouchpadState();
    }
  }

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
          // 悬浮窗态: 输入旁路 (点击/拖动由 Index 悬浮交互层接管)
          if (this.floating) {
            return;
          }
          // 触控板模式: 走独立相对位移语义, 旁路下方直接触控状态机
          if (this.touchpadOn) {
            this.handleTouchpad(ev);
            return;
          }
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
                while (Math.abs(this.twoFingerAccumY) >= DesktopLayer.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumY > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 0, sign, DesktopLayer.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumY -= sign * DesktopLayer.SCROLL_STEP_PX;
                  if (++this.scrollLogCounter % 5 === 0) {
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-V tl=%{public}d',
                      this.twoFingerTl);
                  }
                }
                // 水平滚动
                while (Math.abs(this.twoFingerAccumX) >= DesktopLayer.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumX > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 1, sign, DesktopLayer.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumX -= sign * DesktopLayer.SCROLL_STEP_PX;
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

            if (this.touchPending && dist >= DesktopLayer.DRAG_THRESHOLD) {
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

              if (elapsed >= DesktopLayer.LONG_PRESS_MS) {
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
                const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < DesktopLayer.DOUBLE_TAP_MS
                                    && dist < DesktopLayer.DOUBLE_TAP_DIST && this.pendingTl === this.lastTapTl);

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
          // 诊断 (临时): 非 Move 事件在处理器入口记录硬件时间戳 (排查 explorer
          // 单击变双击, 见 DesktopWindow.ets 同注释)
          if (ev.action !== MouseAction.Move) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-RAW a=%{public}d btn=%{public}d ts=%{public}d',
              ev.action, ev.button, Math.round(ev.timestamp % 1000000000));
          }
          // 悬浮窗态: 输入旁路 (物理鼠标同样不转发给 Wine)
          if (this.floating) {
            return;
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
            // rawDelta 按设备校准 (与 WineWindow 同规则: 鼠标 2.5 / 触控板 0.75)
            // — 收口 ArkTS 时此处漏乘, 桌面模式相对模式增量只有融合窗口的
            // 1/2.5, 同一鼠标两种手感; 绝对模式有差分自标定, 不受影响
            let rawDx = ev.rawDeltaX ?? 0;
            let rawDy = ev.rawDeltaY ?? 0;
            const devId = ev.deviceId ?? 0;
            if (devId > 0) {
              // 设备校准 × 用户倍率 (relMouseSpeed) 均在 getScale 内乘入
              const s = InputDeviceMapper.getInstance().getScale(devId);
              rawDx *= s;
              rawDy *= s;
            }
            testNapi.sendPointerEvent(tl, MouseAction.Move, px, py, 0, rawDx, rawDy);
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
              // (其后续 Move/Up 会被 mouseLeftActive 拦截, 不会双发);
              // 触控板模式同理清态 (合成 touch 可能先一步 Down)
              this.touchPending = false;
              this.isDragging = false;
              this.resetTouchpadState();
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
        if (this.floating) return;  // 悬浮窗态: 输入旁路
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0) return;
        ev.stopPropagation();
        const code = OH2EVDEV[ev.keyCode] ?? 0;
        if (code <= 0) return;
        const pressed = ev.type === KeyType.Down;
        // 回车补发 (同 WineWindow.ets): 系统吞 KeyDown 只派 KeyUp 时,
        // 无配对 up 补发 down+up, 保证桌面模式 Wine 收到完整按键
        if (code === EVDEV_KEY_ENTER) {
          if (pressed) {
            this.lastEnterDownTs = Date.now();
          } else if (Date.now() - this.lastEnterDownTs > DesktopLayer.ENTER_SYNTH_WINDOW_MS) {
            hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] ENTER SYNTH down (missing down)');
            testNapi.sendKeyEvent(rootId, EVDEV_KEY_ENTER, true);
          }
        }
        testNapi.sendKeyEvent(rootId, code, pressed);
      })

      // ── 滚轮事件 ──
      .onAxisEvent((ev?: AxisEvent) => {
        if (this.floating) return;  // 悬浮窗态: 输入旁路
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

      // ── 触控板虚拟光标指示 (不响应触摸, 纯展示) ──
      if (this.touchpadOn) {
        // 描边不用纯白: 白色背景 (记事本等) 下光标会整个消失, 用浅灰保证可辨
        Circle({ width: 14, height: 14 })
          .fill('#66FFFFFF')
          .stroke('#FFB3B3B3')
          .strokeWidth(1.5)
          .position({ x: this.cursorVpX, y: this.cursorVpY })
          .hitTestBehavior(HitTestMode.None)
      }
    }
    .width('100%').height('100%').backgroundColor('#000')
  }
}

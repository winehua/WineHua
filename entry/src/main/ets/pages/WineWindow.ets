import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { EVDEV_KEY_ENTER, OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputDeviceMapper } from '../service/InputDeviceMapper';
import { FusionWindowManager } from '../service/FusionWindowManager';
import { KeyboardOverlay } from '../components/KeyboardOverlay';

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
      testNapi.resizeRenderer(this.toplevelId, rect.surfaceWidth, rect.surfaceHeight, BigInt(surfaceId));
      const mgr = WineWindowManager.getInstance();
      // surface 是物理像素, Wine 需要逻辑坐标 (统一经 getEffectiveScale, 不再硬编码 2.0)
      const s = mgr.getEffectiveScale();
      const w = Math.round(rect.surfaceWidth / s);
      const h = Math.round(rect.surfaceHeight / s);
      // 拖拽缩放中 → configure 带 RESIZING, Wine 才采用新尺寸 (见 setResizeDragging)
      const resizing = mgr.isResizeDragging(this.toplevelId);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceChanged tl=%{public}d → notifyToplevelResize(%{public}d, %{public}d, resize=%{public}s)',
        this.toplevelId, w, h, resizing ? 'yes' : 'no');
      testNapi.notifyToplevelResize(this.toplevelId, w, h, resizing);
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceChanged resizeRenderer: %{public}s', JSON.stringify(e));
    }
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceDestroyed tl=%{public}d surfaceId=%{public}s',
      this.toplevelId, surfaceId);
    try {
      testNapi.destroyRenderer(this.toplevelId, BigInt(surfaceId));
    } catch (e) {
      hilog.error(DOMAIN, 'WineWindow', 'ERR onSurfaceDestroyed destroyRenderer: %{public}s', JSON.stringify(e));
    }
  }
}

@Entry
@Component
struct WineWindow {
  controller: WineXComponentController = new WineXComponentController();
  // toplevelId 由承载方 (WineWindowAbility/FusionWindowManager/ModalWindowManager)
  // 在 loadContent 前 setPendingToplevel 入队, aboutToAppear 出队取得
  // (与 PC 多窗口同款, PC 实测 OK)。
  @State toplevelId: number = 0;
  @State surfacePxW: number = 1;
  @State surfacePxH: number = 1;
  // ── 顶条 (任务 #129 回退路线: 几何补偿式自绘顶条) ──
  // stripVp = px2vp(stripPxOf(tl)), 判据/换算单点在 FusionWindowManager;
  // 0 = 不渲染。判据内含 modal 承载 (#167 续: x11 下 modal 对话框也有
  // 顶条); wayland incumbent / WWA / popup 形态恒 0 零变化。
  @State stripVp: number = 0;
  @State title: string = '';
  private stripMoving: boolean = false;
  private stripStartPx: number = 0;
  private stripStartPy: number = 0;
  private stripMgrListener: (() => void) | null = null;

  // ── 触控状态机 (移植自 DesktopWindow.ets 桌面模式, 单窗口简化) ──
  // 单指: 短按→左键点击 / 长按→右键 / 移动超阈值→按住左键拖拽; 双指→滚轮
  // 物理鼠标左键的合成 touch: onMouse 已按真实时序直通, mouseLeftActive 抑制
  // (若进状态机, 静止点击会被压成 Press+Release 同刻脉冲, dinput 轮询不可见)
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  // M4d RF#3: 顶条拖动 = 移动承载窗本身 (非 wine 内容拖拽)。系统拖拽接管后
  // Move/Up 不得再注入 wine (合成事件会打断系统拖拽 / 产生幽灵按钮)。
  private windowMoveDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private mouseLeftActive: boolean = false;
  private motionLogCounter: number = 0;
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  // M4d RF#3: fusion subWindow 承载的"标题栏" = 窗口顶条。x11 下 wine 的
  // X 窗 = win32 rect 减 WM 装饰区 (D52 created 坐标 = 原点 + caption+border
  // 偏移, 实测 2026-10-11), caption 属于缺席的 WM —— guest 像素里没有标题栏,
  // HTCAPTION/_NET_WM_MOVERESIZE 触摸路径结构性不可达; 顶条实为 wine 菜单栏
  // (高度 ≈ guest 顶部 20 逻辑像素)。短按仍走点击 (菜单可开),
  // 仅超过拖动阈值的顶条手势转为移动承载窗 (startMoving, 与 wayland Pad
  // incumbent 同一系统拖拽通道); wine 不感知位置 (managed 模式位置由
  // ArkTS/系统权威, popup 偏移按父相对量不受影响)。
  private static readonly DRAG_STRIP_LOGICAL_PX: number = 20; // guest 逻辑像素

  // M4d-review Minor#3: 物理阈值按有效 scale 现算 (原硬编码 40 只对 scale-2
  // 设备成立; displayScale/scaleBoost 不同的设备上过小会把菜单栏拖动误判为
  // 内容拖拽, 过大会吞掉客户区)
  private dragStripPx(): number {
    return Math.round(WineWindow.DRAG_STRIP_LOGICAL_PX *
      WineWindowManager.getInstance().getEffectiveScale());
  }
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
    // (与 PC 多窗口同款, PC 实测 OK)
    this.toplevelId = testNapi.getCurrentToplevelId() as number;
    // 同步设置给 controller: 保证 onSurfaceCreated 拿到正确的 toplevelId
    this.controller.toplevelId = this.toplevelId;
    // ── 顶条初始化 (任务 #129): stripPxOf > 0 = x11 fusion 子窗或 x11 modal
    // 承载 (#167 续) ──
    // wayland / WWA / popup 形态 stripVp 恒 0: 不订阅不渲染, 渲染树
    // 与交互与改动前一致 (隔离红线; 判据单点在 FusionWindowManager)。
    const stripPx = FusionWindowManager.getInstance().stripPxOf(this.toplevelId);
    if (stripPx > 0) {
      this.stripVp = px2vp(stripPx);
      const wm = WineWindowManager.getInstance();
      this.title = wm.getWindowTitle(this.toplevelId);
      // title 事件 → applyEvent 记 e.title (modal 承载走 onTitle) →
      // notify (仅顶条存在时发) → 本刷新
      this.stripMgrListener = () => {
        this.title = WineWindowManager.getInstance().getWindowTitle(this.toplevelId);
        this.stripVp = px2vp(FusionWindowManager.getInstance().stripPxOf(this.toplevelId));
      };
      wm.subscribe(this.stripMgrListener);
      hilog.info(DOMAIN, 'WineWindow', 'strip active tl=%{public}d px=%{public}d vp=%{public}d',
        this.toplevelId, stripPx, Math.round(this.stripVp));
    }
    hilog.info(DOMAIN, 'WineWindow', 'aboutToAppear tl=%{public}d', this.toplevelId);
  }

  aboutToDisappear(): void {
    if (this.stripMgrListener !== null) {
      WineWindowManager.getInstance().unsubscribe(this.stripMgrListener);
      this.stripMgrListener = null;
    }
  }

  // ── 顶条交互 (任务 #129 回退路线) ──
  // 三键驱动 M4b 既有动作链 (applyEvent fusion 分支同款落点), 不新造链:
  //   min   → wine 'minimized' 事件同款 → FWM.hide (minimize 近似)
  //   max   → 'maximized'/'unmaximized' 同款 → FWM.fullscreen ↔ restore
  //   close → RF#3 统一通道 sendToplevelClose (x11 分支 = WM_DELETE_WINDOW,
  //           wine 收 WM_CLOSE 走 win32 关窗语义, 保存提示保留, 非强杀)
  private onStripMin(): void {
    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-MIN tl=%{public}d', this.toplevelId);
    FusionWindowManager.getInstance().hide(this.toplevelId);
  }

  private onStripMax(): void {
    const fw = FusionWindowManager.getInstance();
    const fs = fw.isFullscreen(this.toplevelId);
    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-MAX tl=%{public}d → %{public}s',
      this.toplevelId, fs ? 'restore' : 'fullscreen');
    if (fs) {
      fw.restore(this.toplevelId);
    } else {
      fw.fullscreen(this.toplevelId);
    }
  }

  private onStripClose(): void {
    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-CLOSE tl=%{public}d', this.toplevelId);
    try {
      testNapi.sendToplevelClose(this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-CLOSE fail: %{public}s', JSON.stringify(e));
    }
  }

  // 键盘事件共享体 (Stack 与外层 Column 两挂点共用, 见 build 注释):
  // 首次进入事件即 stopPropagation, 两挂点不会双发 (Stack 在上, Column
  // 为其祖先 —— 焦点在 Stack 时事件止于 Stack; 焦点在 Column 时 Stack
  // 不在冒泡链上)。
  private handleWineKey(event: KeyEvent): void {
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
  }

  // 顶条触摸: 轻按交给按钮 onClick; 超过拖动阈值 = 系统拖拽接管
  // (startMoving, f43c764 同通道 — 顶条即承载窗拖动区, 阈值同款物理 px)。
  private onStripTouch(ev: TouchEvent): void {
    if (this.toplevelId <= 0) return;
    if (ev.type === TouchType.Down) {
      try {
        WineWindowManager.getInstance().raiseWindowGroup(this.toplevelId);
      } catch (_) { /* ignore */ }
      const t = ev.changedTouches[0];
      if (!t) return;
      this.stripStartPx = Math.round(vp2px(t.x));
      this.stripStartPy = Math.round(vp2px(t.y));
      this.stripMoving = false;
      return;
    }
    if (ev.type === TouchType.Move) {
      if (this.stripMoving) return;
      const t = ev.changedTouches[0];
      if (!t) return;
      const px = Math.round(vp2px(t.x));
      const py = Math.round(vp2px(t.y));
      const dist = Math.abs(px - this.stripStartPx) + Math.abs(py - this.stripStartPy);
      if (dist < WineWindow.DRAG_THRESHOLD) return;
      const win = WineWindowManager.getInstance().getWindow(this.toplevelId);
      if (!win) return;
      this.stripMoving = true;
      try {
        win.startMoving();
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-DRAG startMoving tl=%{public}d', this.toplevelId);
      } catch (e) {
        this.stripMoving = false;
        hilog.warn(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-DRAG startMoving failed: %{public}s',
          JSON.stringify(e));
      }
      return;
    }
    // Up / Cancel
    this.stripMoving = false;
    // 按钮上起手的拖动被系统拖拽接管时, 按钮子节点收不到 Up —— 按压态在此兜底清理
    this.stripBtnPress = -1;
  }

  // 自绘顶条 (任务 #129 回退路线, stripVp > 0 = x11 fusion 子窗或 modal
  // 承载): Win11
  // caption 观感 —— 46vp 方形按钮 + 10vp 矢量细线图标 (Path, 1vp 描边, 取代
  // 字符字形 ─□×), 悬停/按压灰底, close 悬停红底白叉; 固定亮色不跟系统深浅色
  // (Win 风格基线)。交互模型不变: 顶条区 = 拖动区 (onStripTouch 阈值→
  // startMoving), 三键沿用既有 min/max/close 动作链; 顶条是独立节点, 其上的
  // 点击不落 guest, 输入零坐标换算 (承载窗增高 + XComponent 下移的几何补偿
  // 在 FusionWindowManager 侧完成, plan RF#1)。
  @State stripBtnHover: number = -1;  // 鼠标悬停的按钮 idx (触摸无悬停态)
  @State stripBtnPress: number = -1;  // 按压中的按钮 idx
  private static readonly STRIP_BTN_W: number = 35; // caption 按钮宽 (初版 46, 用户先后裁定 40、随顶条 32→28 等比收至 35 = 40×28/32, 保持 1.25 宽高比)

  // 10vp 图标在 35×stripVp 按钮内居中 (Win11 caption 字形规格: 10×10, 1vp 描边)。
  // 坑: Path.commands 坐标单位是 px (物理像素), 与属性单位 (vp) 不同轨 ——
  // 几何按 vp 设计, 拼命令串时统一 ×vp2px, 否则高密度屏上字形缩小 density 倍
  // (实测 2.5x 屏上 10vp 字形只画出 ~10px, 2026-10-17)。
  private stripIconCmd(idx: number): string {
    const k = vp2px(1);
    const cy = this.stripVp / 2 * k;
    const c = WineWindow.STRIP_BTN_W / 2 * k;
    const r = 5 * k; // 字形半宽 (10vp 方)
    const x0 = c - r;
    const x1 = c + r;
    if (idx === 0) { // 最小化: 中线横杠
      return `M${x0} ${cy} L${x1} ${cy}`;
    }
    if (idx === 1) { // 最大化: 方框
      return `M${x0} ${cy - r} L${x1} ${cy - r} L${x1} ${cy + r} L${x0} ${cy + r} Z`;
    }
    const i = 0.5 * k; // 关闭: 9vp 叉 (叉角内缩 0.5vp)
    return `M${x0 + i} ${cy - r + i} L${x1 - i} ${cy + r - i} ` +
      `M${x1 - i} ${cy - r + i} L${x0 + i} ${cy + r - i}`;
  }

  // 按钮底色: 悬停 #E5E5E5 / 按压 #CCCCCC; close 悬停 #C42B1C / 按压 #B3271A
  private stripBtnBg(idx: number): ResourceColor {
    if (idx === 2) {
      if (this.stripBtnPress === idx) return '#B3271A';
      if (this.stripBtnHover === idx) return '#C42B1C';
      return Color.Transparent;
    }
    if (this.stripBtnPress === idx) return '#CCCCCC';
    if (this.stripBtnHover === idx) return '#E5E5E5';
    return Color.Transparent;
  }

  // close 悬停/按压时字形翻白 (Win11 标志性), 其余恒近黑
  private stripBtnStroke(idx: number): ResourceColor {
    return (idx === 2 && (this.stripBtnHover === idx || this.stripBtnPress === idx))
      ? '#FFFFFF' : '#1B1B1B';
  }

  @Builder
  stripCaptionBtn(idx: number) {
    Path()
      .commands(this.stripIconCmd(idx))
      .fillOpacity(0)
      .stroke(this.stripBtnStroke(idx))
      .strokeWidth(1)
      .width(WineWindow.STRIP_BTN_W)
      .height('100%')
      .backgroundColor(this.stripBtnBg(idx))
      .onHover((h: boolean) => {
        this.stripBtnHover = h ? idx : (this.stripBtnHover === idx ? -1 : this.stripBtnHover);
      })
      .onTouch((ev: TouchEvent) => {
        // 按压视觉态; 触摸继续冒泡到父 Row 的 onStripTouch (拖动阈值判定不变)
        if (ev.type === TouchType.Down) {
          this.stripBtnPress = idx;
        } else if (ev.type !== TouchType.Move && this.stripBtnPress === idx) {
          this.stripBtnPress = -1;
        }
      })
      .onClick(idx === 0 ? () => { this.onStripMin(); }
        : idx === 1 ? () => { this.onStripMax(); }
        : () => { this.onStripClose(); })
  }

  @Builder
  stripBar() {
    Row() {
      Text(this.title.length > 0 ? this.title : 'Wine')
        .fontSize(12)
        .fontColor('#1B1B1B')
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .layoutWeight(1)
        .padding({ left: 12 })
      this.stripCaptionBtn(0)
      this.stripCaptionBtn(1)
      this.stripCaptionBtn(2)
    }
    .width('100%')
    .height(this.stripVp)
    .backgroundColor('#F3F3F3')
    .onTouch((ev: TouchEvent) => { this.onStripTouch(ev); })
  }

  build() {
    if (this.stripVp > 0) {
      // x11 fusion 子窗: 顶条 + guest 内容。guest 内容在外层 Column 的
      // layoutWeight 区里保持原样 (内容树 = 非顶条分支同一 @Builder)。
      Column() {
        this.stripBar()
        Column() {
          this.wineContent()
        }
        .layoutWeight(1)
        .width('100%')
      }
      .width('100%')
      .height('100%')
      // 键盘挂点: 带顶条页面 (x11 fusion 子窗/modal 承载) 的根节点是焦点落点 (实测:
      // WinFocusMove → Column/onFocus; 仅 Stack 有 handler 时键盘全丢) ——
      // 根 Column 与 Stack 共用 handleWineKey (stopPropagation 防双发)。
      // wayland/其他形态无此 Column, Stack 即根, 行为与改动前一致。
      .onKeyEvent((event: KeyEvent) => { this.handleWineKey(event); })
    } else {
      // 非顶条形态 (wayland / WWA / modal / popup): 与改动前同一棵树
      this.wineContent()
    }
  }

  @Builder
  wineContent() {
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
              if (this.isDragging && !this.windowMoveDragging) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL-DRAG tl=%{public}d px=(%{public}d,%{public}d)',
                  this.toplevelId, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Release,
                  this.pendingPx, this.pendingPy, mapMouseButton(1));
              } else {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL tl=%{public}d (no event sent)', this.toplevelId);
              }
              this.touchPending = false;
              this.isDragging = false;
              this.windowMoveDragging = false;
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
              // 点击置顶: 主窗 + 其 popup 子窗口整组提升。WMS 焦点提升只抬主窗,
              // 不同步抬 popup → 子窗口被主窗盖住 (原白屏 bug 的根因, e51da46)。
              // 现多窗口模式的客户区已合入主窗帧 (InlineClient, 不建 popup),
              // 本调用实际保护的是菜单/下拉等浮层 popup (见 raiseWindowGroup)。
              try {
                WineWindowManager.getInstance().raiseWindowGroup(this.toplevelId);
              } catch (_) { /* ignore */ }
              // 进入 PENDING 状态: 暂不发送事件, 等判定长按/拖拽/点击
              this.touchPending = true;
              this.isDragging = false;
              this.windowMoveDragging = false;
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
                // M4d RF#3: fusion subWindow 顶条手势 = 移动承载窗 (定案见
                // DRAG_STRIP_LOGICAL_PX 注释)。短按不进此分支, 菜单点击不受影响;
                // 系统拖拽接管后 wine 收不到任何合成事件 (窗移动对 guest
                // 不可见, managed 模式位置由 ArkTS/系统权威)。
                if (this.touchStartPy <= this.dragStripPx() &&
                    WineWindowManager.getInstance().fusionUsesSubWindow()) {
                  const win = WineWindowManager.getInstance().getWindow(this.toplevelId);
                  if (win) {
                    this.touchPending = false;
                    this.isDragging = true;
                    this.windowMoveDragging = true;
                    this.lastTapTime = 0;
                    try {
                      win.startMoving();
                      hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-DRAG startMoving tl=%{public}d px=(%{public}d,%{public}d)',
                        this.toplevelId, px, py);
                    } catch (e) {
                      hilog.warn(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-DRAG startMoving failed tl=%{public}d: %{public}s',
                        this.toplevelId, JSON.stringify(e));
                      this.windowMoveDragging = false;
                      this.isDragging = false;
                      this.touchPending = true; // 回落普通内容拖拽判定
                    }
                    return;
                  }
                }
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
                if (this.windowMoveDragging) {
                  // 系统拖拽接管中: 不向 wine 注入 (见 windowMoveDragging 注释)
                  return;
                }
                // Pad 多窗口 (subWindow 承载): 触摸拖动 wine 标题栏 → wine 已发
                // move 请求 (moveGrabPending), 由 OHOS startMoving 接管系统拖拽。
                // 必须在触摸路径处理 — 本状态机的合成 mouse Move 被守卫吞掉,
                // onMouse 分支到不了; 仅子窗口承载 (Ability 窗口触屏拖不动属
                // PC 现状, PC 有鼠标/触控板)。startMoving 后系统接管, 后续
                // Move 注入自动恢复 (moveGrabPending 在 move_end 变 false)
                if (WineWindowManager.getInstance().shouldStartMoving(this.toplevelId) &&
                    WineWindowManager.getInstance().fusionUsesSubWindow()) {
                  try {
                    const win = WineWindowManager.getInstance().getWindow(this.toplevelId);
                    if (win) {
                      win.startMoving();
                      // 消费 pending: 防止拖拽期间每帧重复 startMoving (见 clearMovePending)
                      WineWindowManager.getInstance().clearMovePending(this.toplevelId);
                      hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TP-DRAG startMoving tl=%{public}d', this.toplevelId);
                      return;
                    }
                  } catch (e) { /* fall through to inject */ }
                }
                testNapi.sendPointerEvent(this.toplevelId, MouseAction.Move, px, py, 0);
                if (++this.motionLogCounter % 10 === 0) {
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-MOVE tl=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                    this.toplevelId, px, py, this.motionLogCounter);
                }
              }
              // PENDING 且未超阈值: 不做任何事 (等 Up 判定长按/点击)

            } else if (ev.type === TouchType.Up) {
              // WMS 置顶可能晚于 Down 的 raise, 松手再提一次组 (理由同 Down 分支)
              try {
                WineWindowManager.getInstance().raiseWindowGroup(this.toplevelId);
              } catch (_) { /* ignore */ }
              if (this.isDragging) {
                if (this.windowMoveDragging) {
                  // 顶条拖动 (系统拖拽接管中): 只收尾, 不向 wine 注入
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] STRIP-DRAG end tl=%{public}d px=(%{public}d,%{public}d)',
                    this.toplevelId, px, py);
                  this.isDragging = false;
                  this.windowMoveDragging = false;
                  return;
                }
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

      // 自定义虚拟键盘 (InputSettingsService.keyboardEnabled 打开时生效)
      KeyboardOverlay({ targetId: this.toplevelId })
    }
    .width('100%')
    .height('100%')
    // 底色固定黑: XComponent surface 的透明像素 (ARGB 帧在渲染器里清透明底 —
    // 异型/圆角窗口的透明区要能透出) 透出的就是本层。原为透明 (#00000000),
    // 依据"surface 不透明覆盖全部区域" — 该假设对 ARGB 帧不成立: 浅色模式下
    // 画面外区域显示系统默认底色白边 (2026-09-13 魔兽3 实测左右各 173px 白条,
    // 深色模式碰巧是黑的所以一直没暴露)。本页只有全屏 XComponent, 固定黑无
    // 副作用; 透明菜单 popup 走独立子窗口页面 (WinePopup), 不受此改动影响
    .backgroundColor('#000')
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
            // 消费 pending: 防止拖拽期间每帧重复 startMoving (见 clearMovePending)
            WineWindowManager.getInstance().clearMovePending(this.toplevelId);
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
        this.windowMoveDragging = false;
        this.twoFingerActive = false;
      }
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        // btn 用十进制: ArkTS hilog 不支持 %{public}x (渲染成乱码 '0x}x',
        // 按键无从辨认) — evdev: left=272 right=273 middle=274
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE tl=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=%{public}d',
          this.toplevelId, event.action, Math.round(px), Math.round(py), btn);
      }
      // 按输入设备类型缩放 rawDelta (相对模式增量 = rawDx × kSensRel 无标定,
      // 触控板 raw 单位约为鼠标 5~10 倍, 不缩放则"移动一点点光标跑很多");
      // 绝对模式有差分自标定, 缩放不影响结果
      // event.deviceId 类型是 number|undefined (运行时恒有, 类型标注缺省)
      let rawDx = event.rawDeltaX ?? 0;
      let rawDy = event.rawDeltaY ?? 0;
      const devId = event.deviceId ?? 0;
      if (devId > 0) {
        const s = InputDeviceMapper.getInstance().getScale(devId);
        rawDx *= s;
        rawDy *= s;
      }
      testNapi.sendPointerEvent(this.toplevelId, event.action, px, py, btn, rawDx, rawDy, true);
    })

    // -- 键盘事件 (焦点在 Stack 时; 焦点在 strip 分支的根 Column 时由
    //    build() 里的同款挂点接住 —— 实测: 带顶条的页面根节点被焦点系统
    //    选中, 键盘事件不到 Stack) --
    .onKeyEvent((event: KeyEvent) => { this.handleWineKey(event); })

    .onAreaChange((_o: Area, n: Area) => {
      this.surfacePxW = vp2px(n.width as number);
      this.surfacePxH = vp2px(n.height as number);
      hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onAreaChange tl=%{public}d w=%{public}d h=%{public}d',
        this.toplevelId, Math.round(this.surfacePxW), Math.round(this.surfacePxH));
    })
  }
}

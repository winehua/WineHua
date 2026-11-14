import { display } from '@kit.ArkUI';
import { HgsSurface } from 'hgs';
import { hilog } from '@kit.PerformanceAnalysisKit';
import hgsNapi from 'libhgs.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputSettingsService } from '../service/InputSettingsService';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

// 原桌面页内容原样迁入 (Step 3.5: 双 Ability → 单 Ability 三层页面,
// @Entry 页 → Index 内常驻组件)。XComponent id 保持 'wine_desktop_xc' 不变。
// surface 生命周期与直触/鼠标/键盘/滚轮由 WineSurface/WinePointerMachine
// 公共实现 (与 DesktopWindow/WineWindow/WinePopup 共用); 本组件保留自己的
// 差异: floating 悬浮旁路、触控板模式 (相对位移虚拟光标, 原实现原样保留)、
// 启动器可见时的尺寸反写守卫。
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
  // 创建时缓存的桌面 rootId (退出流程先清 WWM rootId, 销毁配对用)
  private lastRootId: number = 0;

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
  // 触控板手势状态 (与直接触控状态机字段互不共用 — 公共状态机在
  // WinePointerMachine 内, 触控板语义独立)
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
  private static readonly LONG_PRESS_MS: number = 500;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  private static readonly DOUBLE_TAP_MS: number = 400;
  private static readonly DOUBLE_TAP_DIST: number = 30;
  private static readonly SCROLL_STEP_PX: number = 40; // 物理像素/滚动步, winewayland.drv 只处理 discrete


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
    return (hgsNapi.findToplevelAt(Math.round(this.vcX), Math.round(this.vcY)) as number) ?? 0;
  }

  private clickAtCursor(btn: number): void {
    const tl = this.tlAtCursor();
    if (tl <= 0) return;
    hgsNapi.raiseToplevel(tl);
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    hgsNapi.sendPointerEvent(tl, MouseAction.Press, x, y, btn);
    hgsNapi.sendPointerEvent(tl, MouseAction.Release, x, y, btn);
  }

  private resetTouchpadState(): void {
    if (this.tpDragging && this.tpDragTl > 0) {
      hgsNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
        Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
    }
    this.tpDown = false;
    this.tpMoved = false;
    this.tpDragArmed = false;
    this.tpDragging = false;
    this.tpDragTl = 0;
    this.tpTwoFinger = false;
    this.tpLastTapTime = 0;
  }

  build() {
    Column() {
      HgsSurface({
        xcId: 'wine_desktop_xc',
        resolveId: () => WineWindowManager.getInstance().getDesktopRootId(),
        mouseMode: 'desktop',
        floating: this.floating,
        scaleRawDelta: true,
        onSurfaceCreatedCallback: (sid: string, id: number) => {
          this.lastRootId = id;
          try { hgsNapi.createRenderer(id, BigInt(sid)); } catch (_) {}
        },
        onSurfaceChangedCallback: (_sid: string, id: number, w: number, h: number) => {
          try {
            hgsNapi.resizeRenderer(id, w, h);
            // 启动器可见 (窗口退出全屏) 时 surface 高度随系统栏波动 (1840→1683):
            // 跳过 root resize, 否则中间态高度会缩小 Wine 桌面, 且退出后残留
            if (WineWindowManager.getInstance().desktopLauncherVisible) {
              return;
            }
            const scale = WineWindowManager.getInstance().getEffectiveScale();
            hgsNapi.notifyToplevelResize(id, Math.round(w / scale), Math.round(h / scale), false);
          } catch (_) {}
        },
        onSurfaceDestroyedCallback: (id: number) => {
          // 用创建时缓存的 rootId 配对销毁 (退出流程先清 WWM 的 rootId)
          const rid = id > 0 ? id : this.lastRootId;
          if (rid > 0) {
            try { hgsNapi.destroyRenderer(rid); } catch (_) {}
          }
          this.lastRootId = 0;
        }
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}

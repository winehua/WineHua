import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

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
        .backgroundColor('#000')
        .focusable(false)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
    .focusable(true)
    .defaultFocus(true)

    // -- 鼠标事件 --
    .onMouse((event: MouseEvent) => {
      if (this.toplevelId <= 0) return;
      const px = vp2px(event.x);
      const py = vp2px(event.y);
      const btn = mapMouseButton(event.button);
      if (event.action !== MouseAction.Move) {
        hilog.info(DOMAIN, 'WineWindow', '[Input] MOUSE tl=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=0x%{public}x',
          this.toplevelId, event.action, Math.round(px), Math.round(py), btn);
      }
      testNapi.sendPointerEvent(this.toplevelId, event.action, px, py, btn);
    })

    // -- 键盘事件 --
    .onKeyEvent((event: KeyEvent) => {
      hilog.info(DOMAIN, 'WineWindow', '[KBD-DEBUG] onKeyEvent FIRED! keyCode=%{public}d type=%{public}d tl=%{public}d',
        event.keyCode, event.type, this.toplevelId);
      if (this.toplevelId <= 0) return;
      event.stopPropagation();
      const evdevCode = OH2EVDEV[event.keyCode] ?? 0;
      if (evdevCode === 0) {
        hilog.warn(DOMAIN, 'WineWindow', '[Input] KEY unmapped ohosCode=%{public}d', event.keyCode);
        return;
      }
      const pressed = (event.type === KeyType.Down);
      hilog.info(DOMAIN, 'WineWindow', '[Input] KEY tl=%{public}d ohos=%{public}d->evdev=%{public}d %{public}s',
        this.toplevelId, event.keyCode, evdevCode, pressed ? 'DOWN' : 'UP');
      testNapi.sendKeyEvent(this.toplevelId, evdevCode, pressed);
    })

    .onAreaChange((_o: Area, n: Area) => {
      this.surfacePxW = vp2px(n.width as number);
      this.surfacePxH = vp2px(n.height as number);
    })
  }
}

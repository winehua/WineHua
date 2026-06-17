import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

const DOMAIN = 0x0000;

@Entry
@Component
struct WineWindow {
  controller: XComponentController = new XComponentController();
  @State toplevelId: number = 0;
  @State surfacePxW: number = 1;
  @State surfacePxH: number = 1;

  aboutToAppear(): void {
    this.toplevelId = testNapi.getCurrentToplevelId() as number;
    hilog.info(DOMAIN, 'WineWindow', ' aboutToAppear tl=%{public}d', this.toplevelId);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_sub_xc',
        type: XComponentType.SURFACE,
        controller: this.controller,
        libraryname: 'entry'
      })
        .width('100%')
        .height('100%')
        .backgroundColor('#000')
        .onLoad(() => {
          hilog.info(DOMAIN, 'WineWindow', ' onLoad: calling initXComponent...');
          try {
            testNapi.initXComponent();
            hilog.info(DOMAIN, 'WineWindow', 'onLoad: initXComponent returned OK');
          } catch (e) {
            hilog.error(DOMAIN, 'WineWindow', 'ERR onLoad initXComponent EXCEPTION: %{public}s', JSON.stringify(e));
          }
        })
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
      if (this.toplevelId <= 0) return;
      event.stopPropagation();
      const evdevCode = OH2EVDEV[event.keyCode] ?? 0;
      if (evdevCode === 0) {
        hilog.warn(DOMAIN, 'WineWindow', '[Input] KEY unmapped ohosCode=%{public}d', event.keyCode);
        return;
      }
      const pressed = (event.type === KeyType.Down);
      hilog.info(DOMAIN, 'WineWindow', '[Input] KEY tl=%{public}d ohos=%{public}d→evdev=%{public}d %{public}s',
        this.toplevelId, event.keyCode, evdevCode, pressed ? 'DOWN' : 'UP');
      testNapi.sendKeyEvent(this.toplevelId, evdevCode, pressed);
    })

    .onAreaChange((_o: Area, n: Area) => {
      this.surfacePxW = vp2px(n.width as number);
      this.surfacePxH = vp2px(n.height as number);
    })
  }
}

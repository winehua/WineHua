import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

@Entry
@Component
struct WineWindow {
  controller: XComponentController = new XComponentController();

  aboutToAppear(): void {
    hilog.info(DOMAIN, 'WineWindow', '▶ aboutToAppear');
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
          // ★ onLoad 在 native XComponent 加载完成后触发,
          // 此时 Init 已经调用完毕, exports 已包含正确的 XComponent
          hilog.info(DOMAIN, 'WineWindow', '▶ onLoad: calling initXComponent (post-Init)...');
          try {
            testNapi.initXComponent();
            hilog.info(DOMAIN, 'WineWindow', 'onLoad: initXComponent returned ✓');
          } catch (e) {
            hilog.error(DOMAIN, 'WineWindow', '✗ onLoad initXComponent EXCEPTION: %{public}s', JSON.stringify(e));
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}

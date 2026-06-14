import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

@Entry
@Component
struct WineWindow {
  controller: XComponentController = new XComponentController();

  aboutToAppear(): void {
    hilog.info(DOMAIN, 'WineWindow', 'sub-window page loaded, registering XComponent');
    // 注册子窗口 XComponent 的 native callback（PluginManager::Export 会获取当前 exports 上的 XComponent）
    testNapi.initXComponent();
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
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}

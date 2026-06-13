import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const SOCK = '/data/storage/el2/base/files/wine-wayland';
const WINE_BIN = '/data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox';
const WINE_LIB = `${WINE_BIN}/bin/x86_64-unix`;

@Entry
@Component
struct Index {
  @State message: string = 'Wine for HarmonyOS';
  @State state: string = 'idel';
  controller: XComponentController = new XComponentController();

  aboutToAppear(): void {
    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
      this.state = s;
    });
  }

  startWine = (): void => {
    this.message = '启动 Wayland compositor...';
    const ok: boolean = testNapi.startServer(SOCK);
    hilog.info(DOMAIN, 'wine', 'wayland server: %{public}s', ok ? 'OK' : 'FAIL');
    if (!ok) {
      this.message = 'Wayland compositor 启动失败';
      return;
    }

    this.message = '启动 Wine...';
    const pid: number = testNapi.launchClient(
      `${WINE_BIN}/bin/box64`,
      [`${WINE_BIN}/bin/box64`, `${WINE_BIN}/bin/wine`, `${WINE_BIN}/bin/notepad.exe`],
      SOCK,
      `${WINE_BIN}/bin:${WINE_LIB}`
    );
    hilog.info(DOMAIN, 'wine', 'wine pid: %{public}d', pid);
    if (pid <= 0) {
      this.message = 'Wine 启动失败';
      return;
    }
    this.message = `Wine running, pid=${pid}`;
  }

  build() {
    Column() {
      // 顶部工具栏: 按钮 + 状态 一行
      Row() {
        Button('启动 Wine')
          .fontSize(14)
          .onClick(this.startWine)

        Text(this.state == 'active' ? '● 运行中' : '○ 待机')
          .fontSize(13)
          .fontColor(this.state == 'active' ? '#4f4' : '#888')
          .margin({ left: 12 })
      }
      .width('100%')
      .height(36)
      .padding({ left: 12, right: 12 })
      .backgroundColor('#1a1a1a')
      .alignItems(VerticalAlign.Center)

      // XComponent — 占满剩余空间
      XComponent({ id: 'wine-xc', type: XComponentType.SURFACE, controller: this.controller, libraryname: 'entry' })
        .width('100%')
        .layoutWeight(1)
        .backgroundColor('#000')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}

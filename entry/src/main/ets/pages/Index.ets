import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';

const DOMAIN = 0x0000;
const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';
const WINE_BIN = '/data/service/hnp/winehua.org/winehua_0.1.0/opt/winehua';
const WINE_LIB = `${WINE_BIN}/bin/x86_64-unix`;
const BOX64 = `${WINE_BIN}/bin/box64`;
const BIN_DIR = `${WINE_BIN}/bin`;
const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

@Entry
@Component
struct Index {
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];

  aboutToAppear(): void {
    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
      this.state = s;
      if (s == 'wineserver-starting' || s == 'wineboot-starting') {
        this.statusText = 'Wine 正在初始化...';
      } else if (s == 'wine-ready') {
        this.statusText = '已就绪';
        this.refreshPrefix();
      } else if (s == 'wine-running') {
        this.statusText = '运行中';
      } else if (s == 'exited') {
        this.statusText = '已退出';
      }
    });
    this.refreshPrefix();
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix() as boolean;
      if (this.prefixReady) {
        this.statusText = '已就绪';
        this.curDir = DRIVE_C;
        this.dirStack = [];
        this.listDir(DRIVE_C);
      } else {
        this.statusText = '未初始化';
        this.entries = [];
      }
    } catch (e) {
      this.prefixReady = false;
      this.statusText = '未初始化';
      this.entries = [];
    }
  }

  private listDir = (path: string): void => {
    try {
      let names = fs.listFileSync(path);
      names.sort((a, b) => {
        let aIsDir = this.isDir(path + '/' + a);
        let bIsDir = this.isDir(path + '/' + b);
        if (aIsDir && !bIsDir) return -1;
        if (!aIsDir && bIsDir) return 1;
        if (!aIsDir && !bIsDir) {
          let aExe = a.toLowerCase().endsWith('.exe') ? 0 : 1;
          let bExe = b.toLowerCase().endsWith('.exe') ? 0 : 1;
          return aExe - bExe || a.localeCompare(b);
        }
        return a.localeCompare(b);
      });
      this.entries = names;
    } catch (e) {
      this.entries = [];
    }
  }

  private isDir = (path: string): boolean => {
    try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
  }

  private isExe = (name: string): boolean => {
    return name.toLowerCase().endsWith('.exe');
  }

  private enterDir = (name: string): void => {
    let newPath = this.curDir + '/' + name;
    this.dirStack.push(this.curDir);
    this.curDir = newPath;
    this.listDir(newPath);
  }

  private goUp = (): void => {
    let parent = this.dirStack.pop();
    if (parent) {
      this.curDir = parent;
      this.listDir(parent);
    }
  }

  private doInit = (): void => {
    this.statusText = '启动服务...';
    const ok: boolean = testNapi.startServer(SOCK);
    if (!ok) {
      this.statusText = '启动服务失败';
      return;
    }
    // launchClient 现在只做 wineserver + wineboot --init
    testNapi.launchClient(BOX64, [], SOCK, `${WINE_BIN}/bin:${WINE_LIB}`);
  }

  private doReset = (): void => {
    this.statusText = '正在清空...';
    try {
      testNapi.resetWinePrefix();
      this.state = 'idle';
      this.prefixReady = false;
      this.entries = [];
      this.statusText = '已清空';
      setTimeout(() => this.doInit(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'reset error: %{public}s', JSON.stringify(e));
      this.statusText = '清空失败';
    }
  }

  private doRun = (name: string): void => {
    this.statusText = '启动中...';
    let exePath = this.curDir + '/' + name;
    testNapi.runWineExe(BIN_DIR, SOCK, `${WINE_BIN}/bin:${WINE_LIB}`, exePath);
  }

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  build() {
    Column() {
      // -- 顶部标题栏 --
      Row() {
        Text($r('app.string.app_name'))
          .fontSize(15).fontWeight(FontWeight.Bold).fontColor('#fff')
        Text(this.statusText)
          .fontSize(13)
          .fontColor(this.state == 'wine-running' ? '#4f4' :
                     this.state == 'exited' ? '#f84' :
                     this.prefixReady ? '#8cf' : '#888')
          .margin({ left: 12 }).layoutWeight(1)
      }
      .width('100%').height(38)
      .padding({ left: 12, right: 12 })
      .backgroundColor('#1a1a1a')
      .alignItems(VerticalAlign.Center)

      // -- 操作按钮 --
      Row() {
        if (!this.prefixReady) {
          Button('初始化 Wine')
            .fontSize(14)
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
            .onClick(this.doInit)
        } else {
          Row() {
            Button('重置 Wine 环境')
              .fontSize(13).backgroundColor('#833')
              .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
              .onClick(this.doReset)
              .margin({ right: 8 })
            Text('|')
              .fontSize(13).fontColor('#555').margin({ left: 4, right: 4 })
            Button('重新初始化')
              .fontSize(13).backgroundColor('#444')
              .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
              .onClick(this.doInit)
          }
        }
      }
      .width('100%').padding({ left: 12, right: 12, top: 8, bottom: 4 })
      .justifyContent(FlexAlign.Start)

      // -- 文件浏览器 --
      if (this.prefixReady) {
        Row() {
          if (this.curDir != DRIVE_C) {
            Button('← 上级目录')
              .fontSize(12).height(28).backgroundColor('#333')
              .onClick(this.goUp)
          }
          Text(this.relPath())
            .fontSize(12).fontColor('#888')
            .margin({ left: 8 }).layoutWeight(1)
            .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ left: 12, right: 12, top: 6, bottom: 2 })

        List({ space: 1 }) {
          ForEach(this.entries, (name: string) => {
            ListItem() {
              Row() {
                Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                  .fontSize(14).width(24)
                Text(name)
                  .fontSize(13)
                  .fontColor(this.isExe(name) ? '#dfd' :
                             this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
                  .layoutWeight(1)
                  .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                if (this.isExe(name)) {
                  Button('启动')
                    .fontSize(12).height(28)
                    .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
                    .onClick(() => this.doRun(name))
                }
              }
              .width('100%').padding({ left: 12, right: 8, top: 6, bottom: 6 })
              .alignItems(VerticalAlign.Center)
              .onClick(() => {
                if (this.isDir(this.curDir + '/' + name)) {
                  this.enterDir(name);
                }
              })
            }
            .backgroundColor('#1e1e1e')
          })
        }
        .width('100%').layoutWeight(1)
      } else {
        Column() {
          Text('需要先初始化 Wine 环境')
            .fontSize(13).fontColor('#666')
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%').backgroundColor('#111')
  }
}

import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { WineEnvService, DRIVE_C } from '../service/WineEnvService';
import { AppLibraryService, AppEntry } from '../service/AppLibraryService';

const DOMAIN = 0x0000;

// 文件页: 原 Index 的文件浏览器整体迁入 (listDir/enterDir/goUp/relPath 等逻辑未变)。
// exe 行内动作: "启动" (一次性运行, 走 AppLibraryService.launch) 与 "加入应用库" (已入库显示"已在库中")。
@Component
export struct FileBrowserView {
  // -- 镜像自 service 的状态 (订阅驱动, 见 aboutToAppear) --
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State libEntries: AppEntry[] = [];
  // -- 页面自有状态 (原 Index 文件浏览器) --
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];

  private env: WineEnvService = WineEnvService.getInstance();
  private lib: AppLibraryService = AppLibraryService.getInstance();
  // 上一次的 prefixReady 镜像值, 用于边沿检测 (避免每次 notify 都重复 listDir)
  private prevPrefixReady: boolean = false;

  private mirrorEnv = (): void => {
    this.state = this.env.state;
    this.prefixReady = this.env.prefixReady;

    // prefix 就绪/失效时的目录联动 (模式同原 Index): 检测边沿后只触发一次
    if (this.prefixReady != this.prevPrefixReady) {
      if (this.prefixReady) {
        this.curDir = DRIVE_C;
        this.dirStack = [];
        this.listDir(DRIVE_C);
      } else {
        this.entries = [];
      }
      this.prevPrefixReady = this.prefixReady;
    }
  }

  private mirrorLib = (): void => {
    this.libEntries = this.lib.entries;
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirrorEnv);
    this.lib.subscribe(this.mirrorLib);
    this.mirrorEnv();
    this.mirrorLib();
  }

  private listDir = (path: string): void => {
    try {
      hilog.info(DOMAIN, 'wine', 'listDir: path=%{public}s', path);
      let names = fs.listFileSync(path);
      hilog.info(DOMAIN, 'wine', 'listDir: got %{public}d entries', names.length);
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

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  private inLibrary = (path: string): boolean => {
    for (const entry of this.libEntries) {
      if (entry.path == path) {
        return true;
      }
    }
    return false;
  }

  build() {
    Column() {
      if (this.prefixReady) {
        Row() {
          if (this.curDir != DRIVE_C) {
            Button('← 上级目录')
              .backgroundColor($r('app.color.control_bg'))
              .onClick(this.goUp)
          }
          Text(this.relPath())
            .fontColor($r('app.color.text_tertiary'))
            .margin({ left: 10 }).layoutWeight(1)
            .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 6 })

        List({ space: 1 }) {
          ForEach(this.entries, (name: string) => {
            ListItem() {
              Row() {
                Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                  .width(28)
                Text(name)
                  .fontColor(this.isExe(name) ? $r('app.color.accent_soft') :
                             this.isDir(this.curDir + '/' + name) ? $r('app.color.state_info') : $r('app.color.text_tertiary'))
                  .layoutWeight(1)
                  .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                if (this.isExe(name)) {
                  Button('启动')
                    .height(32)
                    .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
                    .onClick(() => this.lib.launch(this.curDir + '/' + name))
                  if (this.inLibrary(this.curDir + '/' + name)) {
                    Text('已在库中')
                      .fontSize(13)
                      .fontColor($r('app.color.text_disabled'))
                      .margin({ left: 10 })
                  } else {
                    Button('加入应用库')
                      .height(32)
                      .margin({ left: 8 })
                      .onClick(() => this.lib.add(this.curDir + '/' + name))
                  }
                }
              }
              .width('100%').height(44).padding({ left: 14, right: 14 })
              .alignItems(VerticalAlign.Center)
              .onClick(() => {
                if (this.isDir(this.curDir + '/' + name)) {
                  this.enterDir(name);
                }
              })
            }
            .backgroundColor($r('app.color.card_bg'))
          }, (name: string) => name)
        }
        .width('100%').layoutWeight(1)
      } else {
        Column() {
          Text('需要先初始化 Wine 环境')
            .fontColor($r('app.color.text_disabled'))
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.window_bg'))
  }
}

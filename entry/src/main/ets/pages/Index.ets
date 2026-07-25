import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';
import { WineEnvService, DRIVE_C, SOCK, WINE_BIN, WINE_LIB } from '../service/WineEnvService';
import { ProcessService, ProcessInfo } from '../service/ProcessService';

const DOMAIN = 0x0000;

@Entry
@Component
struct Index {
  // -- 镜像自 service 的状态 (订阅驱动, 见 aboutToAppear) --
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];
  @State processList: ProcessInfo[] = [];
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';

  private env: WineEnvService = WineEnvService.getInstance();
  private proc: ProcessService = ProcessService.getInstance();
  // 上一次的 prefixReady 镜像值, 用于边沿检测 (避免每次 notify 都重复 listDir)
  private prevPrefixReady: boolean = false;

  aboutToAppear(): void {
    const env = this.env;
    const proc = this.proc;
    const mirror = () => {
      this.state = env.state;
      this.prefixReady = env.prefixReady;
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
      this.processList = proc.processList;

      // prefix 就绪/失效时的目录联动原发生在 refreshPrefix/waitForPrefix 内部;
      // 迁入 service 后不碰页面字段, 改为页面检测 prefixReady 边沿后自行处理, 只触发一次
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
    };
    env.subscribe(mirror);
    proc.subscribe(mirror);
    proc.bindEnv(env);
    env.init(getContext(this));
    mirror();
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

  private doRun = (name: string): void => {
    this.env.setStatusText('启动中...');
    let exePath = this.curDir + '/' + name;
    testNapi.runWineExe(WINE_BIN, SOCK, `${WINE_BIN}:${WINE_LIB}`, exePath, this.env.zHome);
  }

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  build() {
    Stack() {
    Row() {
      // ============================================================
      //  侧边栏
      // ============================================================
      Column() {
        // -- Wine 环境状态 --
        Text('Wine 环境')
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_secondary'))
          .width('100%').padding({ top: 12, bottom: 6 })

        Row() {
          Circle({ width: 8, height: 8 })
            .fill(this.prefixReady ? $r('app.color.state_ok') : $r('app.color.state_warn'))
            .margin({ right: 6 })
          Text(this.statusText)
            .fontColor(this.state == 'wine-running' ? $r('app.color.state_ok') :
                       this.state == 'exited' ? $r('app.color.state_warn') :
                       this.prefixReady ? $r('app.color.state_info') : $r('app.color.text_tertiary'))
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ bottom: 12 })
        .alignItems(VerticalAlign.Center)

        Divider().color($r('app.color.divider')).width('100%')

        // -- 快速操作 --
        Text('快速操作')
          .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_secondary'))
          .width('100%').padding({ top: 12, bottom: 8 })

        if (!this.prefixReady) {
          Button('初始化 Wine')
            .width('100%').backgroundColor($r('app.color.action_primary'))
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
            .onClick(this.env.doInit)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor($r('app.color.action_danger'))
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.env.doReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor($r('app.color.action_warning'))
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.env.doInit)
            .margin({ bottom: 8 })
        }

        Divider().color($r('app.color.divider')).width('100%')

        // -- 运行中进程 --
        Row() {
          Text('运行中进程')
            .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_secondary'))
            .layoutWeight(1)
          Button({ type: ButtonType.Circle, stateEffect: true }) {
            Text('↻')
          }
          .width(28).height(28).backgroundColor($r('app.color.control_bg'))
          .onClick(this.proc.refresh)
        }
        .width('100%').padding({ top: 12, bottom: 8 })
        .alignItems(VerticalAlign.Center)

        if (this.processList.length > 0) {
          List({ space: 1 }) {
            ForEach(this.processList, (proc: ProcessInfo) => {
              ListItem() {
                Row() {
                  Column() {
                    Text(proc.name)
                      .fontColor($r('app.color.accent_soft'))
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill($r('app.color.state_ok')).margin({ right: 10 })
                  }
                  Text('✕')
                    .fontColor($r('app.color.danger'))
                    .onClick(() => this.proc.kill(proc.pid))
                }
                .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor($r('app.color.card_bg'))
            })
          }
          .width('100%').layoutWeight(1)
          .margin({ left: -14, right: -14 })
          .scrollBar(BarState.Off)
        } else {
          Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
            .fontColor($r('app.color.text_disabled'))
            .width('100%').padding({ top: 8 })
        }
      }
      .width(300).height('100%')
      .backgroundColor($r('app.color.panel_bg'))
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })

      // -- 侧边栏分隔线 --
      Divider().vertical(true).color($r('app.color.divider')).height('100%')

      // ============================================================
      //  右侧主内容区
      // ============================================================
      Column() {
        // -- 标题栏 --
        Row() {
          Text($r('app.string.app_name'))
            .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))

          Text(this.statusText)
            .fontColor(this.state == 'wine-running' ? $r('app.color.state_ok') :
                       this.state == 'exited' ? $r('app.color.state_warn') :
                       this.prefixReady ? $r('app.color.state_info') : $r('app.color.text_tertiary'))
            .margin({ left: 12 }).layoutWeight(1)
            .maxLines(1)
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor($r('app.color.header_bg'))
        .alignItems(VerticalAlign.Center)

        // -- 文件浏览器 --
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
                      .onClick(() => this.doRun(name))
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
            })
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
      .layoutWeight(1).height('100%')
      .backgroundColor($r('app.color.window_bg'))
    }
    .width('100%').height('100%')
    if (this.initOverlayVisible && !this.prefixReady) {
      Column() {
        LoadingProgress()
          .width(42)
          .height(42)
          .color($r('app.color.accent'))
          .margin({ bottom: 18 })
        Text('正在准备 Wine')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .margin({ bottom: 8 })
        Text(this.initOverlayText)
          .fontSize(15)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
    }
    }
    .width('100%').height('100%')
  }
}

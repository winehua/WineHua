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

interface ProcessInfo {
  pid: number;
  name: string;
  path: string;
  state: string;
}

@Entry
@Component
struct Index {
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];
  @State processList: ProcessInfo[] = [];

  aboutToAppear(): void {
    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
      this.state = s;

      // 解析 pid:state 格式 (多进程消息)
      const colonIdx = s.indexOf(':');
      const pid = colonIdx >= 0 ? parseInt(s.substring(0, colonIdx)) : -1;
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;

      if (stateMsg == 'wineserver-starting' || stateMsg == 'wineboot-starting') {
        this.statusText = 'Wine 正在初始化...';
      } else if (stateMsg == 'wine-ready') {
        this.statusText = '已就绪';
        this.refreshPrefix();
      } else if (stateMsg == 'wine-running') {
        this.statusText = '运行中';
        this.refreshProcessList();
      } else if (stateMsg == 'exited') {
        this.refreshProcessList();
        // 如果还有运行中的进程, 保持 running 状态
        const running = this.processList.filter(p => p.state === 'running');
        if (running.length === 0) {
          this.statusText = '已就绪';
        }
      } else if (stateMsg == 'wine-failed') {
        this.statusText = '启动失败';
        this.refreshProcessList();
      }
    });
    this.refreshPrefix();

    // 自动初始化: 始终在启动后 1s 初始化 Wine (startServer+launchClient)
    setTimeout(() => {
      if (this.state === 'idle') {
        hilog.info(DOMAIN, 'wine', 'auto-init: starting Wine...');
        this.doInit();
      }
    }, 1000);
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix() as boolean;
      if (this.prefixReady) {
        this.curDir = DRIVE_C;
        this.dirStack = [];
        this.listDir(DRIVE_C);
      } else {
        this.entries = [];
      }
    } catch (e) {
      this.prefixReady = false;
      this.entries = [];
    }
  }

  private refreshProcessList = (): void => {
    try {
      this.processList = testNapi.getProcessList() as ProcessInfo[];
    } catch (e) {
      this.processList = [];
    }
  }

  private doKillProcess = (pid: number): void => {
    try {
      testNapi.killProcess(pid);
      setTimeout(() => this.refreshProcessList(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'killProcess error: %{public}s', JSON.stringify(e));
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
    testNapi.launchClient(BOX64, [], SOCK, `${WINE_BIN}/bin:${WINE_LIB}`);
  }

  private doReset = (): void => {
    this.statusText = '正在清空...';
    try {
      testNapi.resetWinePrefix();
      this.state = 'idle';
      this.prefixReady = false;
      this.processList = [];
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
    Row() {
      // ============================================================
      //  侧边栏 (可折叠)
      // ============================================================
        Column() {
          // -- Wine 环境状态 --
          Text('Wine 环境')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .width('100%').padding({ top: 10, bottom: 2, left: 10 })

          Row() {
            Circle({ width: 6, height: 6 })
              .fill(this.prefixReady ? '#4f4' : '#f84')
              .margin({ right: 4 })
            Text(this.statusText)
              
              .fontColor(this.state == 'wine-running' ? '#4f4' :
                         this.state == 'exited' ? '#f84' :
                         this.prefixReady ? '#8cf' : '#888')
              .layoutWeight(1)
              .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').padding({ left: 10, right: 8, bottom: 8 })
          .alignItems(VerticalAlign.Center)

          Divider().color('#2a2a2a').width('100%')

          // -- 快速操作 --
          Text('快速操作')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .width('100%').padding({ top: 8, bottom: 4, left: 10 })

          if (!this.prefixReady) {
            Button('初始化 Wine')
              .width('100%').height(32).backgroundColor('#285')
              .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
              .onClick(this.doInit)
              .margin({ bottom: 4, left: 6, right: 6 })
          } else {
            Button('重置 Wine')
              .width('100%').height(32).backgroundColor('#633')
              .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
              .onClick(this.doReset)
              .margin({ bottom: 4, left: 6, right: 6 })
            Button('重新初始化')
              .width('100%').height(32).backgroundColor('#383')
              .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
              .onClick(this.doInit)
              .margin({ bottom: 4, left: 6, right: 6 })
          }

          Divider().color('#2a2a2a').width('100%')

          // -- 运行中进程 --
          Row() {
            Text('运行中进程')
              .fontWeight(FontWeight.Bold).fontColor('#999')
              .layoutWeight(1)
            Button({ type: ButtonType.Circle, stateEffect: true }) {
              Text('↻')
            }
            .width(24).height(24).backgroundColor('#333')
            .onClick(this.refreshProcessList)
          }
          .width('100%').padding({ top: 8, bottom: 4, left: 10, right: 4 })
          .alignItems(VerticalAlign.Center)

          if (this.processList.length > 0) {
            List({ space: 1 }) {
              ForEach(this.processList, (proc: ProcessInfo) => {
                ListItem() {
                  Row() {
                    Column() {
                      Text(proc.name)
                        .fontColor('#dfd')
                        .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                    }
                    .layoutWeight(1).alignItems(HorizontalAlign.Start)

                    if (proc.state === 'running') {
                      Circle({ width: 5, height: 5 }).fill('#4f4').margin({ right: 8 })
                    }
                    Text('✕')
                      .fontColor('#f55').width(20).textAlign(TextAlign.Center)
                      .onClick(() => this.doKillProcess(proc.pid))
                  }
                  .width('100%').padding({ left: 10, right: 4, top: 5, bottom: 5 })
                  .alignItems(VerticalAlign.Center)
                }
                .backgroundColor('#1e1e1e')
              })
            }
            .width('100%').layoutWeight(1)
            .scrollBar(BarState.Off)
          } else {
            Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
              .fontColor('#555')
              .width('100%').padding({ left: 10, top: 4 })
          }
        }
        .width(190).height('100%')
        .backgroundColor('#141414')
        .padding({ top: 4 })

        // -- 侧边栏分隔线 --
        Divider().vertical(true).color('#2a2a2a').height('100%')

      // ============================================================
      //  右侧主内容区
      // ============================================================
      Column() {
        // -- 标题栏 --
        Row() {
          Text($r('app.string.app_name'))
            .fontWeight(FontWeight.Bold).fontColor('#fff')

          Text(this.statusText)
            
            .fontColor(this.state == 'wine-running' ? '#4f4' :
                       this.state == 'exited' ? '#f84' :
                       this.prefixReady ? '#8cf' : '#888')
            .margin({ left: 10 }).layoutWeight(1)
            .maxLines(1)
        }
        .width('100%').height(38)
        .padding({ left: 6, right: 12 })
        .backgroundColor('#1a1a1a')
        .alignItems(VerticalAlign.Center)

        // -- 文件浏览器 --
        if (this.prefixReady) {
          Row() {
            if (this.curDir != DRIVE_C) {
              Button('← 上级目录')
                .height(26).backgroundColor('#333')
                .onClick(this.goUp)
            }
            Text(this.relPath())
              .fontColor('#888')
              .margin({ left: 8 }).layoutWeight(1)
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').padding({ left: 8, right: 12, top: 4, bottom: 2 })

          List({ space: 1 }) {
            ForEach(this.entries, (name: string) => {
              ListItem() {
                Row() {
                  Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                    .width(24)
                  Text(name)
                    
                    .fontColor(this.isExe(name) ? '#dfd' :
                               this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
                    .layoutWeight(1)
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  if (this.isExe(name)) {
                    Button('启动')
                      .height(28)
                      .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
                      .onClick(() => this.doRun(name))
                  }
                }
                .width('100%').padding({ left: 8, right: 6, top: 6, bottom: 6 })
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
              .fontColor('#555')
          }
          .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
        }
      }
      .layoutWeight(1).height('100%')
      .backgroundColor('#111')
    }
    .width('100%').height('100%')
  }
}

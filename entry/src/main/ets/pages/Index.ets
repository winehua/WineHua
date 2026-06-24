import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;
const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';
const FILES_BASE = '/data/storage/el2/base/files';

// PC 模式 (HNP): Wine 运行时由系统挂载到 /data/service/hnp/
const WINE_BIN_PC = '/data/service/hnp/winehua.org/winehua_0.1.0/opt/winehua/bin';
const WINE_LIB_PC = `${WINE_BIN_PC}/x86_64-unix`;

// Pad 模式 (无 HNP): Wine 运行时从 rawfile 解压到 files/
const WINE_ROOT_PAD = `${FILES_BASE}/wine`;
const WINE_BIN_PAD = `${WINE_ROOT_PAD}/bin`;
const WINE_LIB_PAD = `${WINE_BIN_PAD}/x86_64-unix`;

// 运行时检测: 如果 HNP 路径不存在则使用 Pad 路径
let WINE_BIN = WINE_BIN_PC;
let WINE_LIB = WINE_LIB_PC;
let BIN_DIR = WINE_BIN;
let LAUNCHER_BIN = `${WINE_BIN}/wine`;

function detectWinePath(): void {
  try {
    if (fs.accessSync(WINE_BIN_PC)) {
      // PC 模式 (HNP)
      WINE_BIN = WINE_BIN_PC;
      WINE_LIB = WINE_LIB_PC;
      BIN_DIR = WINE_BIN;
      LAUNCHER_BIN = `${WINE_BIN}/wine`;
      hilog.info(DOMAIN, 'wine', 'detectWinePath: PC mode (HNP)');
      return;
    }
  } catch (_) {}
  // Pad 模式 (无 HNP)
  WINE_BIN = WINE_BIN_PAD;
  WINE_LIB = WINE_LIB_PAD;
  BIN_DIR = WINE_BIN;
  LAUNCHER_BIN = `${WINE_BIN}/box64`;
  hilog.info(DOMAIN, 'wine', 'detectWinePath: Pad mode (rawfile)');
}

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
    detectWinePath();

    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
      this.state = s;

      const colonIdx = s.indexOf(':');
      const pid = colonIdx >= 0 ? parseInt(s.substring(0, colonIdx)) : -1;
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;

      if (stateMsg == 'wineserver-starting' || stateMsg == 'wineboot-starting') {
        this.statusText = 'Wine 正在初始化...';
      } else if (stateMsg == 'wine-ready') {
        this.statusText = '已就绪';
        // wineboot 可能还在后台初始化 prefix，轮询等待 drive_c 就绪
        this.waitForPrefix();
      } else if (stateMsg == 'wine-running') {
        this.statusText = '运行中';
        this.refreshProcessList();
      } else if (stateMsg == 'exited') {
        this.refreshProcessList();
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
    // 下一帧自动初始化 (避免阻塞 UI 首次渲染)
    setTimeout(() => this.doInit(), 0);
  }

  // Pad: 从 rawfile 取出 wine-data.zip → zlib.decompressFile 解压到 files/wine/
  // 使用 getRawFdSync 流式读取，避免 249MB 一次加载到内存导致 OOM
  private async extractWineData(): Promise<void> {
    const marker = `${WINE_ROOT_PAD}/share/wine/wine.inf`;
    hilog.info(DOMAIN, 'wine', 'extract: check marker=%{public}s', marker);
    try {
      if (fs.accessSync(marker)) {
        hilog.info(DOMAIN, 'wine', 'extract: already done (marker exists)');
        return;
      }
    } catch (_) {}
    hilog.info(DOMAIN, 'wine', 'extract: marker not found, start extracting');

    try {
      const ctx = getContext(this);
      const rm = ctx.resourceManager;

      // 流式拷贝: getRawFdSync 获取 fd → 分块读取 → 写入临时文件
      hilog.info(DOMAIN, 'wine', 'extract: getRawFdSync wine-data.zip...');
      const rawFd = rm.getRawFdSync('wine-data.zip');
      hilog.info(DOMAIN, 'wine', 'extract: rawFd fd=%{public}d offset=%{public}d length=%{public}d',
        rawFd.fd, rawFd.offset, rawFd.length);

      const tmpZip = `${FILES_BASE}/wine-data.zip`;
      hilog.info(DOMAIN, 'wine', 'extract: writing tmpZip=%{public}s (stream)', tmpZip);
      const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);

      const CHUNK = 4 * 1024 * 1024; // 4MB 分块
      const buf = new ArrayBuffer(CHUNK);
      let pos = rawFd.offset;
      const end = rawFd.offset + rawFd.length;
      let totalWritten = 0;

      while (pos < end) {
        const toRead = Math.min(CHUNK, end - pos);
        const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
        if (n <= 0) break;
        fs.writeSync(dst.fd, buf, { length: n });
        pos += n;
        totalWritten += n;
      }
      fs.closeSync(dst);
      rm.closeRawFdSync('wine-data.zip');
      hilog.info(DOMAIN, 'wine', 'extract: stream copy ok, totalWritten=%{public}d', totalWritten);

      // 确保目标目录存在
      fs.mkdirSync(WINE_ROOT_PAD, true);
      hilog.info(DOMAIN, 'wine', 'extract: calling decompressFile → %{public}s', WINE_ROOT_PAD);
      await zlib.decompressFile(tmpZip, WINE_ROOT_PAD, {});
      hilog.info(DOMAIN, 'wine', 'extract: decompress ok');

      // 验证
      if (fs.accessSync(marker)) {
        hilog.info(DOMAIN, 'wine', 'extract: verified, marker exists');
      } else {
        hilog.warn(DOMAIN, 'wine', 'extract: marker still missing after decompress!');
      }
      this.statusText = '已解压';
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'extract FAIL: %{public}s', JSON.stringify(e));
      this.statusText = '解压失败';
    }
  }

  private refreshPrefix = (): void => {
    try {
      let names = fs.listFileSync(DRIVE_C);
      this.prefixReady = names.length > 0;
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

  // wineboot 后台异步初始化 prefix，轮询直到 drive_c 有内容
  private waitForPrefix = async (): Promise<void> => {
    for (let i = 0; i < 90; i++) {
      try {
        let names = fs.listFileSync(DRIVE_C);
        if (names.length > 0) {
          this.prefixReady = true;
          this.curDir = DRIVE_C;
          this.dirStack = [];
          this.listDir(DRIVE_C);
          return;
        }
      } catch (e) { /* drive_c 还没出现 */ }
      await new Promise<void>(r => setTimeout(r, 1000));
    }
    // 超时，仍然尝试刷新一次
    this.refreshPrefix();
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

  private doInit = async (): Promise<void> => {
    // 确保 HOME 目录存在 (Z: 映射目标)
    const zHome = '/storage/Users/currentUser/Download/app.hackeris.winehua';
    if (!fs.accessSync(zHome)) {
      fs.mkdirSync(zHome, true);
      hilog.info(DOMAIN, 'wine', 'auto-init: created HOME=%{public}s', zHome);
    }
    // Pad: 检查是否需要解压 wine 数据文件
    hilog.info(DOMAIN, 'wine', 'auto-init: check PC path=%{public}s', WINE_BIN_PC);
    const isPC = fs.accessSync(WINE_BIN_PC);
    hilog.info(DOMAIN, 'wine', 'auto-init: isPC=%{public}s', String(isPC));
    if (!isPC) {
      this.statusText = '解压数据...';
      hilog.info(DOMAIN, 'wine', 'auto-init: extracting wine data...');
      await this.extractWineData();
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: starting Wine...');
    this.statusText = '启动服务...';
    const ok: boolean = testNapi.startServer(SOCK);
    if (!ok) {
      this.statusText = '启动服务失败';
      return;
    }
    testNapi.launchClient(LAUNCHER_BIN, [], SOCK, `${BIN_DIR}:${WINE_LIB}`);
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
    testNapi.runWineExe(BIN_DIR, SOCK, `${BIN_DIR}:${WINE_LIB}`, exePath);
  }

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  build() {
    Row() {
      // ============================================================
      //  侧边栏
      // ============================================================
      Column() {
        // -- Wine 环境状态 --
        Text('Wine 环境')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 6 })

        Row() {
          Circle({ width: 8, height: 8 })
            .fill(this.prefixReady ? '#4f4' : '#f84')
            .margin({ right: 6 })
          Text(this.statusText)
            .fontColor(this.state == 'wine-running' ? '#4f4' :
                       this.state == 'exited' ? '#f84' :
                       this.prefixReady ? '#8cf' : '#888')
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ bottom: 12 })
        .alignItems(VerticalAlign.Center)

        Divider().color('#2a2a2a').width('100%')

        // -- 快速操作 --
        Text('快速操作')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        if (!this.prefixReady) {
          Button('初始化 Wine')
            .width('100%').backgroundColor('#285')
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting')
            .onClick(this.doInit)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor('#633')
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.doReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor('#652')
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.doInit)
            .margin({ bottom: 8 })
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
          .width(28).height(28).backgroundColor('#333')
          .onClick(this.refreshProcessList)
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
                      .fontColor('#dfd')
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill('#4f4').margin({ right: 10 })
                  }
                  Text('✕')
                    .fontColor('#f55')
                    .onClick(() => this.doKillProcess(proc.pid))
                }
                .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
          .margin({ left: -14, right: -14 })
          .scrollBar(BarState.Off)
        } else {
          Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
            .fontColor('#555')
            .width('100%').padding({ top: 8 })
        }
      }
      .width(300).height('100%')
      .backgroundColor('#141414')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })

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
            .margin({ left: 12 }).layoutWeight(1)
            .maxLines(1)
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor('#1a1a1a')
        .alignItems(VerticalAlign.Center)

        // -- 文件浏览器 --
        if (this.prefixReady) {
          Row() {
            if (this.curDir != DRIVE_C) {
              Button('← 上级目录')
                .backgroundColor('#333')
                .onClick(this.goUp)
            }
            Text(this.relPath())
              .fontColor('#888')
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
                    .fontColor(this.isExe(name) ? '#dfd' :
                               this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
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

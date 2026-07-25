import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;
const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';
const FILES_BASE = '/data/storage/el2/base/files';

// Wine 运行时从 rawfile 解压到 files/
const WINE_ROOT = `${FILES_BASE}/wine`;
const WINE_BIN = `${WINE_ROOT}/bin`;
const WINE_LIB = `${WINE_BIN}/x86_64-unix`;
const BIN_DIR = WINE_BIN;
const BOX64 = `${WINE_BIN}/box64`;

const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

function pathExists(path: string): boolean {
  try {
    fs.accessSync(path);
    return true;
  } catch (_) {
    return false;
  }
}

function readTextIfExists(path: string): string {
  try {
    return fs.readTextSync(path, { encoding: 'utf-8' }).trim();
  } catch (_) {
    return '';
  }
}

function writeTextFile(path: string, text: string): void {
  const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
}

function ensureDirectory(path: string): boolean {
  try {
    if (!pathExists(path)) {
      fs.mkdirSync(path, true);
    }
    return pathExists(path);
  } catch (e) {
    hilog.warn(DOMAIN, 'wine', 'mkdir failed path=%{public}s err=%{public}s', path, JSON.stringify(e));
    return false;
  }
}

function removePathRecursive(path: string): void {
  let st: fs.Stat;
  try {
    st = fs.lstatSync(path);
  } catch (e) {
    hilog.warn(DOMAIN, 'wine', 'removePath: skip missing path=%{public}s err=%{public}s', path, JSON.stringify(e));
    return;
  }

  if (st.isSymbolicLink() || !st.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }

  const names = fs.listFileSync(path);
  for (const name of names) {
    removePathRecursive(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}

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
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  private hasRawfileWineData: boolean = false;
  private zHome: string = '/storage/Users/currentUser/Download';

  aboutToAppear(): void {
    this.hasRawfileWineData = this.detectRawfileWineData();

    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
      this.state = s;

      const colonIdx = s.indexOf(':');
      const pid = colonIdx >= 0 ? parseInt(s.substring(0, colonIdx)) : -1;
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;

      if (stateMsg == 'wineserver-starting' || stateMsg == 'wineboot-starting') {
        this.setInitStatus('Wine 正在初始化...', true);
      } else if (stateMsg == 'wine-ready') {
        this.setInitStatus('已就绪', false);
        // wineboot 可能还在后台初始化 prefix，轮询等待 drive_c 就绪
        this.waitForPrefix();
      } else if (stateMsg == 'wine-running') {
        this.setInitStatus('运行中', false);
        this.refreshProcessList();
      } else if (stateMsg == 'exited') {
        this.refreshProcessList();
        const running = this.processList.filter(p => p.state === 'running');
        if (running.length === 0) {
          this.setInitStatus('已就绪', false);
        }
      } else if (stateMsg == 'wine-failed') {
        this.setInitStatus('启动失败', false);
        this.refreshProcessList();
      }
    });
    this.refreshPrefix();
    // 下一帧自动初始化 (避免阻塞 UI 首次渲染)
    setTimeout(() => this.doInit(), 0);
  }

  private detectRawfileWineData(): boolean {
    const ctx = getContext(this);
    const rm = ctx.resourceManager;
    try {
      const rawFd = rm.getRawFdSync('wine-data.zip');
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip exists len=%{public}d', rawFd.length);
      rm.closeRawFdSync('wine-data.zip');
      return true;
    } catch (_) {
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip not packaged');
      return false;
    }
  }

  private setInitStatus(text: string, visible: boolean): void {
    this.statusText = text;
    this.initOverlayText = text;
    this.initOverlayVisible = visible;
  }

  // Pad: 从 rawfile 取出 wine-data.zip → zlib.decompressFile 解压到 files/wine/
  // 使用 getRawFdSync 流式读取，避免 249MB 一次加载到内存导致 OOM
  private async extractWineData(): Promise<void> {
    const marker = `${WINE_ROOT}/share/wine/wine.inf`;
    hilog.info(DOMAIN, 'wine', 'extract: check marker=%{public}s', marker);
    try {
      if (fs.accessSync(marker)) {
        hilog.info(DOMAIN, 'wine', 'extract: already done (marker exists)');
        return;
      }
    } catch (_) {}
    hilog.info(DOMAIN, 'wine', 'extract: marker not found, start extracting');

    const ctx = getContext(this);
    const rm = ctx.resourceManager;
    const rawFd = rm.getRawFdSync('wine-data.zip');

    try {
      // 流式拷贝: getRawFdSync 获取 fd → 分块读取 → 写入临时文件
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
      hilog.info(DOMAIN, 'wine', 'extract: stream copy ok, totalWritten=%{public}d', totalWritten);

      // 确保目标目录存在
      fs.mkdirSync(WINE_ROOT, true);
      hilog.info(DOMAIN, 'wine', 'extract: calling decompressFile → %{public}s', WINE_ROOT);
      await zlib.decompressFile(tmpZip, WINE_ROOT, {});
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
    } finally {
      rm.closeRawFdSync('wine-data.zip');
    }
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix();
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

  // wineboot 后台异步初始化 prefix，轮询直到 registry 和核心目录完整
  private waitForPrefix = async (): Promise<void> => {
    for (let i = 0; i < 90; i++) {
      try {
        if (testNapi.checkWinePrefix()) {
          this.prefixReady = true;
          this.initOverlayVisible = false;
          this.curDir = DRIVE_C;
          this.dirStack = [];
          this.listDir(DRIVE_C);
          return;
        }
      } catch (e) { /* prefix 还没初始化完成 */ }
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
    // HOME 目录 (Z: 映射目标), 通过 DocumentViewPicker 获取 Download 路径
    const documentViewPicker = new picker.DocumentViewPicker();
    let documentSaveResult = await documentViewPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
    const pickerHome = new fileUri.FileUri(documentSaveResult[0]).path;
    this.zHome = pickerHome.endsWith('/') ? pickerHome : `${pickerHome}/`;
    hilog.info(DOMAIN, 'wine', 'auto-init: Z home=%{public}s', this.zHome);
    const gamesDir = `${this.zHome}games`;
    if (!ensureDirectory(gamesDir)) {
      this.setInitStatus('创建 games 目录失败', false);
      return;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: games directory=%{public}s', gamesDir);
    // 检查是否需要解压 wine 数据文件
    hilog.info(DOMAIN, 'wine', 'auto-init: check marker=%{public}s', `${WINE_ROOT}/share/wine/wine.inf`);
    let needExtract = true;
    try { needExtract = !fs.accessSync(`${WINE_ROOT}/share/wine/wine.inf`); } catch (_) {}
    if (needExtract) {
      this.setInitStatus('解压数据...', true);
      hilog.info(DOMAIN, 'wine', 'auto-init: extracting wine data...');
      await this.extractWineData();
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: starting Wine...');
    this.setInitStatus('启动服务...', true);
    const ok: boolean = testNapi.startServer(SOCK);
    if (!ok) {
      this.setInitStatus('启动服务失败', false);
      return;
    }
    testNapi.launchClient(BOX64, [], SOCK, `${BIN_DIR}:${WINE_LIB}`, this.zHome);
  }

  private doReset = (): void => {
    this.setInitStatus('正在清空...', true);
    try {
      testNapi.resetWinePrefix();
      this.state = 'idle';
      this.prefixReady = false;
      this.processList = [];
      this.entries = [];
      this.setInitStatus('已清空', false);
      setTimeout(() => this.doInit(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'reset error: %{public}s', JSON.stringify(e));
      this.setInitStatus('清空失败', false);
    }
  }

  private doRun = (name: string): void => {
    this.statusText = '启动中...';
    let exePath = this.curDir + '/' + name;
    testNapi.runWineExe(BIN_DIR, SOCK, `${WINE_BIN}:${WINE_LIB}`, exePath, this.zHome);
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
            .onClick(this.doInit)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor($r('app.color.action_danger'))
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.doReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor($r('app.color.action_warning'))
            .enabled(this.state != 'wineserver-starting' && this.state != 'wineboot-starting' && this.state != 'wine-running')
            .onClick(this.doInit)
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
                      .fontColor($r('app.color.accent_soft'))
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill($r('app.color.state_ok')).margin({ right: 10 })
                  }
                  Text('✕')
                    .fontColor($r('app.color.danger'))
                    .onClick(() => this.doKillProcess(proc.pid))
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

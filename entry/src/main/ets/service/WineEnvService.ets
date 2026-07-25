import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import testNapi from 'libentry.so';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { ProcessService } from './ProcessService';

const DOMAIN = 0x0000;
const FILES_BASE = '/data/storage/el2/base/files';
// doInit 与页面 doRun 连同一个 wineserver socket
export const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';

// Wine 运行时从 rawfile 解压到 files/
const WINE_ROOT = `${FILES_BASE}/wine`;
export const WINE_BIN = `${WINE_ROOT}/bin`;
export const WINE_LIB = `${WINE_BIN}/x86_64-unix`;
const BIN_DIR = WINE_BIN;
const BOX64 = `${WINE_BIN}/box64`;

// 页面文件浏览器以 C: 盘为根
export const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

function pathExists(path: string): boolean {
  try {
    fs.accessSync(path);
    return true;
  } catch (_) {
    return false;
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

// Wine 环境生命周期服务: 检测/解压 wine 数据、启动/重置 wineserver、跟踪 prefix 状态。
// 原 Index.ets 的非 UI 逻辑原样迁入; 页面经 subscribe 镜像状态字段, service 不感知 UI。
export class WineEnvService {
  private static instance: WineEnvService | null = null;

  static getInstance(): WineEnvService {
    if (!WineEnvService.instance) {
      WineEnvService.instance = new WineEnvService();
    }
    return WineEnvService.instance;
  }

  // -- 页面镜像的状态 (任何变化后 notify) --
  state: string = 'idle';
  prefixReady: boolean = false;
  statusText: string = '检测中...';
  initOverlayText: string = '';
  initOverlayVisible: boolean = false;
  // 最近一次 native state 原始字符串, ProcessService 据冒号后缀联动刷新进程列表
  lastStateMsg: string = '';
  zHome: string = '/storage/Users/currentUser/Download';

  private hasRawfileWineData: boolean = false;
  private ctx: common.Context | null = null;
  private initialized: boolean = false;
  private listeners: Array<() => void> = [];

  // 极简订阅: 状态变化后逐个调用
  subscribe(listener: () => void): void {
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 原 aboutToAppear 的初始化流程; 幂等, 重复调用直接返回 (native 回调只注册一次)
  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    this.ctx = ctx;

    this.hasRawfileWineData = this.detectRawfileWineData();

    testNapi.setStateCallback(this.onNativeState);
    this.refreshPrefix();
    // 下一帧自动初始化 (避免阻塞 UI 首次渲染)
    setTimeout(() => this.doInit(), 0);
  }

  // 页面 doRun 启动 exe 前的状态提示 (原页面直写 statusText, 现统一由 service 持有)
  setStatusText(text: string): void {
    this.statusText = text;
    this.notify();
  }

  // native state 回调, 分支逻辑与原页面实现一致
  private onNativeState = (s: string): void => {
    hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);
    this.state = s;
    this.lastStateMsg = s;

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
      // notify 内 ProcessService 联动 refresh (原 refreshProcessList)
      this.notify();
    } else if (stateMsg == 'exited') {
      // notify 内 ProcessService 先联动 refresh (原 refreshProcessList 在分支最前),
      // 这里再读刷新后的列表判断是否还有 running 进程
      this.notify();
      const running = ProcessService.getInstance().processList.filter(p => p.state === 'running');
      if (running.length === 0) {
        this.setInitStatus('已就绪', false);
      }
    } else if (stateMsg == 'wine-failed') {
      this.setInitStatus('启动失败', false);
    }
  }

  private detectRawfileWineData(): boolean {
    const rm = this.ctx!.resourceManager;
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
    this.notify();
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

    const rm = this.ctx!.resourceManager;
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
      this.setStatusText('已解压');
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'extract FAIL: %{public}s', JSON.stringify(e));
      this.setStatusText('解压失败');
    } finally {
      rm.closeRawFdSync('wine-data.zip');
    }
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix();
    } catch (e) {
      this.prefixReady = false;
    }
    // prefixReady 变化后的目录联动 (curDir/dirStack/listDir/清空 entries)
    // 由页面镜像时检测边沿自行处理, service 不碰页面字段
    this.notify();
  }

  // wineboot 后台异步初始化 prefix，轮询直到 registry 和核心目录完整
  private waitForPrefix = async (): Promise<void> => {
    for (let i = 0; i < 90; i++) {
      try {
        if (testNapi.checkWinePrefix()) {
          this.prefixReady = true;
          this.initOverlayVisible = false;
          this.notify();
          return;
        }
      } catch (e) { /* prefix 还没初始化完成 */ }
      await new Promise<void>(r => setTimeout(r, 1000));
    }
    // 超时，仍然尝试刷新一次
    this.refreshPrefix();
  }

  doInit = async (): Promise<void> => {
    if (!this.ctx) {
      return;
    }
    // HOME 目录 (Z: 映射目标), 通过 DocumentViewPicker 获取 Download 路径
    const documentViewPicker = new picker.DocumentViewPicker(this.ctx);
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

  doReset = (): void => {
    this.setInitStatus('正在清空...', true);
    try {
      testNapi.resetWinePrefix();
      this.state = 'idle';
      this.prefixReady = false;
      ProcessService.getInstance().clear(); // 原 this.processList = []
      // 原 this.entries = [] 由页面检测 prefixReady true→false 边沿后自行清空
      this.setInitStatus('已清空', false);
      setTimeout(() => this.doInit(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'reset error: %{public}s', JSON.stringify(e));
      this.setInitStatus('清空失败', false);
    }
  }
}

import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

interface ToplevelInfo {
  id: number;
  title: string;
  subWindow: window.Window | null;
  // 标题栏偏移: windowHeight - xcHeight (初始创建时测量)
  titleBarOffset: number;
}

export class WineWindowManager {
  private static instance: WineWindowManager | null = null;
  private windowStage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private toplevels: Map<number, ToplevelInfo> = new Map();
  // 设备显示密度 (用于缩放子窗口, 匹配主窗口的物理大小)
  private displayScale: number = 1.0;

  static getInstance(): WineWindowManager {
    if (!WineWindowManager.instance) {
      WineWindowManager.instance = new WineWindowManager();
    }
    return WineWindowManager.instance;
  }

  async init(windowStage: window.WindowStage): Promise<void> {
    this.windowStage = windowStage;
    this.mainWindow = windowStage.getMainWindowSync();

    // 获取设备显示密度 (主窗口 XComponent EGL 渲染会充满屏幕, 子窗口也需要同样缩放)
    try {
      const d = display.getDefaultDisplaySync();
      this.displayScale = d.densityPixels;
      hilog.info(DOMAIN, 'WineWM', '[MW] display densityPixels=%{public}f', this.displayScale);
    } catch (_) {
      this.displayScale = 1.0;
    }

    testNapi.setToplevelCallback((id: number, event: string, data: string) => {
      hilog.info(DOMAIN, 'WineWM', '[MW] cb toplevel #%{public}d event=%{public}s data=%{public}s',
        id, event, data);
      this.onToplevelEvent(id, event, data);
    });

    hilog.info(DOMAIN, 'WineWM', '[MW] initialized scale=%{public}f', this.displayScale);
  }

  private onToplevelEvent(id: number, event: string, data: string): void {
    switch (event) {
      case 'created':
        this.createSubWindow(id, data);
        break;
      case 'destroyed':
        this.destroySubWindow(id);
        break;
      case 'title':
        this.updateTitle(id, data);
        break;
      case 'resize':
        this.resizeWindow(id, data);
        break;
    }
  }

  private resizeWindow(id: number, data: string): void {
    const info = this.toplevels.get(id);
    if (!info || !info.subWindow) return;

    try {
      const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
      const w = obj.w as number;
      const h = obj.h as number;
      if (w && h) {
        // 按设备密度缩放 + 补偿标题栏高度
        const scaledW = Math.round(w * this.displayScale);
        const scaledH = Math.round(h * this.displayScale + info.titleBarOffset);
        hilog.info(DOMAIN, 'WineWM', '[MW] resizing sub-window #%{public}d: frame=%{public}dx%{public}d → window=%{public}dx%{public}d (scale=%{public}f +offset=%{public}d)',
          id, w, h, scaledW, scaledH, this.displayScale, info.titleBarOffset);
        info.subWindow.resize(scaledW, scaledH);
      }
    } catch (_) { /* ignore parse error */ }
  }

  private async createSubWindow(id: number, data: string): Promise<void> {
    if (!this.mainWindow) {
      hilog.error(DOMAIN, 'WineWM', 'mainWindow not ready, cannot create sub-window');
      return;
    }

    try {
      // 解析 JSON: {title?, w?, h?}
      let title = `Wine Window ${id}`;
      // 默认尺寸稍大 (初始值, 首帧到达后会 resize 到实际大小)
      let w = 800, h = 600;
      try {
        const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
        if (obj.title) title = obj.title as string;
        if (obj.w) w = obj.w as number;
        if (obj.h) h = obj.h as number;
      } catch (_e) { /* use defaults */ }

      // 按设备密度缩放 (匹配主窗口 XComponent EGL 渲染的物理大小)
      const scaledW = Math.round(w * this.displayScale);
      const scaledH = Math.round(h * this.displayScale);

      hilog.info(DOMAIN, 'WineWM', '[MW] creating sub-window #%{public}d: %{public}s %{public}dx%{public}d (scaled: %{public}dx%{public}d, scale=%{public}f)',
        id, title, w, h, scaledW, scaledH, this.displayScale);

      // 关键! 先设置 pending toplevel, 子窗口 XComponent OnSurfaceCreated 时才能正确路由
      testNapi.setPendingToplevel(id);

      const subWin = await this.mainWindow.createSubWindowWithOptions(
        `wine_toplevel_${id}`,
        { title: title, decorEnabled: true, isModal: false }
      );

      await subWin.loadContent('pages/WineWindow');
      // 子窗口 resize 的是外框尺寸, XComponent 内容区 = 外框 - 标题栏
      await subWin.resize(scaledW, scaledH);
      await subWin.showWindow();

      // 估算标题栏偏移 (子窗口外框 - XComponent 内容区), 按缩放修正
      // 观察值: 480-420=60, 640-580≈60 (unscaled)
      const titleBarOffset = Math.round(60 * this.displayScale);

      // 存储
      this.toplevels.set(id, { id, title, subWindow: subWin, titleBarOffset: titleBarOffset });

      // 监听子窗口关闭
      subWin.on('windowEvent', (eventType: number) => {
        hilog.info(DOMAIN, 'WineWM', '[MW] sub-window #%{public}d event=%{public}d', id, eventType);
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          this.toplevels.delete(id);
          testNapi.sendToplevelClose(id);
          testNapi.destroyToplevel(id);
        }
      });

      hilog.info(DOMAIN, 'WineWM', '[MW] sub-window #%{public}d created ✓', id);
    } catch (err) {
      hilog.error(DOMAIN, 'WineWM', '[MW] FAILED create sub-window #%{public}d: %{public}s',
        id, JSON.stringify(err));
    }
  }

  private async destroySubWindow(id: number): Promise<void> {
    const info = this.toplevels.get(id);
    if (!info) return;

    try {
      if (info.subWindow) {
        await info.subWindow.destroyWindow();
      }
      this.toplevels.delete(id);
      testNapi.destroyToplevel(id);
      hilog.info(DOMAIN, 'WineWM', '[MW] sub-window #%{public}d destroyed', id);
    } catch (err) {
      hilog.error(DOMAIN, 'WineWM', '[MW] FAILED destroy sub-window #%{public}d: %{public}s',
        id, JSON.stringify(err));
    }
  }

  private updateTitle(id: number, data: string): void {
    const info = this.toplevels.get(id);
    if (!info || !info.subWindow) return;

    try {
      const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
      if (obj.title) {
        info.title = obj.title as string;
        info.subWindow.setWindowTitle(obj.title as string);
      }
    } catch (_) { /* ignore parse error */ }
  }
}

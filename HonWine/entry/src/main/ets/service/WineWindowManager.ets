import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

interface ToplevelInfo {
  id: number;
  title: string;
  subWindow: window.Window | null;
}

export class WineWindowManager {
  private static instance: WineWindowManager | null = null;
  private windowStage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private toplevels: Map<number, ToplevelInfo> = new Map();

  static getInstance(): WineWindowManager {
    if (!WineWindowManager.instance) {
      WineWindowManager.instance = new WineWindowManager();
    }
    return WineWindowManager.instance;
  }

  async init(windowStage: window.WindowStage): Promise<void> {
    this.windowStage = windowStage;
    this.mainWindow = windowStage.getMainWindowSync();

    testNapi.setToplevelCallback((id: number, event: string, data: string) => {
      hilog.info(DOMAIN, 'WineWM', '[MW] cb toplevel #%{public}d event=%{public}s data=%{public}s',
        id, event, data);
      this.onToplevelEvent(id, event, data);
    });

    hilog.info(DOMAIN, 'WineWM', '[MW] initialized');
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
    }
  }

  private async createSubWindow(id: number, data: string): Promise<void> {
    if (!this.mainWindow) {
      hilog.error(DOMAIN, 'WineWM', 'mainWindow not ready, cannot create sub-window');
      return;
    }

    try {
      // 解析 JSON: {title?, w?, h?}
      let title = `Wine Window ${id}`;
      let w = 640, h = 480;
      try {
        const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
        if (obj.title) title = obj.title as string;
        if (obj.w) w = obj.w as number;
        if (obj.h) h = obj.h as number;
      } catch (_e) { /* use defaults */ }

      hilog.info(DOMAIN, 'WineWM', '[MW] creating sub-window #%{public}d: %{public}s %{public}dx%{public}d',
        id, title, w, h);

      // 关键! 先设置 pending toplevel, 子窗口 XComponent OnSurfaceCreated 时才能正确路由
      testNapi.setPendingToplevel(id);

      const subWin = await this.mainWindow.createSubWindowWithOptions(
        `wine_toplevel_${id}`,
        { title: title, decorEnabled: true, isModal: false }
      );

      await subWin.loadContent('pages/WineWindow');
      await subWin.resize(w, h);
      await subWin.showWindow();

      // 存储
      this.toplevels.set(id, { id, title, subWindow: subWin });

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

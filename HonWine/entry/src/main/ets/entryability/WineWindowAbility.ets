import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;
const TAG = 'WWA';

export default class WineWindowAbility extends UIAbility {
  private toplevelId: number = 0;
  private cleanedUp: boolean = false;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      const params = want.parameters ?? {};
      this.toplevelId = Number(params.toplevelId) || 0;
      const title = params.title as string ?? '';

      hilog.info(DOMAIN, TAG, '[WWA]  onCreate: tl=%{public}d title="%{public}s" launchReason=%{public}d params=%{public}s',
        this.toplevelId, title, launchParam.launchReason, JSON.stringify(params));

      if (this.toplevelId === 0) {
        hilog.error(DOMAIN, TAG, '[WWA] ERR no toplevelId, terminating self');
        this.context.terminateSelf();
        return;
      }

      // 检查是否在启动期间已经被销毁
      if (WineWindowManager.getInstance().isDestroyed(this.toplevelId)) {
        hilog.warn(DOMAIN, TAG, '[WWA] ERR #%{public}d already destroyed, terminating self', this.toplevelId);
        this.context.terminateSelf();
        return;
      }

      // 注册 context (供 WineWindowManager 调 terminateSelf)
      WineWindowManager.getInstance().registerAbility(this.toplevelId, this.context);

      hilog.info(DOMAIN, TAG, '[WWA] onCreate OK tl=%{public}d', this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR onCreate EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '[WWA]  onWindowStageCreate tl=%{public}d cleanedUp=%{public}s',
      this.toplevelId, this.cleanedUp ? 'yes' : 'no');

    // 检查是否已清理
    if (this.cleanedUp) {
      hilog.warn(DOMAIN, TAG, '[WWA] ERR #%{public}d already cleaned up, skipping onWindowStageCreate', this.toplevelId);
      return;
    }

    try {
      //  设置 pending toplevel, WineWindow.aboutToAppear 同步读取 (loadContent 期间同步调用)
      hilog.info(DOMAIN, TAG, '[WWA] -> setPendingToplevel(%{public}d)', this.toplevelId);
      testNapi.setPendingToplevel(this.toplevelId);

      let mainWindow = windowStage.getMainWindowSync();
      hilog.info(DOMAIN, TAG, '[WWA] mainWindow=%{public}s', mainWindow ? 'ok' : 'NULL');

      if (!mainWindow) {
        hilog.error(DOMAIN, TAG, '[WWA] ERR #%{public}d getMainWindowSync returned NULL!', this.toplevelId);
        return;
      }

      // 注册 Window 引用
      WineWindowManager.getInstance().registerWindow(this.toplevelId, mainWindow);

      // 应用 pending 事件
      WineWindowManager.getInstance().flushPendingEvents(this.toplevelId);

      // 监听窗口事件 (关闭 / 最小化 / 最大化等)
      mainWindow.on('windowEvent', (eventType: number) => {
        hilog.info(DOMAIN, TAG, '[WWA] #%{public}d windowEvent type=%{public}d', this.toplevelId, eventType);
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          hilog.info(DOMAIN, TAG, '[WWA] #%{public}d  WINDOW_DESTROYED (user closed)', this.toplevelId);
          this.cleanup();
        }
      });

      hilog.info(DOMAIN, TAG, '[WWA] -> loadContent pages/WineWindow...');
      windowStage.loadContent('pages/WineWindow', (err) => {
        if (err.code) {
          hilog.error(DOMAIN, TAG, '[WWA] ERR #%{public}d loadContent FAILED: %{public}s',
            this.toplevelId, JSON.stringify(err));
          return;
        }
        hilog.info(DOMAIN, TAG, '[WWA] loadContent OK tl=%{public}d', this.toplevelId);
        //  窗口装饰配置 -- 必须在 loadContent 之后调用
        try {
          const mw = windowStage.getMainWindowSync();
          if (mw) {
            mw.setWindowTitleButtonVisible(false, false, false);
            mw.setWindowDecorVisible(false);
            mw.setWindowDecorHeight(40);
            hilog.info(DOMAIN, TAG, '[WWA] window decor config OK');
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[WWA] window decor config failed: %{public}s', JSON.stringify(e));
        }
        //  loadContent 后窗口装饰已就绪, 可获取标题栏高度
        WineWindowManager.getInstance().onAfterLoadContent(this.toplevelId);
      });

      hilog.info(DOMAIN, TAG, '[WWA] onWindowStageCreate OK tl=%{public}d', this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR onWindowStageCreate EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '[WWA] onForeground tl=%{public}d', this.toplevelId);
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '[WWA] onBackground tl=%{public}d', this.toplevelId);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '[WWA]  onDestroy tl=%{public}d (cleanedUp=%{public}s)',
      this.toplevelId, this.cleanedUp ? 'yes' : 'no');
    try {
      if (!this.cleanedUp) {
        this.cleanup();
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR onDestroy EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
    hilog.info(DOMAIN, TAG, '[WWA] onDestroy OK tl=%{public}d', this.toplevelId);
  }

  private cleanup(): void {
    if (this.cleanedUp) {
      hilog.warn(DOMAIN, TAG, '[WWA] cleanup tl=%{public}d: already cleaned, skip', this.toplevelId);
      return;
    }
    this.cleanedUp = true;
    hilog.info(DOMAIN, TAG, '[WWA]  cleanup tl=%{public}d START: sendToplevelClose -> destroyToplevel -> unregisterAbility',
      this.toplevelId);
    try {
      testNapi.sendToplevelClose(this.toplevelId);
      hilog.info(DOMAIN, TAG, '[WWA] cleanup tl=%{public}d: sendToplevelClose done', this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR cleanup sendToplevelClose EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
    try {
      testNapi.destroyToplevel(this.toplevelId);
      hilog.info(DOMAIN, TAG, '[WWA] cleanup tl=%{public}d: destroyToplevel done', this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR cleanup destroyToplevel EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
    try {
      WineWindowManager.getInstance().unregisterAbility(this.toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[WWA] ERR cleanup unregisterAbility EXCEPTION tl=%{public}d: %{public}s',
        this.toplevelId, JSON.stringify(e));
    }
    hilog.info(DOMAIN, TAG, '[WWA] cleanup tl=%{public}d DONE', this.toplevelId);
  }
}

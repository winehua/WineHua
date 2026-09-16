import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { PopupWindowManager } from '../service/PopupWindowManager';

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
      // 注册主窗口 id: 游戏相对模式锁光标 (LockCursor) 用
      // [WINID] WWA 主窗口身份打点 (对照 WMS win NNN 归属)
      hilog.info(DOMAIN, TAG, '[WWA] mainWindow id=%{public}d', mainWindow.getWindowProperties().id);
      testNapi.registerHostWindow(mainWindow.getWindowProperties().id);
      // 注册 WindowStage 引用 (popup 子窗口创建入口)
      WineWindowManager.getInstance().registerWindowStage(this.toplevelId, windowStage);

      // 父窗口移动/缩放 → 重新定位其 popup 子窗口
      mainWindow.on('windowRectChange', (data: window.RectChangeOptions) => {
        const mgr = WineWindowManager.getInstance();
        PopupWindowManager.getInstance().repositionPopups(
          this.toplevelId, mainWindow, mgr.getEffectiveScale());
        // 拖拽缩放 (DRAG_START/DRAG) → 标记缩放中, onSurfaceChanged 的
        // configure 据此带 RESIZING (Wine 才采用新尺寸); DRAG_END 发一次
        // 0 尺寸 configure 让 Wine 退出 size-move (保持当前尺寸)
        if (data.reason === window.RectChangeReason.DRAG_START ||
            data.reason === window.RectChangeReason.DRAG) {
          mgr.setResizeDragging(this.toplevelId, true);
        } else if (data.reason === window.RectChangeReason.DRAG_END) {
          mgr.setResizeDragging(this.toplevelId, false);
          try {
            testNapi.notifyToplevelResizeEnd(this.toplevelId);
          } catch (e) {
            hilog.warn(DOMAIN, TAG, '[WWA] #%{public}d notifyToplevelResizeEnd fail: %{public}s',
              this.toplevelId, JSON.stringify(e));
          }
        }
      });

      // 应用 pending 事件
      WineWindowManager.getInstance().flushPendingEvents(this.toplevelId);

      // 禁止 OHOS 标题栏双击最大化 (与 Wine 内置最大化冲突),
      // 仍可通过 win.maximize() 程序调用最大化 (Wine 按钮)
      mainWindow.on('windowStatusChange', (statusType: number) => {
        hilog.info(DOMAIN, TAG, '[WWA] #%{public}d windowStatusChange status=%{public}d',
          this.toplevelId, statusType);
      });

      // 监听窗口事件 (关闭 / 最小化 / 恢复等)
      mainWindow.on('windowEvent', (eventType: number) => {
        hilog.info(DOMAIN, TAG, '[WWA] #%{public}d windowEvent type=%{public}d', this.toplevelId, eventType);
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          hilog.info(DOMAIN, TAG, '[WWA] #%{public}d  WINDOW_DESTROYED (user closed)', this.toplevelId);
          this.cleanup();
        } else if (eventType === window.WindowEventType.WINDOW_SHOWN) {
          hilog.info(DOMAIN, TAG, '[WWA] #%{public}d  WINDOW_SHOWN → resume input', this.toplevelId);
          try { testNapi.setToplevelVisible(this.toplevelId, true); } catch (_) {}
        } else if (eventType === window.WindowEventType.WINDOW_HIDDEN) {
          hilog.info(DOMAIN, TAG, '[WWA] #%{public}d  WINDOW_HIDDEN → suppress input', this.toplevelId);
          try { testNapi.setToplevelVisible(this.toplevelId, false); } catch (_) {}
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
            // 窗口底色黑 (兜底): 主修复在 WineWindow 页的 Stack 底 (ARGB 帧
            // 画面外的透明区透出的是它), 本层防页面加载前/重建的空窗期露
            // 系统默认色 (浅色模式白)。窗口属性须在 loadContent 后设置才生效
            // (同上标题栏配置); popup/modal 子窗口底色各自另设
            try { mw.setWindowBackgroundColor('#FF000000'); } catch (_) {}
            // 禁止 OHOS 标题栏双击最大化 (与 Wine 内置最大化冲突),
            // maximize() 程序调用不受影响
            try { mw.setWindowTitleMoveEnabled(false); } catch (_) {}
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

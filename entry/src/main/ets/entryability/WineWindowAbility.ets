import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { PopupWindowManager } from '../service/PopupWindowManager';
import { DisplaySettingsService } from '../service/DisplaySettingsService';
import { DisplayRouteService } from '../service/DisplayRouteService';
import { InputSettingsService } from '../service/InputSettingsService';

const DOMAIN = 0x0000;
const TAG = 'WWA';

export default class WineWindowAbility extends UIAbility {
  private toplevelId: number = 0;
  private cleanedUp: boolean = false;
  // D52: guest 坐标 (created 事件携带)。x/y = -1 表示无坐标 (wayland),
  // 不定位保持 incumbent 默认摆放。
  private initX: number = -1;
  private initY: number = -1;
  private initW: number = 0;
  private initH: number = 0;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // 融合模式窗口可能先于 Index 就绪 (自动化直启), 保证虚拟键盘设置可用
    InputSettingsService.getInstance().init(this.context);
    try {
      const params = want.parameters ?? {};
      this.toplevelId = Number(params.toplevelId) || 0;
      const title = params.title as string ?? '';
      this.initX = params.x === undefined ? -1 : Number(params.x);
      this.initY = params.y === undefined ? -1 : Number(params.y);
      this.initW = Number(params.w) || 0;
      this.initH = Number(params.h) || 0;

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

      // D52: x11 多窗按 guest 坐标摆放承载窗 (双窗并排, 不再叠放)。
      // guest 逻辑坐标 × effectiveScale = OHOS 物理像素。x/y = -1 (wayland
      // created 无坐标) 时跳过, 保持 OHOS 默认摆放 —— incumbent 不变。
      if (this.initX >= 0 && this.initY >= 0 && this.initW > 0 && this.initH > 0) {
        try {
          const scale = WineWindowManager.getInstance().getEffectiveScale();
          const px = Math.round(this.initX * scale);
          const py = Math.round(this.initY * scale);
          const pw = Math.round(this.initW * scale);
          const ph = Math.round(this.initH * scale);
          mainWindow.resize(pw, ph);
          mainWindow.moveWindowToGlobal(px, py);
          hilog.info(DOMAIN, TAG, '[WWA] #%{public}d positioned guest(%{public}d,%{public}d %{public}dx%{public}d) → px(%{public}d,%{public}d %{public}dx%{public}d)',
            this.toplevelId, this.initX, this.initY, this.initW, this.initH, px, py, pw, ph);
        } catch (e) {
          hilog.error(DOMAIN, TAG, '[WWA] ERR #%{public}d window position: %{public}s',
            this.toplevelId, JSON.stringify(e));
        }
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
        // 任务 #135: 位置回写 guest (host→wine 几何回写移动半边; WWA 承载
        // 路径与 FWM 子窗同款上报, 值源 = windowRect 原点 / scale; 仅 x11,
        // native 侧仍有权威门)。坐标契约见 WineWindowManager.reportCarrierMove。
        if (DisplayRouteService.getInstance().isX11()) {
          try {
            const s = mgr.getEffectiveScale();
            const rect = mainWindow.getWindowProperties().windowRect;
            testNapi.notifyToplevelMove(this.toplevelId, Math.round(rect.left / s), Math.round(rect.top / s));
          } catch (_) { /* ignore */ }
        }
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
            // 任务 #129 T2: 装饰按显示路线分叉 ——
            //   x11 (PC/2in1 承载): 开系统装饰。x11 无人绘制 caption
            //     (xwm 不画, winex11 managed 模式亦无自绘), 系统标题栏是
            //     唯一标题/拖动/关闭出口; 标题按钮给关闭出路 (点击 = 系统
            //     关窗 → WINDOW_DESTROYED → cleanup → sendToplevelClose
            //     x11 分支 = WM_DELETE_WINDOW, 保存提示语义保留)。
            //   wayland incumbent: 保持无装饰 (winewayland 自绘 CSD, 改动前
            //     行为逐字节不变 —— 隔离红线)。
            // 每步独立 try/catch + 日志: 装饰 API 设备能力面窄 (801/1300004
            // 各调用不同), 合并 try 会吞掉精确错误面 (2026-10-11 Pad 实测:
            // 合并 catch 只见 801 不知哪一步)。
            const step = (name: string, fn: () => void): void => {
              try {
                fn();
                hilog.info(DOMAIN, TAG, '[WWA] decor step %{public}s ok', name);
              } catch (e) {
                hilog.warn(DOMAIN, TAG, '[WWA] decor step %{public}s FAILED: %{public}s',
                  name, JSON.stringify(e));
              }
            };
            const isX11 = DisplayRouteService.getInstance().isX11();
            if (isX11) {
              step('decorVisible(true)', () => { mw.setWindowDecorVisible(true); });
              step('titleButtons(true,true,true)', () => { mw.setWindowTitleButtonVisible(true, true, true); });
              // 标题栏拖动 (x11 无 wine caption → 系统标题栏是唯一拖动面)。
              // 该开关同携双击最大化 —— x11 managed 下 wine 无 caption 自
              // 最大化路径, 无双最大化打架面 (实测确认见 ledger)。
              step('titleMove(true)', () => { mw.setWindowTitleMoveEnabled(true); });
            } else {
              step('titleButtons(false,false,false)', () => { mw.setWindowTitleButtonVisible(false, false, false); });
              step('decorVisible(false)', () => { mw.setWindowDecorVisible(false); });
              // 禁止 OHOS 标题栏双击最大化 (与 Wine 内置最大化冲突),
              // maximize() 程序调用不受影响
              step('titleMove(false)', () => { mw.setWindowTitleMoveEnabled(false); });
            }
            step('decorHeight(40)', () => { mw.setWindowDecorHeight(40); });
            // 窗口底色黑 (兜底): 主修复在 WineWindow 页的 Stack 底 (ARGB 帧
            // 画面外的透明区透出的是它), 本层防页面加载前/重建的空窗期露
            // 系统默认色 (浅色模式白)。窗口属性须在 loadContent 后设置才生效
            // (同上标题栏配置); popup/modal 子窗口底色各自另设
            step('bgColor', () => { mw.setWindowBackgroundColor('#FF000000'); });
            // 保持屏幕常亮 (设置项): 游戏跑在子窗口 (主要场景), 创建时应用持久化值
            step('keepScreenOn', () => {
              mw.setWindowKeepScreenOn(DisplaySettingsService.getInstance().keepScreenOn)
                .catch((e: Object) => {
                  hilog.warn(DOMAIN, TAG, '[WWA] setWindowKeepScreenOn fail: %{public}s', JSON.stringify(e));
                });
            });
            hilog.info(DOMAIN, TAG, '[WWA] window decor config done (x11=%{public}s)', isX11 ? 'yes' : 'no');
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[WWA] window decor config outer failed: %{public}s', JSON.stringify(e));
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
    if (this.toplevelId > 0) {
      try { testNapi.setToplevelVisible(this.toplevelId, true); } catch (_) {}
    }
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '[WWA] onBackground tl=%{public}d', this.toplevelId);
    if (this.toplevelId > 0) {
      try { testNapi.setToplevelVisible(this.toplevelId, false); } catch (_) {}
    }
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

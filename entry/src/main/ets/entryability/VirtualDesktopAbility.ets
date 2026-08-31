import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;
const TAG = 'VDA';

// PC 虚拟桌面窗口 (独立 Ability): 承载 desktop root 的 XComponent。
// 与 WineWindowAbility 的区别: 无 toplevelId (root 单例), 不 setPendingToplevel;
// 窗口用系统默认初始大小, 保留系统标题栏 (可拖动/缩放/最小化/关闭);
// "虚拟桌面按屏幕大小初始化"由 Wine 桌面分辨率保证 (setOutputSize)。
// 生命周期对齐 master DesktopAbility (registerDesktopAbility + ctx 模式):
//   - 手动拉起: 初始化完毕主界面呈现悬浮窗桌面, 点击悬浮窗 →
//     WineWindowManager.openDesktopWindow → startAbility
//   - 用户关窗 (✕): markDesktopWindowClosed — 引擎会话后台继续, 主界面
//     悬浮窗重新出现 (master 版 onDestroy 会 destroyToplevel 杀 root,
//     本窗口关窗不杀 — PC 虚拟桌面要求"关窗后台运行", 与 Pad 全屏桌面
//     (关不掉)语义不同)
//   - root 销毁 (会话结束/热重启): WineWindowManager 持 ctx terminateSelf
export default class VirtualDesktopAbility extends UIAbility {
  private cleanedUp: boolean = false;
  // 沉浸最大化进行中 (windowStatusChange 防递归, 见 onWindowStageCreate)
  private immersiveMaximized: boolean = false;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, '[VDA] onCreate launchReason=%{public}d', launchParam.launchReason);
    try {
      // 注册 ctx (root 销毁时 manager 直接 terminateSelf 关窗) + 存活标记
      WineWindowManager.getInstance().registerDesktopWindow(this.context);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[VDA] ERR onCreate registerDesktopWindow: %{public}s', JSON.stringify(e));
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '[VDA] onWindowStageCreate cleanedUp=%{public}s',
      this.cleanedUp ? 'yes' : 'no');
    if (this.cleanedUp) {
      hilog.warn(DOMAIN, TAG, '[VDA] ERR already cleaned up, skipping onWindowStageCreate');
      return;
    }
    try {
      // 重新打开窗口: 重置沉浸态 — 上次会话可能停在沉浸最大化, 不重置则
      // immersiveMaximized / AppStorage vdImmersive 残留 → 悬浮"退出全屏"
      // 按钮误显示。新窗口一律从普通浮动 (FLOATING) 态开始。
      this.immersiveMaximized = false;
      AppStorage.setOrCreate('vdImmersive', false);

      const mainWindow = windowStage.getMainWindowSync();
      if (!mainWindow) {
        hilog.error(DOMAIN, TAG, '[VDA] ERR getMainWindowSync NULL');
        return;
      }
      mainWindow.setWindowTitle('Wine 桌面');
      // 窗口用系统默认初始大小 — "虚拟桌面按屏幕大小初始化"由 Wine 桌面
      // 分辨率保证 (WineWindowManager.init 的 setOutputSize = 屏幕/scale),
      // 窗口只是显示口: 任何尺寸下 renderer letterbox (ComputeFitRect)
      // 等比缩放呈现, 全屏化 (系统最大化) 恰好 1:1 无黑边。
      // 不在此 resize 到屏幕: 满屏窗口会盖住系统任务栏, 且窗口尺寸与
      // 桌面分辨率解耦后, 用户缩放窗口不影响 Wine 桌面布局。

      // 用户关窗 (✕/系统手势): 引擎会话后台继续, 仅清窗口标记
      // (root 销毁关窗由 WineWindowManager 持 ctx terminateSelf, 不在此处理)
      mainWindow.on('windowEvent', (eventType: number) => {
        hilog.info(DOMAIN, TAG, '[VDA] windowEvent type=%{public}d', eventType);
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          this.cleanup();
        }
      });

      // 最大化 → 沉浸全屏, 与游戏全屏同款路径:
      // ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER (API 14+): 铺满屏幕、
      // 自动隐藏 dock/状态栏/标题栏, 且鼠标悬停屏幕边缘热区时不唤出标题栏/
      // Dock — 默认 ENTER_IMMERSIVE 会 hover 底部浮现 dock, 抢走虚拟桌面里
      // 全屏游戏的边缘鼠标事件 (FPS 转向/边缘 UI 失效), 与游戏全屏同因。
      // 还原 → win.recover() 回普通窗口。
      // WindowStatusType (API≥14): 2=MAXIMIZE 4=FLOATING(普通自由窗口)。
      // 实测: 点最大化按钮发 2, 还原发 4。immersiveMaximized 防递归:
      // maximize()/recover() 自身会触发状态回调, 标志位保证只执行一次。
      mainWindow.on('windowStatusChange', (statusType: number) => {
        hilog.info(DOMAIN, TAG, '[VDA] windowStatusChange status=%{public}d immersive=%{public}s',
          statusType, this.immersiveMaximized ? 'yes' : 'no');
        if (statusType === window.WindowStatusType.MAXIMIZE) {
          // 2 = MAXIMIZE: 用户点最大化按钮。发起进入沉浸全屏 (DISABLE_HOVER:
          // hover 热区不唤出标题栏/dock, 不抢虚拟桌面边缘鼠标)。
          // maximize() 自身会触发回调, !immersiveMaximized 防递归。
          // 真正进入沉浸全屏后系统发 FULL_SCREEN(1), 由下个分支显示按钮。
          if (!this.immersiveMaximized) {
            this.immersiveMaximized = true;
            mainWindow.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER).then(() => {
              hilog.info(DOMAIN, TAG, '[VDA] maximize(DISABLE_TITLE_AND_DOCK_HOVER) OK');
            }).catch((e: Error) => {
              hilog.warn(DOMAIN, TAG, '[VDA] maximize failed: %{public}s', JSON.stringify(e));
              this.immersiveMaximized = false;
              AppStorage.setOrCreate('vdImmersive', false);
            });
          }
        } else if (statusType === window.WindowStatusType.FULL_SCREEN) {
          // 1 = FULL_SCREEN: 沉浸全屏稳定态 (maximize(DISABLE) 完成)。此时
          // 才是真正铺满屏幕盖住 dock 的状态 → 显示悬浮"退出全屏"按钮。
          // status=1 可能重复触发 (日志实测), setOrCreate 幂等无副作用
          AppStorage.setOrCreate('vdImmersive', true);
        } else {
          // 非全屏 (3 最小化 / 4 浮动 / 5 分屏): 一律隐藏还原按钮 + 重置
          // 沉浸态, 保证"只有全屏才显示退出全屏按钮"。窗口退出沉浸
          // (还原按钮 recover 或系统手势) 后系统已切到 FLOATING, 无需显式 recover。
          if (this.immersiveMaximized) {
            this.immersiveMaximized = false;
          }
          AppStorage.setOrCreate('vdImmersive', false);
        }
      });

      hilog.info(DOMAIN, TAG, '[VDA] -> loadContent pages/VirtualDesktop...');
      windowStage.loadContent('pages/VirtualDesktop', (err) => {
        if (err.code) {
          hilog.error(DOMAIN, TAG, '[VDA] ERR loadContent FAILED: %{public}s', JSON.stringify(err));
          return;
        }
        hilog.info(DOMAIN, TAG, '[VDA] loadContent OK');
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[VDA] ERR onWindowStageCreate EXCEPTION: %{public}s', JSON.stringify(e));
    }
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '[VDA] onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '[VDA] onBackground');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '[VDA] onDestroy (cleanedUp=%{public}s)', this.cleanedUp ? 'yes' : 'no');
    if (!this.cleanedUp) {
      this.cleanup();
    }
  }

  private cleanup(): void {
    if (this.cleanedUp) {
      hilog.warn(DOMAIN, TAG, '[VDA] cleanup: already cleaned, skip');
      return;
    }
    this.cleanedUp = true;
    hilog.info(DOMAIN, TAG, '[VDA] cleanup START');
    try {
      // 引擎会话继续后台运行, 仅清窗口标记 (重开入口由启动器提供)
      WineWindowManager.getInstance().markDesktopWindowClosed();
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[VDA] ERR cleanup markDesktopWindowClosed: %{public}s', JSON.stringify(e));
    }
    hilog.info(DOMAIN, TAG, '[VDA] cleanup DONE');
  }
}

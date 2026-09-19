import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'WineHuaScreenAwake';

/**
 * 屏幕常亮控制 (2026-09-18)
 *
 * 为什么产品化: 设备熄屏后会自动锁屏, 而锁屏状态下 `aa start` 会被系统拒绝
 * (10106102 The device screen is locked), 表现为"命令成功但什么都没启动"——
 * 自动化测试与 Steam 会话都会被这一点打断。应用在前台时保持屏幕常亮是最直接
 * 的解法, 顺带也符合游戏/桌面会话的使用预期 (玩的时候不该熄屏)。
 *
 * 只在 ability 前台期间开启, 退到后台立即关闭, 不影响后台耗电。
 */
export class ScreenAwake {
  private static mainWindow: window.Window | undefined = undefined;
  private static desired: boolean = false;
  private static applied: boolean | undefined = undefined;

  /** EntryAbility.onWindowStageCreate 里绑定主窗口。 */
  static attach(win: window.Window): void {
    ScreenAwake.mainWindow = win;
    ScreenAwake.applied = undefined;   // 换窗口后需要重新下发
    ScreenAwake.apply();
  }

  static setKeepScreenOn(on: boolean): void {
    ScreenAwake.desired = on;
    ScreenAwake.apply();
  }

  private static apply(): void {
    const win = ScreenAwake.mainWindow;
    if (!win) return;
    if (ScreenAwake.applied === ScreenAwake.desired) return;
    const desired = ScreenAwake.desired;
    win.setWindowKeepScreenOn(desired).then(() => {
      ScreenAwake.applied = desired;
      hilog.info(DOMAIN, TAG, 'keepScreenOn=%{public}s', String(desired));
    }).catch((err: Error) => {
      hilog.warn(DOMAIN, TAG, 'keepScreenOn=%{public}s failed: %{public}s',
        String(desired), JSON.stringify(err));
    });
  }
}

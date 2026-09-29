import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'WineHuaScreenAwake';

// Each binding serializes its own window operations. An old completion must not
// update a replacement window, or re-enable keep-awake after onBackground.
class ScreenAwakeWindow {
  readonly win: window.Window;
  private owner: string;
  private desired: boolean = false;
  private applied: boolean | undefined = undefined;
  private pending: boolean = false;

  constructor(win: window.Window, owner: string) {
    this.win = win;
    this.owner = owner;
  }

  setKeepScreenOn(on: boolean): void {
    this.desired = on;
    this.apply();
  }

  private async apply(): Promise<void> {
    if (this.pending || this.applied === this.desired) return;
    const requested = this.desired;
    this.pending = true;
    try {
      await this.win.setWindowKeepScreenOn(requested);
      this.applied = requested;
      hilog.info(DOMAIN, TAG, 'owner=%{public}s keepScreenOn=%{public}s', this.owner, String(requested));
    } catch (err) {
      this.applied = undefined;
      hilog.warn(DOMAIN, TAG, 'owner=%{public}s keepScreenOn=%{public}s failed: %{public}s',
        this.owner, String(requested), JSON.stringify(err));
    } finally {
      this.pending = false;
      // Consume a lifecycle change received while the request was pending.
      // A failed unchanged request is not retried indefinitely.
      if (this.desired !== requested) this.apply();
    }
  }
}

/** Per-Ability control: foreground game/desktop windows stay awake. */
export class ScreenAwake {
  private owner: string;
  private binding: ScreenAwakeWindow | undefined = undefined;
  private desired: boolean = false;

  constructor(owner: string) {
    this.owner = owner;
  }

  attach(win: window.Window): void {
    if (this.binding?.win === win) return;
    this.detach();
    this.binding = new ScreenAwakeWindow(win, this.owner);
    this.binding.setKeepScreenOn(this.desired);
  }

  setKeepScreenOn(on: boolean): void {
    this.desired = on;
    this.binding?.setKeepScreenOn(on);
  }

  detach(): void {
    this.binding?.setKeepScreenOn(false);
    this.binding = undefined;
  }
}

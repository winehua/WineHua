import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

export class PhoneGraphicsSettings {
  private static instance: PhoneGraphicsSettings | null = null;
  private store: preferences.Preferences | null = null;

  static getInstance(): PhoneGraphicsSettings {
    if (!PhoneGraphicsSettings.instance) PhoneGraphicsSettings.instance = new PhoneGraphicsSettings();
    return PhoneGraphicsSettings.instance;
  }

  init(ctx: common.Context): void {
    if (this.store) return;
    let scale = 2;
    let desktopResolution = 'auto';
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: 'phone_graphics' });
      scale = this.store.getSync('scale', 2) === 3 ? 3 : 2;
      desktopResolution = this.store.getSync('desktopResolution', 'auto') === '1024x768' ? '1024x768' : 'auto';
    } catch (err) {
      hilog.warn(0, 'wine', 'Phone graphics preference unavailable: %{public}s', JSON.stringify(err));
    }
    AppStorage.setOrCreate<number>('winehua.phone.scale.preference', scale);
    AppStorage.set<number>('winehua.phone.scale.preference', scale);
    AppStorage.setOrCreate<string>('winehua.phone.desktop.preference', desktopResolution);
    AppStorage.set<string>('winehua.phone.desktop.preference', desktopResolution);
  }

  setDesktopResolution(resolution: string): void {
    const selected = resolution === '1024x768' ? '1024x768' : 'auto';
    AppStorage.setOrCreate<string>('winehua.phone.desktop.preference', selected);
    AppStorage.set<string>('winehua.phone.desktop.preference', selected);
    try {
      if (this.store) {
        this.store.putSync('desktopResolution', selected);
        this.store.flushSync();
      }
    } catch (err) {
      hilog.warn(0, 'wine', 'Desktop resolution preference save failed: %{public}s', JSON.stringify(err));
    }
  }

  setScale(scale: number): void {
    const selected = scale === 3 ? 3 : 2;
    AppStorage.set<number>('winehua.phone.scale.preference', selected);
    try {
      if (this.store) {
        this.store.putSync('scale', selected);
        this.store.flushSync();
      }
    } catch (err) {
      hilog.warn(0, 'wine', 'Phone graphics preference save failed: %{public}s', JSON.stringify(err));
    }
  }
}

import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'display_settings';
const KEY_SCALE: string = 'scale';
const KEY_KEEP_SCREEN_ON: string = 'keep_screen_on';
const DEFAULT_SCALE: number = 2.0;
// 范围 [1, 3] (用户决策: 0.75 档位取消); 旧版本存过的 <1 值由 clamp 收敛到 1.0
const MIN_SCALE: number = 1.0;
const MAX_SCALE: number = 3.0;

// 供设置页选择器循环的预设档位 (非强制, setScale 只 clamp 不吸附)
export const SCALE_PRESETS: number[] = [1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0];

// 显示缩放 (全局, 用户决策: 不做单应用覆盖): 持久化用户缩放值。
// 生效机制 (HiDPI 化后): WineWindowManager.init 读此值下发 wine DPI
// (WINEHUA_LOGPIXELS = 96 × scale), wine 按物理分辨率原生渲染; 呈现层恒 1:1
// (effectiveScale 收敛到 DESKTOP_PRESENT_SCALE)。
// 生效时机: 仅应用启动 — 运行中改值不影响当前会话,
// 故本服务不提供 subscribe, 设置页改完提示重启即可。
export class DisplaySettingsService {
  private static instance: DisplaySettingsService | null = null;

  static getInstance(): DisplaySettingsService {
    if (!DisplaySettingsService.instance) {
      DisplaySettingsService.instance = new DisplaySettingsService();
    }
    return DisplaySettingsService.instance;
  }

  scale: number = DEFAULT_SCALE;
  // 保持屏幕常亮 (窗口级 keepScreenOn 标志, 参照 HiSH): 持久化同 store,
  // 生效时机 = 窗口创建/启动时 (EntryAbility 主窗口 + WineWindowAbility 子窗口
  // 各应用一次), 设置页切换时立即应用到主窗口 (winehua.mainWindow)
  keepScreenOn: boolean = false;

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const v = this.store.getSync(KEY_SCALE, DEFAULT_SCALE) as number;
      if (typeof v === 'number' && Number.isFinite(v)) {
        this.scale = this.clamp(v);
      }
      const keep = this.store.getSync(KEY_KEEP_SCREEN_ON, false) as boolean;
      if (typeof keep === 'boolean') {
        this.keepScreenOn = keep;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'display settings load fail: %{public}s', JSON.stringify(e));
    }
  }

  setScale(v: number): void {
    this.scale = this.clamp(v);
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_SCALE, this.scale);
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'display settings persist fail: %{public}s', JSON.stringify(e));
    }
  }

  setKeepScreenOn(v: boolean): void {
    this.keepScreenOn = v;
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_KEEP_SCREEN_ON, v);
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'keep screen on persist fail: %{public}s', JSON.stringify(e));
    }
  }

  private clamp(v: number): number {
    return Math.min(MAX_SCALE, Math.max(MIN_SCALE, v));
  }
}

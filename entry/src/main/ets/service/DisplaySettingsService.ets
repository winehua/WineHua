import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'display_settings';
const KEY_SCALE: string = 'scale';
const DEFAULT_SCALE: number = 2.0;
const MIN_SCALE: number = 0.75;
const MAX_SCALE: number = 3.0;

// 供设置页选择器循环的预设档位 (非强制, setScale 只 clamp 不吸附)
export const SCALE_PRESETS: number[] = [1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0];

// 显示缩放 (全局, 用户决策: 不做单应用覆盖): 持久化目标 effectiveScale。
// 生效时机: 仅应用启动 — WineWindowManager.init 读此值算 scaleBoost
// (effectiveScale = densityPixels × boost), 运行中改值不影响当前会话,
// 故本服务不提供 subscribe, 设置页改完自行刷新显示即可。
// 注: 此缩放只作用于 ArkTS 侧 "Wine 逻辑坐标 → 物理像素" 的窗口布局换算;
// native 的 setDisplayScale 已是兼容空操作, 不要试图经它联动。
export class DisplaySettingsService {
  private static instance: DisplaySettingsService | null = null;

  static getInstance(): DisplaySettingsService {
    if (!DisplaySettingsService.instance) {
      DisplaySettingsService.instance = new DisplaySettingsService();
    }
    return DisplaySettingsService.instance;
  }

  scale: number = DEFAULT_SCALE;

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

  private clamp(v: number): number {
    return Math.min(MAX_SCALE, Math.max(MIN_SCALE, v));
  }
}

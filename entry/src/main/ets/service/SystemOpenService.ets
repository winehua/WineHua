import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'system_open';
const KEY_ENABLED: string = 'enabled';
const DEFAULT_ENABLED: boolean = false;

// 系统打开 (实验): Wine 内打开 URL/无关联文件时, winebrowser 经 broker
// OPEN 请求交宿主 startAbility 用系统应用打开。默认关 = 上游行为
// (winebrowser 走 unix 兜底)。开关是 broker 门禁 (native 全局现读),
// 切换即时生效, 不需要重启引擎。
export class SystemOpenService {
  private static instance: SystemOpenService | null = null;

  static getInstance(): SystemOpenService {
    if (!SystemOpenService.instance) {
      SystemOpenService.instance = new SystemOpenService();
    }
    return SystemOpenService.instance;
  }

  enabled: boolean = DEFAULT_ENABLED;

  private store: preferences.Preferences | null = null;

  init(ctx: common.Context): void {
    if (this.store) {
      return;
    }
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      this.enabled = this.store.getSync(KEY_ENABLED, DEFAULT_ENABLED) as boolean;
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'system open load fail: %{public}s', JSON.stringify(e));
    }
    this.push();
  }

  setEnabled(v: boolean): void {
    this.enabled = v;
    if (this.store) {
      try {
        this.store.putSync(KEY_ENABLED, v);
        this.store.flush();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'system open persist fail: %{public}s', JSON.stringify(e));
      }
    }
    this.push();
  }

  // 推送到 broker 门禁 (proc/broker.cpp 全局)
  private push(): void {
    testNapi.setSystemOpenEnabled(this.enabled);
    hilog.info(DOMAIN, 'wine', 'system open enabled=%{public}d pushed', this.enabled ? 1 : 0);
  }
}

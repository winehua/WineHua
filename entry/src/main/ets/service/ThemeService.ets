import { preferences } from '@kit.ArkData';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'theme_settings';
const KEY_MODE: string = 'mode';

export const THEME_SYSTEM: string = 'system';   // 跟随系统
export const THEME_LIGHT: string = 'light';     // 浅色
export const THEME_DARK: string = 'dark';       // 深色

// 默认档 = 跟随系统 (用户决策 2026-09-12): 系统深色则应用深色, 系统浅色则浅色,
// 与平台其它应用行为一致。只在"从未设置过"时生效 — 用户选过的档位存在
// preferences 里, 读盘值优先于本默认值 (见 init)。
const DEFAULT_MODE: string = THEME_SYSTEM;

// 应用外观 (三档): 浅色 / 深色 / 跟随系统。落盘 preferences, 生效 = 立即 —
// setMode 调 ApplicationContext.setColorMode, 系统重新解析全部
// $r('app.color.*') 引用 (浅色取 base/element/color.json, 深色额外匹配
// dark/element/color.json 限定词目录), 无需重启应用。
//
// 边界: 只覆盖应用自身绘制的界面 (启动器/设置页/应用库/文件浏览器/任务列表/
// 对话框)。Wine 里的虚拟桌面、程序窗口、右键菜单由 Wine 自己绘制 (explorer/
// comctl32 那套主题), 与本设置无关; 要改那部分得改 Wine 注册表主题。
//
// 调用时序 (文档要求: 窗口已创建 + 页面 loadContent 完成):
//   冷启动 EntryAbility.onCreate → init(读盘), loadContent 回调 → apply();
//   设置页切换时页面早已就绪, setMode 内直接 apply。
// 同进程的 WineWindowAbility 子窗口共享 ApplicationContext, 无需各自 apply。
export class ThemeService {
  private static instance: ThemeService | null = null;

  static getInstance(): ThemeService {
    if (!ThemeService.instance) {
      ThemeService.instance = new ThemeService();
    }
    return ThemeService.instance;
  }

  mode: string = DEFAULT_MODE;

  private store: preferences.Preferences | null = null;
  private appCtx: common.ApplicationContext | null = null;
  private initialized: boolean = false;

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.appCtx = ctx.getApplicationContext();
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const v = this.store.getSync(KEY_MODE, DEFAULT_MODE) as string;
      if (v === THEME_SYSTEM || v === THEME_LIGHT || v === THEME_DARK) {
        this.mode = v;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'theme settings load fail: %{public}s', JSON.stringify(e));
    }
  }

  // 设置页切换: 落盘 + 立即生效
  setMode(v: string): void {
    if (v !== THEME_SYSTEM && v !== THEME_LIGHT && v !== THEME_DARK) {
      return;
    }
    this.mode = v;
    if (this.store) {
      try {
        this.store.putSync(KEY_MODE, v);
        this.store.flush();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'theme persist fail: %{public}s', JSON.stringify(e));
      }
    }
    this.apply();
  }

  // 应用当前模式 (幂等, 冷启动 loadContent 后调一次)
  apply(): void {
    if (!this.appCtx) {
      hilog.warn(DOMAIN, 'wine', 'theme apply skipped: no app context');
      return;
    }
    try {
      this.appCtx.setColorMode(this.toColorMode());
      hilog.info(DOMAIN, 'wine', 'theme applied: %{public}s', this.mode);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'theme apply fail: %{public}s', JSON.stringify(e));
    }
  }

  private toColorMode(): ConfigurationConstant.ColorMode {
    if (this.mode === THEME_LIGHT) {
      return ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT;
    }
    if (this.mode === THEME_DARK) {
      return ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    }
    return ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;
  }
}

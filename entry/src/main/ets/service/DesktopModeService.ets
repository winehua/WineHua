import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { deviceInfo } from '@kit.BasicServicesKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'desktop_mode';
const KEY_MODE: string = 'mode';
export const MODE_VIRTUAL: string = 'virtual';   // 虚拟桌面: 桌面合成器 + 独立窗口
export const MODE_FUSION: string = 'fusion';     // 多窗口模式: 每窗口独立 OHOS 窗口

// OpenHarmony (开鸿) 宿主判定: 开鸿系统禁用了 NCP (原生子进程) —
// appfwk.para 的 const.max_native_child_process=0, StartNativeChildProcess
// 返回 NCP_ERR_MULTI_PROCESS_DISABLED(16010004), 2026-09-12 实测。
// 用途**仅限**进程后端绕行 (EntryAbility 传给 setPhoneMode, cpp 侧走
// proc/phone_adapter 的 fork 实现), 不要拿它当形态判据 —— 进程创建方式
// 与交互形态是两件事: 开鸿 PC 是 PC 形态 (自由窗口/鼠标键盘), 只是因为
// 系统禁用了 NCP 才需要退回 fork 后端。
export function isOpenHarmonyHost(): boolean {
  return deviceInfo.osFullName.startsWith('OpenHarmony');
}

// 承载形态判据 (单一出处): tablet/phone 无系统自由窗口, 多窗口模式的每个窗口
// 用主窗口下的 subWindow 承载 (FusionWindowManager); 2in1/PC 用独立 UIAbility
// 主窗口 (系统自由窗口 + 原生视频)。
// 判据只看 deviceType, 不看宿主是否开鸿: 开鸿 PC (deviceType='pc') 同样按 PC
// 形态承载 (硬件也是 PC: 鼠标键盘 + 1920x1080 桌面), NCP 绕行由 setPhoneMode
// 单独承担 —— 2026-09-12 修: 此前 isOpenHarmonyHost() 短路让它落到移动形态,
// 开鸿 PC 上交互全走了手机逻辑。
export function isSubWindowFormFactor(): boolean {
  const t: string = deviceInfo.deviceType;
  // 2in1 与 PC 同属桌面形态: 部分设备 PC 上报 'pc' 而非 '2in1', 两者都要排除,
  // 否则 pc 会被当成移动形态落到 subWindow 承载
  return t !== '2in1' && t !== 'pc';
}

// 默认模式: 一律虚拟桌面 (2026-09-12 开鸿适配决策, 用户拍板)。
// 原因: 多窗口模式依赖"在窗口下挂附属子窗口"来承载 Wine 的弹出菜单/tooltip/
// 模态框 (PopupWindowManager、ModalWindowManager 的 createSubWindowWithOptions),
// 而开鸿 PC 的系统窗口管理不支持该接口 — 实测返回 1300002
// "The window is not created or destroyed" (WineWindowAbility 的 stage 和主窗
// stage 都试过, 同样失败), 结果菜单一律弹不出来。虚拟桌面模式下这些 popup 由
// C++ compositor 直接合成进桌面帧, 不依赖子窗口能力, 故统一走虚拟桌面。
// 相应地不再提供桌面模式切换: 设置页入口已移除, 这里的 mode 不再接受改写。
const DEFAULT_MODE: string = MODE_VIRTUAL;

// 桌面模式 (全形态可切): 持久化用户选择。生效时机: 仅应用启动 — WineWindowManager.init 读此值
// 判定 isDesktopMode (explorer 是否启动/桌面合成器是否开启), 运行中改值
// 不影响当前会话, 故本服务不提供 subscribe, 设置页改完自行刷新显示即可
// (与 DisplaySettingsService 同款, 见其"重启生效"约定)。
export class DesktopModeService {
  private static instance: DesktopModeService | null = null;

  static getInstance(): DesktopModeService {
    if (!DesktopModeService.instance) {
      DesktopModeService.instance = new DesktopModeService();
    }
    return DesktopModeService.instance;
  }

  mode: string = DEFAULT_MODE;

  // 显式模式覆盖 (aa want 试验通道, 不落盘): 优先于 mode/持久化值, 仅内存
  // 生效 — 走实例单例字段而非 AppStorage (want 解析在 Ability 生命周期,
  // AppStorage 跨上下文读取实测不可靠)
  private overrideMode: string = '';

  // 只接受 virtual: 多窗口模式依赖 createSubWindowWithOptions 承载菜单/模态框,
  // 开鸿 PC 不支持该接口 (1300002), 放行 fusion 会让菜单又弹不出来。
  setOverrideMode(v: string): void {
    if (v === MODE_VIRTUAL) {
      this.overrideMode = v;
      hilog.info(DOMAIN, 'wine', 'desktop mode override set: %{public}s', v);
    } else if (v === MODE_FUSION) {
      hilog.warn(DOMAIN, 'wine',
        'desktop mode override "%{public}s" ignored: multi-window unsupported on this host', v);
    }
  }

  getOverrideMode(): string {
    return this.overrideMode;
  }

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      // 桌面模式切换已移除: 不再沿用持久化值。老版本可能存过 fusion, 若读回来
      // 会在开鸿上切回多窗口模式, 菜单/模态框又会弹不出来 (见 DEFAULT_MODE 注释)。
      // 这里只报告, 不采纳 — mode 恒为 DEFAULT_MODE。
      this.mode = DEFAULT_MODE;
      const v = this.store.getSync(KEY_MODE, DEFAULT_MODE) as string;
      if (v !== DEFAULT_MODE) {
        hilog.info(DOMAIN, 'wine',
          'persisted desktop mode "%{public}s" ignored, forced to "%{public}s"', v, DEFAULT_MODE);
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode load fail: %{public}s', JSON.stringify(e));
    }
  }

  setMode(v: string): void {
    if (v !== MODE_VIRTUAL && v !== MODE_FUSION) {
      return;
    }
    this.mode = v;
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_MODE, this.mode);
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode persist fail: %{public}s', JSON.stringify(e));
    }
  }
}

import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
export const ROUTE_WAYLAND: string = 'wayland';
export const ROUTE_X11: string = 'x11';

// 显示路线裁决 (wayland=现状 / x11=实验台架): 仅显式 want 生效, 不持久化 ——
// 每次冷启动必须带参 (防"实验后 app 停在 x11"的僵尸态)。单例理由与结构同
// DesktopModeService (AppStorage 跨 Ability 上下文读取实测不可靠, 见
// EntryAbility 的 desktopMode 注释)。生效时机 = 冷启动解析一次, 引擎"重启
// Wine"等应用内会话操作不重置。
// 消费点白名单 (spec 2026-10-03 §2.2): DesktopWindow surface 分发 / 输入
// 分发 / native env 汇集点 (经 NAPI 镜像)。出现第四处 route 判断 = 违规。
export class DisplayRouteService {
  private static instance: DisplayRouteService | null = null;

  static getInstance(): DisplayRouteService {
    if (!DisplayRouteService.instance) {
      DisplayRouteService.instance = new DisplayRouteService();
    }
    return DisplayRouteService.instance;
  }

  private route: string = ROUTE_WAYLAND;

  setOverrideMode(v: string): void {
    if (v === ROUTE_WAYLAND || v === ROUTE_X11) {
      this.route = v;
      hilog.info(DOMAIN, 'wine', 'display route override set: %{public}s', v);
    } else {
      hilog.warn(DOMAIN, 'wine', 'display route override ignored: %{public}s', v);
    }
  }

  isX11(): boolean {
    return this.route === ROUTE_X11;
  }

  routeName(): string {
    return this.route;
  }
}

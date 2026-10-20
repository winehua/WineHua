import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import displayRouteNapi from 'libdisplay_route_napi.so';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'display_route';
const KEY_ROUTE: string = 'route';
export const ROUTE_WAYLAND: string = 'wayland';
export const ROUTE_X11: string = 'x11';
// 缺省 x11 (2026-10-10 用户拍板: x11 为当前主线, 默认即 x11 方便手工随时测试)
const DEFAULT_ROUTE: string = ROUTE_X11;

// 显示路线裁决 (wayland=原生路线对照 / x11=主线): 三级优先 —
// ① want 参数 winehua.displayRoute (内存覆盖, 最高优先, smoke/自动化钉档位用)
// ② 设置页「显示路线（实验）」持久化选择 (2026-10-11 起, 落 preferences)
// ③ 缺省 x11。
// 原「不持久化」约束 (spec 2026-10-03 §2.2) 被 ② 取代: 选择在设置页可见
// (非隐性僵尸态), 且缺省已是 x11。应用单点 = applyRoute (#172): 全 app 唯一
// 有权建立/切换「已应用路线」的入口 (盖 env 章 + 拆旧 x11 链 + 起 x11 链)。
// 消费方: EntryAbility 启动路径 (onCreate/onNewWant; 三级优先的解析留在
// 入口) 与设置页切换 (Index.selectRoute, 应用后经 restartEngine 编排重启
// 会话即生效, 不再要求完全退出应用)。smoke 编排直调 native 钉档+画布绑定
// (带 surfaceId/xclient 语义), 是测试通道, 不经本点。
// 消费点白名单 (spec 2026-10-03 §2.2): DesktopWindow surface 分发 / 输入
// 分发 / native env 汇集点 (经 NAPI 镜像) / 窗口装饰形态 (任务 #129 方向①:
// 顶条与系统装饰只在 x11 形态渲染, 判据单点 = FusionWindowManager.stripPxOf,
// WWA decor 分支同理 —— wayland incumbent 行为不变是硬约束, 装饰必须
// route 分叉)。新增消费点先改本注释并给单点判据, 再加代码。
export class DisplayRouteService {
  private static instance: DisplayRouteService | null = null;

  static getInstance(): DisplayRouteService {
    if (!DisplayRouteService.instance) {
      DisplayRouteService.instance = new DisplayRouteService();
    }
    return DisplayRouteService.instance;
  }

  private route: string = DEFAULT_ROUTE;      // 已应用值 (applyRoute 单点写)
  private applied: boolean = false;           // 本进程内是否已应用过 (冷启裁决前为 false)
  private preferred: string = DEFAULT_ROUTE;  // 持久化的设置页选择
  private store: preferences.Preferences | null = null;

  init(ctx: common.Context): void {
    if (this.store) {
      return;
    }
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const v = this.store.getSync(KEY_ROUTE, DEFAULT_ROUTE) as string;
      if (v === ROUTE_WAYLAND || v === ROUTE_X11) {
        this.preferred = v;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'display route load fail: %{public}s', JSON.stringify(e));
    }
  }

  // 设置页选择: 落盘持久化, 冷启动生效 (本会话已应用的 route 不变)
  setPreferred(v: string): void {
    if (v !== ROUTE_WAYLAND && v !== ROUTE_X11) {
      hilog.warn(DOMAIN, 'wine', 'display route preferred ignored: %{public}s', v);
      return;
    }
    this.preferred = v;
    if (!this.store) {
      hilog.warn(DOMAIN, 'wine', 'display route persist skipped (no store)');
      return;
    }
    try {
      this.store.putSync(KEY_ROUTE, this.preferred);
      this.store.flush();
      hilog.info(DOMAIN, 'wine', 'display route preferred set: %{public}s', v);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'display route persist fail: %{public}s', JSON.stringify(e));
    }
  }

  preferredRoute(): string {
    return this.preferred;
  }

  // 路线应用单点 (#172): 校验 → 幂等门 → 拆旧链 → 盖章 → 起新链 → 记账。
  // multiwindow = x11 链多窗模式位, 由调用方传入 (route=x11 × mode=fusion
  // 实时判定; 本类不 import WineWindowManager, 防循环依赖)。
  // 依据: x11 链 native 带可复位启停 (g_started 守卫重入, Stop 未启动
  // no-op); wayland 合成器 (WaylandServer) 生命周期 = 引擎会话, 随 StopAll
  // 收放, 无独立拆点。切换生效 = 本点 + 调用方 restartEngine (新 wine 的
  // env 章 spawn 时现读)。x11 早启依据: explorer 在 wineboot 后立刻 spawn,
  // 基础设施晚于它 59s 到达即 XOpenDisplay(:0) 落空 (2026-10-05 实测) ——
  // 起 x11 路线必须同拍起链 (surfaceId=0 仅内核形态, 画布后到经
  // StartWithSurface refresh first-caller-wins 挂 output)。
  // outW/outH: x11 链起输出尺寸 (逻辑分辨率 = 桌面分辨率, 消费方传
  // WineWindowManager.desktopOutputSize(); 0 = native 800x600 台架缺省,
  // smoke 台架路径专用)。桌面分辨率解耦语义的 x11 决定点 — 与 wayland
  // setOutputSize 同值 (桌面分辨率恒 = 屏幕/scale, 窗口只是显示口)。
  applyRoute(target: string, reason: string, multiwindow: boolean,
    outW: number = 0, outH: number = 0): void {
    if (target !== ROUTE_WAYLAND && target !== ROUTE_X11) {
      hilog.warn(DOMAIN, 'wine', '[DisplayRoute] applyRoute reject: %{public}s (%{public}s)',
        target, reason);
      return;
    }
    if (this.applied && this.route === target) {
      hilog.info(DOMAIN, 'wine', '[DisplayRoute] applyRoute no-op: %{public}s (%{public}s)',
        target, reason);
      return;
    }
    if (this.applied && this.route === ROUTE_X11 && target !== ROUTE_X11) {
      displayRouteNapi.stopDisplayRouteChain();
    }
    // env 章: 进程内单写点 (wine_env.cpp 镜像, spawn 现读)。不得吞异常:
    // 静默失败 = 后续 wine 进程全部走错路线且零痕迹 (2026-10-04 xwedge3
    // 实测: 印章缺失无任何日志, 排查多耗一轮构建)
    displayRouteNapi.setDisplayRoute(target);
    if (target === ROUTE_X11) {
      displayRouteNapi.startDisplayRouteChain(0n, false, outW, outH, false, multiwindow);
    }
    this.route = target;
    this.applied = true;
    // 留痕含 multiwindow 位 (2026-10-11 事故: 该值错判为 false 且无线索,
    // 排查需从 explorer 行为间接反推 —— 应用点必须打印)
    hilog.info(DOMAIN, 'wine', '[DisplayRoute] applied: %{public}s (reason=%{public}s, multiwindow=%{public}s)',
      target, reason, multiwindow ? 'true' : 'false');
  }

  isX11(): boolean {
    return this.route === ROUTE_X11;
  }

  routeName(): string {
    return this.route;
  }
}

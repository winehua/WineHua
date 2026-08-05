// 断点系统: "一套信息架构两种壳"的驱动源 (Step 4)。
// 阈值取自 HarmonyOS 多端部署推荐断点 (320/600/840vp), 本应用只用 sm/md/lg 三档:
//   sm < 600vp      手机形态     → 底部 Tabs
//   md 600~840vp    折叠展开/窄窗 → 左侧导航 + 右内容
//   lg >= 840vp     Pad 全屏/PC 宽窗 → 左侧导航 + 大卡栅格
// PC 窗口拉宽到 840vp 即 lg, 拉窄塌回 sm 底部 Tabs。
export type Breakpoint = 'sm' | 'md' | 'lg';

export class BreakpointSystem {
  private static instance: BreakpointSystem | null = null;

  static getInstance(): BreakpointSystem {
    if (!BreakpointSystem.instance) {
      BreakpointSystem.instance = new BreakpointSystem();
    }
    return BreakpointSystem.instance;
  }

  // 默认 lg (Pad 全屏): 拿到实际宽度前先按宽形态渲染, 根容器 onAreaChange 会立刻纠正
  current: Breakpoint = 'lg';

  private listeners: Array<() => void> = [];

  // 极简订阅: 忽略重复引用 (与既有 service 同款)
  subscribe(listener: () => void): void {
    if (this.listeners.indexOf(listener) >= 0) {
      return;
    }
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 由页面根容器 onAreaChange 喂入 (Area.width 单位 vp); 断点变化才 notify
  update(widthVp: number): void {
    let next: Breakpoint = 'lg';
    if (widthVp < 600) {
      next = 'sm';
    } else if (widthVp < 840) {
      next = 'md';
    }
    if (next != this.current) {
      this.current = next;
      this.notify();
    }
  }
}

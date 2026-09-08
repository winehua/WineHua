import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'FusionWM';

// Pad 多窗口模式的窗口承载 (subWindow 路线, 对齐 qucs/QPA 已验证机制)。
//
// 与 PC 融合模式的区别只在"每窗口是 UIAbility 主窗口 (多 window stage)"还是
// "主窗口 stage 下的 OHOS 子窗口" — wine/C++ 侧 (DisplayPolicy desktop=false)
// 完全相同, 全部变化收敛在本类 + WineWindowManager 的承载分叉:
//
//   - subWindow 是辅助窗口: 无系统标题栏/无任务卡片, 窗口装饰全部来自 Wine
//     自绘像素 + 我们提供的系统交互 (拖动/拉伸/层级)
//   - 拖动: 复用 wine xdg_toplevel.move → startMoving() (非自由窗口状态下
//     仅子窗口生效, 见 @ohos.window startMoving 文档 — 正是 Pad 需求)
//   - 拉伸: enableDrag(true) (API 20+, Phone/Tablet/2in1 可调用)
//   - 层级: 创建顺序 (后创建默认在上) + raiseToAppTop (点击置顶);
//     模态对话框由 ModalWindowManager 承载 (isModal 系统语义, 恒置顶)
//   - 大小: wine 逻辑尺寸 × effectiveScale (WineWindowManager 同款换算)
//
// 生命周期: created → ensure() (subWindow 创建+show) → 事件 (resize/position/
// fullscreen/raise...) → destroyed → destroy()。子窗口内容复用 pages/WineWindow
// (setPendingToplevel + XComponent 状态机与 UIAbility 承载完全相同)。
class FusionEntry {
  tl: number = 0;
  subWin: window.Window | null = null;
  w: number = 0;        // wine 逻辑尺寸 (resize/restore 基准)
  h: number = 0;
  creating: boolean = false;      // createSubWindow 异步飞行中 (防重复 ensure)
  destroyPending: boolean = false; // 创建期间收到 destroy → 完成后立即销毁
  fullscreen: boolean = false;    // 当前是否全屏化 (subWindow 无系统最大化,
                                  // 全屏 = resize 到显示尺寸 + 定位 0,0)
  frame: [number, number] | null = null; // 全屏前的 (x, y) 全局位置 (restore 基准)
}

export class FusionWindowManager {
  private static instance: FusionWindowManager | null = null;
  private entries: Map<number, FusionEntry> = new Map();
  private hostStage: window.WindowStage | null = null;
  private scale: number = 2.0;
  private scaleProvider: (() => number) | null = null;
  private cascadeCount: number = 0;

  static getInstance(): FusionWindowManager {
    if (!FusionWindowManager.instance) {
      FusionWindowManager.instance = new FusionWindowManager();
    }
    return FusionWindowManager.instance;
  }

  // hostStage = 主窗口 (EntryAbility) 的 WindowStage — subWindow 只能从
  // WindowStage 创建且宿主恒为主窗口 (OHOS 不支持窗口嵌套), 所有 Pad
  // 多窗口并列挂同一个 stage, 与 qucs/QPA 同构。
  initIfNeeded(stage: window.WindowStage, scaleProvider: () => number): void {
    this.hostStage = stage;
    this.scaleProvider = scaleProvider;
    this.scale = scaleProvider();
  }

  getScale(): number {
    return this.scaleProvider ? this.scaleProvider() : this.scale;
  }

  has(tl: number): boolean {
    return this.entries.has(tl);
  }

  getWindow(tl: number): window.Window | null {
    return this.entries.get(tl)?.subWin ?? null;
  }

  // -- created: 创建子窗口 (幂等: 已存在只做 resize 跟随, 不重建) --
  // onReady 在 subWindow 已 show 后同步调用 (registerWindow 回填 entry)。
  // onFail 在创建失败时调用 (WineWindowManager 清理 entry + 通知 Wine 关闭,
  // 否则 starting 标记卡死 — Wine 侧窗口不存在重发 created)。
  // w/h 为 wine 逻辑尺寸 (created 事件携带)。
  ensure(tl: number, w: number, h: number,
         onReady: (tl: number, win: window.Window) => void,
         onFail?: () => void): void {
    const existing = this.entries.get(tl);
    if (existing?.subWin) {
      this.resize(tl, Math.max(1, w), Math.max(1, h));
      return;
    }
    if (existing?.creating) {
      return; // 飞行中, onReady 完成后会走 reshow 分支 (hide 经 destroyPending)
    }
    if (!this.hostStage) {
      hilog.error(DOMAIN, TAG, '[FWM] ensure #%{public}d: hostStage not ready', tl);
      onFail?.();
      return;
    }

    const entry = new FusionEntry();
    entry.tl = tl;
    entry.w = Math.max(1, w);
    entry.h = Math.max(1, h);
    entry.creating = true;
    this.entries.set(tl, entry);

    // 与 WineWindow/ModalWindow 相同: aboutToAppear 同步读 pending toplevelId
    // (与 PC 同款 FIFO, PC 实测 OK)
    testNapi.setPendingToplevel(tl);

    this.hostStage.createSubWindowWithOptions(`wine_fusion_${tl}`, {
      title: '',
      decorEnabled: false,
      isModal: false
    }).then(async (subWin) => {
      try {
        await subWin.loadContent('pages/WineWindow');
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[FWM] #%{public}d loadContent FAILED: %{public}s',
          tl, JSON.stringify(e));
        this.finishFailed(tl);
        try { await subWin.destroyWindow(); } catch (_) { /* ignore */ }
        onFail?.();
        return;
      }
      entry.creating = false;
      entry.subWin = subWin;

      // 无系统装饰 → 只保留内容, 圆角/直角对齐 PC 承载
      try { subWin.setWindowCornerRadius(0); } catch (_) { /* ignore */ }
      // 窗口级拉伸 (边缘拖拽): API 20+, Phone/Tablet/2in1 可用
      try { await subWin.enableDrag(true); } catch (e) {
        hilog.warn(DOMAIN, TAG, '[FWM] #%{public}d enableDrag: %{public}s', tl, JSON.stringify(e));
      }
      // 屏幕常亮 (设置项值, 与 WineWindowAbility 创建时同款)
      try { subWin.setWindowKeepScreenOn(true); } catch (_) { /* ignore */ }

      // 创建期间收到 destroyed → 立即销毁
      if (entry.destroyPending) {
        hilog.info(DOMAIN, TAG, '[FWM] #%{public}d destroy requested while creating', tl);
        // destroy-while-creating: 页面可能未执行 aboutToAppear → 清 pending 残坑
        // (不删会使后续页面出队拿到死 id, 渲染器挂错 toplevel 黑屏)
        testNapi.cancelPendingToplevel(tl);
        this.destroy(tl);
        return;
      }

      // 位置: wine 逻辑 × scale; 初始为右下方向层叠 (无系统排布, 自定)
      try {
        await subWin.resize(this.toPx(entry.w), this.toPx(entry.h));
        const pos = this.cascadePoint(this.toPx(entry.w), this.toPx(entry.h));
        await subWin.moveWindowToGlobal(pos[0], pos[1]);
        await subWin.showWindow();
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[FWM] #%{public}d show failed: %{public}s', tl, JSON.stringify(e));
        this.destroy(tl);
        return;
      }
      hilog.info(DOMAIN, TAG, '[FWM] #%{public}d shown wine=%{public}dx%{public}d', tl, entry.w, entry.h);
      onReady(tl, subWin);
      this.updateImmersive();  // ≥1 窗口可见 → 沉浸窗口模式 (隐藏任务栏)
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, '[FWM] #%{public}d createSubWindow FAILED: %{public}s', tl, JSON.stringify(e));
      testNapi.cancelPendingToplevel(tl);  // 创建失败 → 同样清残坑
      this.finishFailed(tl);
      onFail?.();
    });
  }

  // 创建失败: 清 entry (后续 destroyed 事件/重建 ensure 都能工作)
  private finishFailed(tl: number): void {
    const e = this.entries.get(tl);
    if (e) e.creating = false;
    this.entries.delete(tl);
  }

  // -- 隐藏状态栏/通知栏 (Pad 多窗口): ≥1 个 wine 子树窗口渲染 → 主窗口
  //    setWindowSystemBarEnable([]) 隐藏状态栏 (窗口初始位置在屏幕顶部,
  //    标题栏会被状态栏遮挡, 拖不动); 最后一个窗口关闭 → 恢复显示。
  //    仅 fusion 子窗口承载计数 (popup/modal 生命周期绑父窗口, 不会独立存在)。
  //    注意: 状态栏可见性由主窗口配置, 主窗口全屏/最大化下生效 (Stage 默认)。
  private immersiveWanted: boolean = false;

  private updateImmersive(): void {
    let active = 0;
    for (const e of this.entries.values()) {
      if (e.subWin !== null) active++;
    }
    const want = active > 0;
    if (want === this.immersiveWanted) return;
    this.immersiveWanted = want;
    const bars: Array<'status' | 'navigation'> = want ? [] : ['status', 'navigation'];
    try {
      const mainWin = this.hostStage?.getMainWindowSync();
      if (mainWin) {
        mainWin.setWindowSystemBarEnable(bars).then(() => {
          hilog.info(DOMAIN, TAG, '[FWM] systemBar %{public}s (active=%{public}d)',
            want ? 'HIDDEN' : 'SHOWN', active);
        }).catch((e: Error) => {
          this.immersiveWanted = !want;  // 回滚标记, 下次再试
          hilog.warn(DOMAIN, TAG, '[FWM] setWindowSystemBarEnable: %{public}s', JSON.stringify(e));
        });
      } else {
        this.immersiveWanted = !want;
      }
    } catch (err) {
      this.immersiveWanted = !want;
      hilog.warn(DOMAIN, TAG, '[FWM] setWindowSystemBarEnable: %{public}s', JSON.stringify(err));
    }
  }

  private toPx(v: number): number {
    return Math.max(1, Math.round(v * this.getScale()));
  }

  // 右下方向层叠 (对齐自由窗口习惯): 每 6 个一轮, 步进右下各 56×scale
  private cascadePoint(w: number, h: number): [number, number] {
    try {
      const d = display.getDefaultDisplaySync();
      const margin = 24 * this.getScale();
      const step = 56 * this.getScale();
      const idx = this.cascadeCount % 6;
      this.cascadeCount++;
      const x = Math.max(margin, Math.round(d.width - w - margin - idx * step));
      const y = Math.max(margin, Math.round(margin + idx * step));
      return [x, y];
    } catch (_) {
      return [0, 0];
    }
  }

  // -- resize (wine 逻辑 × scale; WineWindowManager resize 事件转发) --
  resize(tl: number, w: number, h: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin) return;
    e.w = Math.max(1, w);
    e.h = Math.max(1, h);
    if (e.fullscreen) return; // 全屏期间不响应内容尺寸 (由 fullscreen/restore 掌管)
    e.subWin.resize(this.toPx(e.w), this.toPx(e.h)).catch(() => { /* ignore */ });
  }

  // Wine xdg_toplevel.move 拖动期间: OHOS 系统拖拽接管, 窗口位置由系统驱动
  // (WineWindow.onMouse → startMoving)。每次全局位置变化 → 重排 popup 子窗口
  // (WineWindowManager 在 windowRectChange 里做)。

  // -- 点击置顶 (ArkTS 侧显示层级; wine 侧 z/focus 由 wayland 协议自行管理) --
  raise(tl: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin) return;
    try {
      e.subWin.raiseToAppTop().catch(() => { /* ignore */ });
    } catch (_) { /* ignore */ }
  }

  // -- 全屏化 (subWindow 无系统 maximize; wine fullscreen/maximized 事件 →
  //    全屏 resize 到显示尺寸; 返回由 restore 执行) --
  fullscreen(tl: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin || e.fullscreen) return;
    try {
      const d = display.getDefaultDisplaySync();
      e.frame = [0, 0];
      try {
        const rect = e.subWin.getGlobalRect();
        e.frame = [rect.left, rect.top];
      } catch (_) { /* keep [0,0] */ }
      e.fullscreen = true;
      e.subWin.resize(d.width, d.height).then(() => {
        return e.subWin!.moveWindowToGlobal(0, 0);
      }).catch(() => { /* ignore */ });
      hilog.info(DOMAIN, TAG, '[FWM] #%{public}d fullscreen %{public}dx%{public}d', tl, d.width, d.height);
    } catch (err) {
      hilog.warn(DOMAIN, TAG, '[FWM] #%{public}d fullscreen: %{public}s', tl, JSON.stringify(err));
    }
  }

  restore(tl: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin) return;
    if (!e.fullscreen) {
      // 非全屏态: 按 wine 内容尺寸复位 (配置回弹场景)
      e.subWin.resize(this.toPx(e.w), this.toPx(e.h)).catch(() => { /* ignore */ });
      return;
    }
    e.fullscreen = false;
    const frame = e.frame ?? [48, 48];
    e.subWin.resize(this.toPx(e.w), this.toPx(e.h)).then(() => {
      return e.subWin!.moveWindowToGlobal(frame[0], frame[1]);
    }).catch(() => { /* ignore */ });
    hilog.info(DOMAIN, TAG, '[FWM] #%{public}d restore (%{public}d,%{public}d)', tl, frame[0], frame[1]);
  }

  // -- wine minimized/restored → 系统最小化/再显示。Window 接口无隐藏 API
  //    (hideWindow 不存在), 用 minimize() 近似: 非自由窗口设备上不生效也不
  //    报错 (与 maximize 同款设备行为差异), Pad 多窗口最坏就是窗口保持显示。
  hide(tl: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin) return;
    e.subWin.minimize().catch(() => { /* ignore */ });
    hilog.info(DOMAIN, TAG, '[FWM] #%{public}d minimize (wine minimized)', tl);
  }

  show(tl: number): void {
    const e = this.entries.get(tl);
    if (!e?.subWin) return;
    e.subWin.showWindow().catch(() => { /* ignore */ });
    hilog.info(DOMAIN, TAG, '[FWM] #%{public}d show (wine restored)', tl);
  }

  // -- destroyed: 销毁子窗口 (级联 popup/modal 由 WineWindowManager.unregisterAbility 做) --
  destroy(tl: number): void {
    const e = this.entries.get(tl);
    if (!e) return;
    if (e.creating) {
      e.destroyPending = true;
      return;
    }
    this.entries.delete(tl);
    const subWin = e.subWin;
    e.subWin = null;
    this.updateImmersive();  // 最后一个窗口关闭 → 退出沉浸窗口模式
    if (subWin) {
      try {
        subWin.destroyWindow().then(() => {
          hilog.info(DOMAIN, TAG, '[FWM] #%{public}d destroyed', tl);
        }).catch((err: Error) => {
          hilog.warn(DOMAIN, TAG, '[FWM] #%{public}d destroyWindow: %{public}s', tl, JSON.stringify(err));
        });
      } catch (err) {
        hilog.warn(DOMAIN, TAG, '[FWM] #%{public}d destroyWindow exception: %{public}s',
          tl, JSON.stringify(err));
      }
    }
  }

  // 会话退出兜底: 全部销毁
  destroyAll(): void {
    for (const tl of Array.from(this.entries.keys())) {
      this.destroy(tl);
    }
  }
}

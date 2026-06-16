import { common, Want } from '@kit.AbilityKit';
import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'WineWM';

export class WineWindowManager {
  private static instance: WineWindowManager | null = null;
  private windowStage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private uiAbilityContext: common.UIAbilityContext | null = null;
  // 设备显示密度 + 屏幕尺寸 (用于缩放)
  private displayScale: number = 1.0;
  private scaleBoost: number = 1.0;  // 放大系数, 使 effectiveScale = displayScale × boost = 2.0
  // 标题栏 + 边框总高度 (VP), 用于窗口 resize
  // = getWindowDecorHeight() + decorBorderOffset
  private measuredTitleBarH: number = 56;
  // API 返回的纯标题栏高度 (loadContent 后获取)
  private apiTitleBarH: number = 37;
  // 边框/阴影等 API 不涵盖的额外装饰 (VP), surface 校准自动测量
  private decorBorderOffset: number = 19;  // 56 - 37
  // 校准计数: 前几轮不做 EMA 平滑, 快速收敛
  private barCalibrationCount: number = 0;

  // toplevelId -> UIAbilityContext (用于 terminateSelf)
  private abilityContexts: Map<number, common.UIAbilityContext> = new Map();
  // toplevelId -> window.Window (用于直接操作 resize/title)
  private windows: Map<number, window.Window> = new Map();
  // toplevelId -> 已销毁标记 (用于竞态检测)
  private destroyed: Set<number> = new Set();
  // 正在启动中的 toplevel (防止重复 startAbility)
  private pendingStarts: Set<number> = new Set();
  // 窗口就绪前到达的 pending 事件
  private pendingEvents: Map<number, Array<[string, string]>> = new Map();
  // toplevelId -> surface 物理尺寸 (用于标题栏高度校准)
  private surfaceSizes: Map<number, [number, number]> = new Map();
  // toplevelId -> 最近一次 resize 的目标 VP 高度 (用于校准 titleBarH)
  private lastResizeTargetH: Map<number, number> = new Map();
  // toplevelId -> 最近一次 resize 的 wine 原始尺寸 (用于校准后重新 resize)
  private lastResizeWineDim: Map<number, [number, number]> = new Map();
  // toplevelId -> 最近一次设置的标题 (loadContent 后会重置, 需要重新 apply)
  private lastTitles: Map<number, string> = new Map();

  static getInstance(): WineWindowManager {
    if (!WineWindowManager.instance) {
      WineWindowManager.instance = new WineWindowManager();
    }
    return WineWindowManager.instance;
  }

  getMainWindow(): window.Window | null {
    return this.mainWindow;
  }

  // -- dump 当前状态 (诊断用) --
  dumpState(): void {
    hilog.info(DOMAIN, TAG, '[MW-DUMP] state: abilityCtx=%{public}d windows=%{public}d pendingStarts=%{public}d pendingEvents=%{public}d destroyed=%{public}d',
      this.abilityContexts.size, this.windows.size, this.pendingStarts.size,
      this.pendingEvents.size, this.destroyed.size);
    this.pendingStarts.forEach(id => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   pendingStart: #%{public}d', id);
    });
    this.pendingEvents.forEach((events, id) => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   pendingEvents #%{public}d: %{public}d items', id, events.length);
    });
    this.windows.forEach((_, id) => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   active window: #%{public}d', id);
    });
  }

  async init(windowStage: window.WindowStage, abilityContext: common.UIAbilityContext): Promise<void> {
    this.windowStage = windowStage;
    this.mainWindow = windowStage.getMainWindowSync();
    this.uiAbilityContext = abilityContext;

    hilog.info(DOMAIN, TAG, '[MW] init: mainWindow=%{public}s ctx=%{public}s',
      this.mainWindow ? 'ok' : 'NULL', this.uiAbilityContext ? 'ok' : 'NULL');

    // 获取设备显示密度
    try {
      const d = display.getDefaultDisplaySync();
      this.displayScale = d.densityPixels;
      this.scaleBoost = 2.0 / this.displayScale;  // 使 effectiveScale = 2.0
      hilog.info(DOMAIN, TAG, '[MW] display %{public}dx%{public}d deviceDPI=%{public}s boost=%{public}s -> effective=2.0',
        d.width, d.height, this.displayScale.toFixed(2), this.scaleBoost.toFixed(2));
      // 传给 C++ 渲染层 effective scale = 2.0
      testNapi.setDisplayScale(this.displayScale * this.scaleBoost);
      hilog.info(DOMAIN, TAG, '[MW] initialized scale=%{public}s display=%{public}dx%{public}d',
        this.displayScale.toFixed(2), d.width, d.height);
    } catch (_) {
      this.displayScale = 1.61;
      this.scaleBoost = 2.0 / 1.61;
      hilog.warn(DOMAIN, TAG, '[MW] display query failed, using displayScale=1.61 boost=1.24');
      testNapi.setDisplayScale(this.displayScale * this.scaleBoost);
    }

    hilog.info(DOMAIN, TAG, '[MW] registering toplevel callback...');
    testNapi.setToplevelCallback((id: number, event: string, data: string) => {
      hilog.info(DOMAIN, TAG, '[MW]  cb toplevel #%{public}d event=%{public}s data=%{public}s',
        id, event, data);
      this.onToplevelEvent(id, event, data);
    });
    hilog.info(DOMAIN, TAG, '[MW] toplevel callback registered OK');
  }

  // -- WineWindowAbility 调用: 注册 / 注销 --

  registerAbility(toplevelId: number, ctx: common.UIAbilityContext): void {
    this.abilityContexts.set(toplevelId, ctx);
    hilog.info(DOMAIN, TAG, '[MW]  registerAbility #%{public}d (total=%{public}d)',
      toplevelId, this.abilityContexts.size);
  }

  registerWindow(toplevelId: number, win: window.Window): void {
    this.windows.set(toplevelId, win);
    hilog.info(DOMAIN, TAG, '[MW]  registerWindow #%{public}d (total=%{public}d, pendingStarts=%{public}s)',
      toplevelId, this.windows.size, this.pendingStarts.has(toplevelId) ? 'yes' : 'no');
  }

  unregisterAbility(toplevelId: number): void {
    const hadCtx = this.abilityContexts.has(toplevelId);
    const hadWin = this.windows.has(toplevelId);
    const hadPend = this.pendingEvents.has(toplevelId);
    const hadStart = this.pendingStarts.has(toplevelId);
    const hadSurf = this.surfaceSizes.has(toplevelId);
    const hadResizeTarget = this.lastResizeTargetH.has(toplevelId);
    const hadWineDim = this.lastResizeWineDim.has(toplevelId);
    const hadTitle = this.lastTitles.has(toplevelId);
    this.abilityContexts.delete(toplevelId);
    this.windows.delete(toplevelId);
    this.pendingEvents.delete(toplevelId);
    this.pendingStarts.delete(toplevelId);
    this.surfaceSizes.delete(toplevelId);
    this.lastResizeTargetH.delete(toplevelId);
    this.lastResizeWineDim.delete(toplevelId);
    this.lastTitles.delete(toplevelId);
    hilog.info(DOMAIN, TAG, '[MW]  unregisterAbility #%{public}d (hadCtx=%{public}s hadWin=%{public}s hadPend=%{public}s hadStart=%{public}s hadSurf=%{public}s hadResizeTarget=%{public}s hadWineDim=%{public}s hadTitle=%{public}s)',
      toplevelId, hadCtx ? 'yes' : 'no', hadWin ? 'yes' : 'no',
      hadPend ? 'yes' : 'no', hadStart ? 'yes' : 'no', hadSurf ? 'yes' : 'no', hadResizeTarget ? 'yes' : 'no');
  }

  isDestroyed(toplevelId: number): boolean {
    const result = this.destroyed.has(toplevelId);
    if (result) {
      hilog.warn(DOMAIN, TAG, '[MW] isDestroyed #%{public}d = true', toplevelId);
    }
    return result;
  }

  // -- loadContent 回调: 窗口装饰就绪后校准标题栏高度 + 重设标题 --
  onAfterLoadContent(toplevelId: number): void {
    const win = this.windows.get(toplevelId);
    if (!win) return;

    // 1. 通过 API 获取标题栏高度 (必须在 loadContent 之后)
    //    getWindowDecorHeight 只返回标题栏文本区, 不含窗口边框。
    //    实际总装饰 = apiTitleBarH + decorBorderOffset
    try {
      const decorH = win.getWindowDecorHeight();
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight = %{public}d', toplevelId, decorH);
      if (decorH >= 37 && decorH <= 112) {
        this.apiTitleBarH = decorH;
        const newBarH = decorH + this.decorBorderOffset;
        if (Math.abs(newBarH - this.measuredTitleBarH) >= 2) {
          this.measuredTitleBarH = newBarH;
          hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: barH=%{public}d (API=%{public}d + offset=%{public}d)',
            toplevelId, newBarH, decorH, this.decorBorderOffset);
          const wd = this.lastResizeWineDim.get(toplevelId);
          if (wd) {
            this.applyEvent(toplevelId, 'resize', JSON.stringify({ w: wd[0], h: wd[1] }));
          }
        }
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight failed', toplevelId);
    }

    // 2.  方形窗口 (必须在 loadContent 之后, API 23 需要 FLOATING 模式)
    try {
      win.setWindowCornerRadius(0);
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius(0) done', toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius failed: %{public}s',
        toplevelId, JSON.stringify(e));
    }

    // 2. loadContent 会把窗口标题重置为默认值, 需重新应用 Wine 标题
    const title = this.lastTitles.get(toplevelId);
    if (title) {
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: reapply title "%{public}s"', toplevelId, title);
      win.setWindowTitle(title);
    }
  }

  // -- 兼容旧接口 --
  reapplyTitle(toplevelId: number): void {
    this.onAfterLoadContent(toplevelId);
  }

  // -- WineWindowAbility 在 onWindowStageCreate 中调用: 应用积压的事件 --
  flushPendingEvents(toplevelId: number): void {
    const pending = this.pendingEvents.get(toplevelId);
    if (!pending || pending.length === 0) {
      hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d: none', toplevelId);
      return;
    }
    this.pendingEvents.delete(toplevelId);

    hilog.info(DOMAIN, TAG, '[MW]  flushPendingEvents #%{public}d: %{public}d items', toplevelId, pending.length);
    for (let i = 0; i < pending.length; i++) {
      hilog.info(DOMAIN, TAG, '[MW]   flush[%{public}d/%{public}d] event=%{public}s data=%{public}s',
        i + 1, pending.length, pending[i][0], pending[i][1]);
      this.applyEvent(toplevelId, pending[i][0], pending[i][1]);
    }
    hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d done', toplevelId);
  }

  // -- toplevel 事件分发 --
  private onToplevelEvent(id: number, event: string, data: string): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelEvent #%{public}d event=%{public}s hasWindow=%{public}s',
      id, event, this.windows.has(id) ? 'yes' : 'no');

    switch (event) {
      case 'created':
        hilog.info(DOMAIN, TAG, '[MW] -> startWineWindowAbility(#%{public}d)', id);
        this.startWineWindowAbility(id, data);
        break;
      case 'destroyed':
        hilog.info(DOMAIN, TAG, '[MW] -> onToplevelDestroyed(#%{public}d)', id);
        this.onToplevelDestroyed(id);
        break;
      case 'surface':
        // C++ 回报 XComponent surface 的物理像素尺寸, 用于动态校准标题栏高度
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const sw = obj.w as number;
          const sh = obj.h as number;
          if (sw && sh && this.displayScale > 0) {
            // 总是保存最新的 surface 物理尺寸
            this.surfaceSizes.set(id, [sw, sh]);
            hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: physical=%{public}dx%{public}d scale=%{public}s',
              id, sw, sh, this.displayScale.toFixed(2));

            //  动态校准 decorBorderOffset:
            //    totalBarH = lastResizeTargetH - surfacePhysicalH/scale
            //    decorBorderOffset = totalBarH - apiTitleBarH
            const lastH = this.lastResizeTargetH.get(id);
            let calibrated = false;
            if (lastH) {
              const contentVPH = Math.round(sh / this.displayScale);
              const totalBarH = lastH - contentVPH;
              if (totalBarH > 0 && totalBarH < 200) {
                // 用 API 值计算边框偏移量
                const newOffset = totalBarH - this.apiTitleBarH;
                const oldOffset = this.decorBorderOffset;
                const oldBarH = this.measuredTitleBarH;
                // 前两轮直接用, 之后 EMA 平滑
                this.barCalibrationCount++;
                if (this.barCalibrationCount <= 2) {
                  this.decorBorderOffset = newOffset;
                } else {
                  this.decorBorderOffset = Math.round(oldOffset * 0.7 + newOffset * 0.3);
                }
                this.measuredTitleBarH = this.apiTitleBarH + this.decorBorderOffset;
                calibrated = (Math.abs(this.measuredTitleBarH - oldBarH) >= 1);
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: calibrate round#%{public}d totalBar=%{public}d offset=%{public}d->%{public}d barH=%{public}d (lastH=%{public}d contentVPH=%{public}d API=%{public}d)',
                  id, this.barCalibrationCount, totalBarH, oldOffset, this.decorBorderOffset,
                  this.measuredTitleBarH, lastH, contentVPH, this.apiTitleBarH);
              }
            }
            // 如果校准后 titleBarH 变了, 用保存的 wine 尺寸重新 resize
            if (calibrated && this.windows.has(id)) {
              const wd = this.lastResizeWineDim.get(id);
              if (wd) {
                const newData = JSON.stringify({ w: wd[0], h: wd[1] });
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: re-resize with calibrated bar=%{public}d', id, this.measuredTitleBarH);
                this.applyEvent(id, 'resize', newData);
              }
            }
          }
        } catch (_) { /* ignore parse error */ }
        break;
      case 'title':
      case 'resize':
        if (this.windows.has(id)) {
          hilog.info(DOMAIN, TAG, '[MW] -> applyEvent directly #%{public}d %{public}s', id, event);
          this.applyEvent(id, event, data);
        } else {
          hilog.info(DOMAIN, TAG, '[MW] -> queue pending #%{public}d %{public}s (window not ready)', id, event);
          let q = this.pendingEvents.get(id);
          if (!q) { q = []; this.pendingEvents.set(id, q); }
          q.push([event, data]);
          hilog.info(DOMAIN, TAG, '[MW]   pending queue size now: %{public}d', q.length);
        }
        break;
      default:
        hilog.warn(DOMAIN, TAG, '[MW] unknown event #%{public}d %{public}s', id, event);
        break;
    }
  }

  // -- apply 单个事件到 window.Window --
  private applyEvent(toplevelId: number, event: string, data: string): void {
    const win = this.windows.get(toplevelId);
    if (!win) {
      hilog.warn(DOMAIN, TAG, '[MW] applyEvent #%{public}d: window NOT FOUND (unregistered?)', toplevelId);
      return;
    }

    switch (event) {
      case 'title':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          if (obj.title) {
            const titleStr = obj.title as string;
            this.lastTitles.set(toplevelId, titleStr);
            hilog.info(DOMAIN, TAG, '[MW] setWindowTitle #%{public}d -> "%{public}s"', toplevelId, titleStr);
            win.setWindowTitle(titleStr);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent title parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'resize':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const w = obj.w as number;
          const h = obj.h as number;
          if (w && h) {
            const barH = 0; //  标题栏已通过 setWindowDecorVisible(false) 隐藏
            const s = this.displayScale * this.scaleBoost;  // effective scale = 2.0
            const winW = Math.round(w * s);
            const winH = Math.round(h * s + barH);
            //  保存目标尺寸, 供 surface 事件校准 titleBarH + 重新 resize
            this.lastResizeTargetH.set(toplevelId, winH);
            this.lastResizeWineDim.set(toplevelId, [w, h]);
            hilog.info(DOMAIN, TAG, '[MW-RESIZE] tl=%{public}d wine=%{public}dx%{public}d window=%{public}dx%{public}d scale=%{public}s',
              toplevelId, w, h, winW, winH, s.toFixed(1));
            win.resize(winW, winH);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent resize parse error: %{public}s', JSON.stringify(e));
        }
        break;
    }
  }

  // -- startAbility 启动 multiton 实例 --
  private startWineWindowAbility(id: number, data: string): void {
    if (this.pendingStarts.has(id)) {
      hilog.warn(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: already in flight, skip', id);
      return;
    }
    this.pendingStarts.add(id);
    hilog.info(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: pendingStarts now %{public}d',
      id, this.pendingStarts.size);

    // 解析初始参数
    let title = '';
    try {
      const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
      if (obj.title) title = obj.title as string;
    } catch (_) { /* use defaults */ }

    hilog.info(DOMAIN, TAG, '[MW]  startAbility(WineWindowAbility) #%{public}d title="%{public}s"',
      id, title);

    if (!this.uiAbilityContext) {
      hilog.error(DOMAIN, TAG, '[MW] FATAL: no uiAbilityContext, cannot start ability');
      this.pendingStarts.delete(id);
      return;
    }

    const want: Want = {
      bundleName: 'app.hackeris.honwine',
      abilityName: 'WineWindowAbility',
      parameters: { toplevelId: id, title: title }
    };

    hilog.info(DOMAIN, TAG, '[MW] startAbility want: bundle=%{public}s ability=%{public}s params=%{public}s',
      want.bundleName ?? '', want.abilityName ?? '', JSON.stringify(want.parameters));

    this.uiAbilityContext.startAbility(want).then(() => {
      hilog.info(DOMAIN, TAG, '[MW] OK startAbility #%{public}d resolved (AMS accepted)', id);
      this.pendingStarts.delete(id);
    }).catch((err: Error) => {
      hilog.error(DOMAIN, TAG, '[MW] ERR startAbility #%{public}d REJECTED: %{public}s',
        id, JSON.stringify(err));
      this.pendingStarts.delete(id);
      // 通知 Wine 窗口创建失败
      hilog.info(DOMAIN, TAG, '[MW] -> sendToplevelClose #%{public}d (fail recovery)', id);
      testNapi.sendToplevelClose(id);
    });
  }

  // -- Wine toplevel destroyed -> terminateSelf --
  private onToplevelDestroyed(id: number): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d', id);
    this.destroyed.add(id);

    // 如果 ability 侧已经清理过 (用户先关窗), 跳过
    if (!this.abilityContexts.has(id) && !this.pendingStarts.has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d: already cleaned up by ability, skip', id);
      return;
    }

    // 清理 C++ renderer
    hilog.info(DOMAIN, TAG, '[MW] -> destroyToplevel #%{public}d', id);
    testNapi.destroyToplevel(id);

    const ctx = this.abilityContexts.get(id);
    if (ctx) {
      hilog.info(DOMAIN, TAG, '[MW] -> terminateSelf #%{public}d', id);
      ctx.terminateSelf().catch((err: Error) => {
        hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf #%{public}d failed: %{public}s',
          id, JSON.stringify(err));
      });
    }
    this.unregisterAbility(id);
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d done', id);
  }
}

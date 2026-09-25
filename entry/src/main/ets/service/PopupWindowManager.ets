import { display, window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'PopupWM';

interface PopupEventData {
  popupId: number;
  x?: number;
  y?: number;
  w?: number;
  h?: number;
  argb?: number;
}

class PopupEntry {
  popupId: number = 0;
  parentToplevel: number = 0;
  subWin: window.Window | null = null;
  // Wine 逻辑坐标, 相对父窗口内容原点 (可为负/超出父窗口)
  offX: number = 0;
  offY: number = 0;
  w: number = 0;
  h: number = 0;
  argb: boolean = false;  // ARGB8888 帧 (圆角菜单/异型 tooltip), 子窗口需透明背景
  // 全屏 popup: 游戏 exclusive 全屏的渲染承载窗口 (popup 尺寸==模拟分辨率==
  // 父窗口 Wine 尺寸)。铺满宿主屏幕 (画面由 renderer fit 拉伸), 不跟随父窗口。
  fullscreenPopup: boolean = false;
}

/**
 * PC 多窗口模式 popup (菜单/tooltip/下拉框) 子窗口管理。
 *
 * C++ compositor 把 Wine 的 subsurface popup 登记为伪 toplevel 并发
 * popup_show/move/resize/hide 事件 (挂在父 toplevel id 上)。
 * 这里为每个 popup 创建一个独立 OHOS 子窗口 (无边框/不可聚焦),
 * 由 pages/WinePopup 的 XComponent 渲染像素 — 不再受父窗口边缘裁剪。
 *
 * 注意: 不 import WineWindowManager (避免循环依赖), 所需对象全部由参数传入。
 */
export class PopupWindowManager {
  private static instance: PopupWindowManager | null = null;
  private popups: Map<number, PopupEntry> = new Map();
  // 创建进行中的 popupId (异步 createSubWindow 期间收到 hide 时延迟销毁)
  private creating: Set<number> = new Set();
  private pendingDestroy: Set<number> = new Set();
  // 父窗口 windowStage 未就绪时积压的事件: parentToplevel -> [event, data]
  private pendingEvents: Map<number, Array<[string, string]>> = new Map();

  static getInstance(): PopupWindowManager {
    if (!PopupWindowManager.instance) {
      PopupWindowManager.instance = new PopupWindowManager();
    }
    return PopupWindowManager.instance;
  }

  // -- 事件入口 (由 WineWindowManager 分发) --
  onPopupEvent(parentToplevel: number, event: string, data: string,
               stage: window.WindowStage | null, parentWin: window.Window | null,
               scale: number, parentWineDim: [number, number] | null = null): void {
    /*
     * 风险标注 (P2): stage/parentWin 为空时所有事件都会积压, 包括 popup_hide。
     * hide 其实只需要 popupId 不需要 stage。当前由 WineWindowManager.unregisterAbility
     * 的级联销毁 (destroyPopupsForToplevel) 兜底, 不会泄漏子窗口;
     * 但 pendingEvents 会留一条永不 flush 的记录 (微量内存)。后续可改为
     * popup_hide 不经 stage 检查直接走 hide()。
     */
    if (!stage || !parentWin) {
      hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}s parent=#%{public}d: stage/window not ready, queue',
        event, parentToplevel);
      let q = this.pendingEvents.get(parentToplevel);
      if (!q) { q = []; this.pendingEvents.set(parentToplevel, q); }
      q.push([event, data]);
      return;
    }
    this.dispatch(parentToplevel, event, data, stage, parentWin, scale, parentWineDim);
  }

  // 父窗口就绪后 flush 积压事件 (WineWindowManager.flushPendingEvents 调用)
  flushPending(parentToplevel: number, stage: window.WindowStage,
               parentWin: window.Window, scale: number): void {
    const q = this.pendingEvents.get(parentToplevel);
    if (!q || q.length === 0) return;
    this.pendingEvents.delete(parentToplevel);
    hilog.info(DOMAIN, TAG, '[POPUP] flushPending parent=#%{public}d: %{public}d items',
      parentToplevel, q.length);
    for (const item of q) {
      this.dispatch(parentToplevel, item[0], item[1], stage, parentWin, scale);
    }
  }

  private dispatch(parentToplevel: number, event: string, data: string,
                   stage: window.WindowStage, parentWin: window.Window, scale: number,
                   parentWineDim: [number, number] | null = null): void {
    let parsed: PopupEventData;
    try {
      parsed = JSON.parse(data) as PopupEventData;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[POPUP] bad json parent=#%{public}d %{public}s: %{public}s',
        parentToplevel, event, data);
      return;
    }
    switch (event) {
      case 'popup_show':
        this.show(parentToplevel, parsed, stage, parentWin, scale, parentWineDim);
        break;
      case 'popup_move':
        this.move(parsed, parentWin, scale);
        break;
      case 'popup_resize':
        this.resizePopup(parsed, parentWin, scale);
        break;
      case 'popup_hide':
        this.hide(parsed.popupId);
        break;
      default:
        hilog.warn(DOMAIN, TAG, '[POPUP] unknown event %{public}s', event);
        break;
    }
  }

  // -- popup_show: 创建子窗口 --
  private show(parentToplevel: number, data: PopupEventData, stage: window.WindowStage,
               parentWin: window.Window, scale: number,
               parentWineDim: [number, number] | null = null): void {
    const popupId = data.popupId;
    if (this.popups.has(popupId) || this.creating.has(popupId)) {
      hilog.warn(DOMAIN, TAG, '[POPUP] show #%{public}d: already exists/creating, skip', popupId);
      return;
    }
    // 全屏 popup 判定: popup 尺寸==父窗口 Wine 尺寸 (游戏 exclusive 全屏的
    // 渲染承载窗口, popup 尺寸即模拟分辨率; 普通菜单/tooltip 远小于父窗口,
    // 不会误判)。铺满宿主屏幕, 画面由 renderer letterbox 拉伸。
    const fullscreenPopup = !!parentWineDim &&
      (data.w ?? 0) === parentWineDim[0] && (data.h ?? 0) === parentWineDim[1];
    hilog.info(DOMAIN, TAG,
      '[POPUP] show #%{public}d parent=#%{public}d off=(%{public}d,%{public}d) %{public}dx%{public}d fsPopup=%{public}s',
      popupId, parentToplevel, data.x ?? 0, data.y ?? 0, data.w ?? 0, data.h ?? 0,
      fullscreenPopup ? 'yes' : 'no');

    const entry = new PopupEntry();
    entry.popupId = popupId;
    entry.parentToplevel = parentToplevel;
    entry.offX = data.x ?? 0;
    entry.offY = data.y ?? 0;
    entry.w = data.w ?? 0;
    entry.h = data.h ?? 0;
    entry.argb = data.argb === 1;
    entry.fullscreenPopup = fullscreenPopup;
    this.creating.add(popupId);

    // 让 pages/WinePopup 的 aboutToAppear 同步取到 popupId (与 PC 同款 FIFO, PC 实测 OK)
    testNapi.setPendingToplevel(popupId);

    stage.createSubWindowWithOptions(`wine_popup_${popupId}`, {
      title: '',
      decorEnabled: false,
      isModal: false
    }).then(async (subWin) => {
      try {
        await subWin.setWindowFocusable(false);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}d setWindowFocusable failed: %{public}s',
          popupId, JSON.stringify(e));
      }
      try {
        await subWin.loadContent('pages/WinePopup');
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d loadContent FAILED: %{public}s',
          popupId, JSON.stringify(e));
        this.creating.delete(popupId);
        try { await subWin.destroyWindow(); } catch (_) { /* ignore */ }
        return;
      }
      try {
        subWin.setWindowCornerRadius(0);
      } catch (_) { /* ignore */ }
      try {
        // ARGB popup (圆角菜单/异型 tooltip) 用透明背景让 alpha 透过
        subWin.setWindowBackgroundColor(entry.argb ? '#00000000' : '#FF000000');
      } catch (_) { /* ignore */ }

      entry.subWin = subWin;
      this.creating.delete(popupId);

      // [WINID] popup 子窗口身份打点: 对照 WMS 日志 win NNN parent XXX 归属。
      // parentWin 即逻辑父窗口 (t1 的窗口); WMS 报的是物理父 (stage 主窗)。
      hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d subWinId=%{public}d parentWinId=%{public}d',
        popupId, subWin.getWindowProperties().id, parentWin.getWindowProperties().id);

      // 创建期间收到 hide → 立即销毁
      if (this.pendingDestroy.has(popupId)) {
        this.pendingDestroy.delete(popupId);
        hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d destroy requested while creating', popupId);
        this.destroyEntry(entry);
        return;
      }

      this.popups.set(popupId, entry);
      subWin.on('windowEvent', (eventType: number) => {
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d WINDOW_DESTROYED', popupId);
          this.popups.delete(popupId);
        }
      });

      try {
        if (entry.fullscreenPopup) {
          // 全屏 popup (游戏 exclusive 渲染承载窗口): 铺满宿主屏幕。
          // 画面由 renderer 的 letterbox fit 拉伸 (frame 模拟分辨率 → 屏幕)。
          const d = display.getDefaultDisplaySync();
          await subWin.resize(d.width, d.height);
          await subWin.moveWindowToGlobal(0, 0);
          hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d fullscreen popup → screen %{public}dx%{public}d',
            popupId, d.width, d.height);
        } else {
          await subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                              Math.max(1, Math.round(entry.h * scale)));
          await this.positionEntry(entry, parentWin, scale);
        }
        await subWin.showWindow();
        hilog.info(DOMAIN, TAG, '[POPUP] #%{public}d shown', popupId);
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d show failed: %{public}s',
          popupId, JSON.stringify(e));
        this.destroyEntry(entry);
      }
    }).catch((e: Error) => {
      this.creating.delete(popupId);
      testNapi.cancelPendingToplevel(popupId);  // 创建失败 → 同样清残坑
      hilog.error(DOMAIN, TAG, '[POPUP] #%{public}d createSubWindow FAILED: %{public}s',
        popupId, JSON.stringify(e));
    });
  }

  // -- popup_move / popup_resize --
  private move(data: PopupEventData, parentWin: window.Window, scale: number): void {
    const entry = this.popups.get(data.popupId);
    if (!entry || !entry.subWin) return;
    entry.offX = data.x ?? entry.offX;
    entry.offY = data.y ?? entry.offY;
    if (entry.fullscreenPopup) {
      entry.subWin.moveWindowToGlobal(0, 0).catch(() => { /* ignore */ });
    } else {
      this.positionEntry(entry, parentWin, scale).catch(() => { /* ignore */ });
    }
  }

  private resizePopup(data: PopupEventData, parentWin: window.Window, scale: number): void {
    const entry = this.popups.get(data.popupId);
    if (!entry || !entry.subWin) return;
    entry.w = data.w ?? entry.w;
    entry.h = data.h ?? entry.h;
    if (entry.fullscreenPopup) {
      // 全屏 popup: 保持铺满宿主屏幕 (Wine 侧 resize 只是模拟分辨率变化)
      const d = display.getDefaultDisplaySync();
      entry.subWin.resize(d.width, d.height).catch(() => { /* ignore */ });
    } else {
      entry.subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                          Math.max(1, Math.round(entry.h * scale))).then(() => {
        return this.positionEntry(entry, parentWin, scale);
      }).catch(() => { /* ignore */ });
    }
  }

  // -- popup_hide --
  private hide(popupId: number): void {
    if (this.creating.has(popupId)) {
      this.pendingDestroy.add(popupId);
      // 窗口关闭于创建中 (destroy-while-creating): 页面可能未执行 aboutToAppear,
      // pending 队列里的 id 无人消费 (残坑) — 立即清除, 否则后续每个 popup
      // 出队拿到这个死 id → 渲染器挂错 toplevel 黑屏 (1.8 实测)。页面已出队
      // 时队列无此 id, cancel 是 no-op。
      testNapi.cancelPendingToplevel(popupId);
      return;
    }
    const entry = this.popups.get(popupId);
    if (!entry) return;
    hilog.info(DOMAIN, TAG, '[POPUP] hide #%{public}d', popupId);
    this.destroyEntry(entry);
  }

  private destroyEntry(entry: PopupEntry): void {
    this.popups.delete(entry.popupId);
    if (entry.subWin) {
      entry.subWin.destroyWindow().catch((e: Error) => {
        hilog.warn(DOMAIN, TAG, '[POPUP] #%{public}d destroyWindow failed: %{public}s',
          entry.popupId, JSON.stringify(e));
      });
      entry.subWin = null;
    }
  }


  // 级联: 父窗口销毁时清理其全部 popup
  destroyPopupsForToplevel(parentToplevel: number): void {
    const doomed: PopupEntry[] = [];
    this.popups.forEach((entry) => {
      if (entry.parentToplevel === parentToplevel) doomed.push(entry);
    });
    for (const entry of doomed) {
      hilog.info(DOMAIN, TAG, '[POPUP] cascade destroy #%{public}d (parent #%{public}d destroyed)',
        entry.popupId, parentToplevel);
      this.destroyEntry(entry);
    }
  }

  // 父窗口移动/缩放 → 重新定位其全部 popup
  // (全局坐标 = 父窗口内容原点 + offset × scale, 无论 OHOS 是否自动跟随都收敛到正确位置)
  repositionPopups(parentToplevel: number, parentWin: window.Window, scale: number): void {
    this.popups.forEach((entry) => {
      if (entry.parentToplevel !== parentToplevel || !entry.subWin) return;
      // 全屏 popup 固定在屏幕 (0,0), 不跟随父窗口
      if (entry.fullscreenPopup) {
        entry.subWin.moveWindowToGlobal(0, 0).catch(() => { /* ignore */ });
        return;
      }
      this.positionEntry(entry, parentWin, scale).catch(() => { /* ignore */ });
    });
  }

  // 全局坐标 = 父窗口 windowRect (全局物理 px) + Wine 逻辑 offset × scale
  private async positionEntry(entry: PopupEntry, parentWin: window.Window, scale: number): Promise<void> {
    if (!entry.subWin) return;
    const rect = parentWin.getWindowProperties().windowRect;
    const gx = Math.round(rect.left + entry.offX * scale);
    const gy = Math.round(rect.top + entry.offY * scale);
    try {
      await entry.subWin.moveWindowToGlobal(gx, gy);
      hilog.info(DOMAIN, TAG, '[POPUP] move ok #%{public}d → (%{public}d,%{public}d)', entry.popupId, gx, gy);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[POPUP] move FAIL #%{public}d → (%{public}d,%{public}d): %{public}s',
        entry.popupId, gx, gy, JSON.stringify(e));
    }
  }

  // z 单元提升: 主窗点击置顶时其全部 popup 必须提到主窗之上 (否则画面被
  // 白壳主窗盖住 — 2026-09-10 白屏实锤)。主窗先 raise、popup 后 raise,
  // 保证"画面在顶层"的不变量。
  raisePopups(parentToplevel: number): void {
    this.popups.forEach((entry) => {
      if (entry.parentToplevel !== parentToplevel || !entry.subWin) return;
      try {
        entry.subWin.raiseToAppTop().catch(() => { /* ignore */ });
      } catch (_) { /* ignore */ }
    });
  }

  // 诊断: 某父窗口是否有活动 popup
  hasPopups(parentToplevel: number): boolean {
    let found = false;
    this.popups.forEach((entry) => {
      if (entry.parentToplevel === parentToplevel) found = true;
    });
    return found;
  }
}

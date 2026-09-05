import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'ModalWM';

// "modal" 事件数据 (C++ JsonModal): modal/owner/tl + dx/dy (相对 owner 的
// 桌面坐标差) + w/h (wine 逻辑尺寸)。ArkTS 侧按 Record<number> 解, 避免
// 跨 NAPI JSON 边界类型断言。
interface ModalEventData {
  modal: number;
  owner: number;
  tl: number;
  dx: number;  // modal 相对 owner 的桌面坐标差 (wine 逻辑)
  dy: number;
  w: number;   // modal 内容尺寸 (wine 逻辑)
  h: number;
}

class ModalEntry {
  modalId: number = 0;
  ownerId: number = 0;
  subWin: window.Window | null = null;
  w: number = 0;
  h: number = 0;
}

// owner 未就绪时暂存的 show 请求数据 (ArkTS 禁内联对象类型, 必须命名 interface)
interface PendingShow {
  data: Record<string, string | number>;
  ownerId: number;
}

/**
 * PC 多窗口模式 modal 对话框承载。
 *
 * C++ compositor 把 Wine 的模态对话框 (WS_EX_DLGMODALFRAME + owner 被禁用)
 * 经 winehua_toplevel 协议上报, 事件 "modal" (json 带相对 owner 的定位差/
 * 尺寸) 由 WineWindowManager 分发。这里在 owner 的 WindowStage 内创建
 * OHOS 模态子窗口 (isModal: true):
 *  - 子窗口是辅助窗口 → 不进任务栏/任务卡片 (主窗口才进)
 *  - OHOS 模态属性 → owner 主窗口不能响应用户操作, 直到本窗口关闭
 *    (与 Win32 模态对话框语义一致)
 * 窗口内容复用 pages/WineWindow (全键盘/触控/鼠标 + renderer), 与
 * PopupWindowManager 同构 (XComponent + createRenderer(modalId))。
 */
export class ModalWindowManager {
  private static instance: ModalWindowManager | null = null;
  private modals: Map<number, ModalEntry> = new Map();
  // 创建进行中的 modalId (异步 createSubWindow 期间收到 hide 时延后销毁)
  private creating: Set<number> = new Set();
  private pendingDestroy: Set<number> = new Set();
  // owner 窗口 (Ability) 未就绪时暂存的 show 请求 (每个 modalId 一条,
  // owner stage/win 回填后由 WineWindowManager.flushPendingEvents 触发)
  private pendingShows: Map<number, PendingShow> = new Map();

  static getInstance(): ModalWindowManager {
    if (!ModalWindowManager.instance) {
      ModalWindowManager.instance = new ModalWindowManager();
    }
    return ModalWindowManager.instance;
  }

  // -- modal=1: 创建模态子窗口 (owner 的 stage/win/scale 由调用方传入) --
  // d 为 JSON.parse 的原始 Record (字符串惯例, 键值数值字符串/数字混存),
  // 内部一律 Number() 化
  show(modalId: number, d: Record<string, string | number>,
       stage: window.WindowStage | null,
       parentWin: window.Window | null, scale: number): void {
    // 已存在 (Wine 每次 WindowPosChanged 重发 modal=1, 带新 dx/dy) →
    // 只做位置/尺寸跟随, 不重建子窗口
    const existing = this.modals.get(modalId);
    if (existing) {
      hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d re-show → reposition', modalId);
      if (parentWin) {
        this.reposition(existing, d, parentWin, scale);
      }
      return;
    }
    // 创建进行中: 重复 show 忽略 (hide 走 pendingDestroy 兜底)
    if (this.creating.has(modalId)) {
      return;
    }

    if (!stage || !parentWin) {
      // owner Ability 异步启动晚于对话框弹出 (启动即弹窗场景): 暂存,
      // owner 的 stage/win 回填后 (WineWindowManager.flushPendingEvents)
      // flushForOwner 补建 — drop 会让 ModalToplevelIds 吞掉后续一切事件
      // 且 owner 已被 wine 禁用, 应用表现为"点不动且无对话框"
      hilog.warn(DOMAIN, TAG, '[MODAL] show #%{public}d: owner #%{public}d stage/win not ready, queue',
        modalId, d.owner ?? 0);
      const pending: PendingShow = { data: d, ownerId: Number(d.owner ?? 0) };
      this.pendingShows.set(modalId, pending);
      return;
    }
    const entry = new ModalEntry();
    entry.modalId = modalId;
    entry.ownerId = Number(d.owner ?? 0);
    entry.w = Math.max(1, Number(d.w ?? 0));
    entry.h = Math.max(1, Number(d.h ?? 0));
    this.creating.add(modalId);

    // 与 WineWindow 同模式: aboutToAppear 读 pending toplevelId (同步先后序)
    testNapi.setPendingToplevel(modalId);

    hilog.info(DOMAIN, TAG,
      '[MODAL] show #%{public}d owner=#%{public}d wine=%{public}dx%{public}d rel=(%{public}d,%{public}d) scale=%{public}s',
      modalId, entry.ownerId, entry.w, entry.h, d.dx ?? 0, d.dy ?? 0, scale.toFixed(2));

    stage.createSubWindowWithOptions(`wine_modal_${modalId}`, {
      title: '',
      decorEnabled: false,
      isModal: true
    }).then(async (subWin) => {
      try {
        await subWin.loadContent('pages/WineWindow');
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d loadContent FAILED: %{public}s',
          modalId, JSON.stringify(e));
        this.creating.delete(modalId);
        this.pendingDestroy.delete(modalId);  // 失败路径同样清理, 防下次 show 闪灭
        try { await subWin.destroyWindow(); } catch (_) { /* ignore */ }
        return;
      }
      try {
        subWin.setWindowBackgroundColor('#FF000000');
      } catch (_) { /* ignore */ }
      try {
        subWin.setWindowCornerRadius(0);
      } catch (_) { /* ignore */ }

      entry.subWin = subWin;
      this.creating.delete(modalId);

      // 创建期间收到 hide → 立即销毁
      if (this.pendingDestroy.has(modalId)) {
        this.pendingDestroy.delete(modalId);
        hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d destroy requested while creating', modalId);
        this.destroyEntry(entry);
        return;
      }
      this.modals.set(modalId, entry);

      try {
        // 尺寸: wine 逻辑 × scale; 位置: owner 屏幕位置 + 相对差 × scale
        await subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                            Math.max(1, Math.round(entry.h * scale)));
        const pr = parentWin.getWindowProperties().windowRect;
        const dx = Number(d.dx ?? 0);
        const dy = Number(d.dy ?? 0);
        await subWin.moveWindowToGlobal(
          pr.left + Math.round(dx * scale),
          pr.top + Math.round(dy * scale));
        await subWin.showWindow();
        hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d shown at screen(%{public}d,%{public}d)',
          modalId, pr.left + Math.round(dx * scale),
          pr.top + Math.round(dy * scale));
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d show failed: %{public}s',
          modalId, JSON.stringify(e));
        this.destroyEntry(entry);
      }
    }).catch((e: Error) => {
      this.creating.delete(modalId);
      this.pendingDestroy.delete(modalId);  // 失败路径同样清理
      hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d createSubWindow FAILED: %{public}s',
        modalId, JSON.stringify(e));
    });
  }

  // -- owner (Ability) 的 stage/win 回填后补建暂存的 show (WineWindowManager
  //    flushPendingEvents 调用) --
  flushForOwner(ownerId: number, stage: window.WindowStage | null,
                parentWin: window.Window | null, scale: number): void {
    if (!stage || !parentWin) return;
    let flushed = false;
    this.pendingShows.forEach((p, modalId) => {
      if (p.ownerId !== ownerId) return;
      this.pendingShows.delete(modalId);
      this.show(modalId, p.data, stage, parentWin, scale);
      flushed = true;
    });
    if (flushed) {
      hilog.info(DOMAIN, TAG, '[MODAL] flushForOwner #%{public}d done', ownerId);
    }
  }

  // -- 已存在 modal 的尺寸/位置跟随 (Wine 重发 modal=1 带新 dx/dy) --
  private reposition(entry: ModalEntry, d: Record<string, string | number>,
                     parentWin: window.Window, scale: number): void {
    if (!entry.subWin) return;
    const w = Math.max(1, Number(d.w ?? entry.w));
    const h = Math.max(1, Number(d.h ?? entry.h));
    entry.w = w;
    entry.h = h;
    const dx = Number(d.dx ?? 0);
    const dy = Number(d.dy ?? 0);
    entry.subWin.resize(Math.max(1, Math.round(w * scale)),
                        Math.max(1, Math.round(h * scale))).then(() => {
      const pr = parentWin.getWindowProperties().windowRect;
      return entry.subWin!.moveWindowToGlobal(
        pr.left + Math.round(dx * scale),
        pr.top + Math.round(dy * scale));
    }).catch(() => { /* ignore */ });
  }

  // -- resize 事件 (wine 逻辑尺寸 × scale) --
  onResize(modalId: number, data: string, scale: number): void {
    const entry = this.modals.get(modalId);
    if (!entry || !entry.subWin) return;
    try {
      const obj: Record<string, number> = JSON.parse(data) as Record<string, number>;
      const w = Number(obj.w ?? entry.w);
      const h = Number(obj.h ?? entry.h);
      if (w > 0 && h > 0) {
        entry.w = w;
        entry.h = h;
      }
      entry.subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                          Math.max(1, Math.round(entry.h * scale)))
        .catch(() => { /* ignore */ });
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d onResize bad data: %{public}s',
        modalId, JSON.stringify(e));
    }
  }

  // -- modal=0 / 窗口销毁: 关闭子窗口 --
  hide(modalId: number): void {
    this.pendingShows.delete(modalId);  // 暂存的 show 一并取消 (hide 晚于 show)
    if (this.creating.has(modalId)) {
      this.pendingDestroy.add(modalId);
      return;
    }
    const entry = this.modals.get(modalId);
    if (!entry) return;
    hilog.info(DOMAIN, TAG, '[MODAL] hide #%{public}d', modalId);
    this.destroyEntry(entry);
  }

  // owner 窗口销毁时级联关闭其全部 modal 子窗口
  destroyForOwner(ownerId: number): void {
    for (const entry of this.modals.values()) {
      if (entry.ownerId === ownerId) {
        hilog.info(DOMAIN, TAG, '[MODAL] owner #%{public}d destroyed → close modal #%{public}d',
          ownerId, entry.modalId);
        this.destroyEntry(entry);
      }
    }
  }

  private destroyEntry(entry: ModalEntry): void {
    this.modals.delete(entry.modalId);
    this.creating.delete(entry.modalId);
    this.pendingDestroy.delete(entry.modalId);
    const subWin = entry.subWin;
    entry.subWin = null;
    if (subWin) {
      // 有 loadContent 的窗口必须异步销毁 (同步 destroy 会崩溃)
      subWin.destroyWindow().then(() => {
        hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d destroyed', entry.modalId);
      }).catch((e: Error) => {
        hilog.warn(DOMAIN, TAG, '[MODAL] #%{public}d destroyWindow: %{public}s',
          entry.modalId, JSON.stringify(e));
      });
    }
  }
}

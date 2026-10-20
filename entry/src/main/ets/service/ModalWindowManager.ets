import { window, display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { FusionWindowManager } from './FusionWindowManager';

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
  // 宿主 stage (创建本子窗口的 WindowStage)。嵌套 modal (owner 是另一
  // modal) 时继承 owner 的, 供以本 modal 为 owner 的下一层补建使用;
  // OHOS subWindow 不能嵌套 (宿主恒为主窗口), 但同 stage 多 subWindow
  // 并存合法, 后创建默认在上层
  stage: window.WindowStage | null = null;
  w: number = 0;
  h: number = 0;
}

// ─── modal 装饰制度 (路线×承载形态差异的唯一收口, 流程体零路线分支) ───
// 谁画模态对话框的标题栏, 两条路线答案不同, 全部差异收在本接口两实现:
//  - NoSystemDecor: wayland 全形态 (wine 自绘 CSD —— incumbent 行为逐字节
//    不变是隔离红线) + x11 Pad subWindow 承载 (页面自绘顶条, #167 续)
//  - SystemDecor: x11 + ability 承载 (PC/2in1 独立窗) —— 系统标题栏,
//    decor 逐项配置同 WWA 主窗; 系统 ✕ 经 systemCloseModal 回发
//    WM_DELETE, 只销毁不回发会让 wine 侧 owner 永久禁用
interface ModalDecor {
  readonly useSystemDecor: boolean;
  optionsTitle(modalId: number): string;
  // resize 的承载装饰高度补偿 (px): Pad=0 (顶条补偿走 stripPxOf),
  // SystemDecor=getWindowDecorHeight()×scale
  heightCompPx(subWin: window.Window | null, scale: number): number;
  // loadContent 后的窗口配置 (decor API + close 契约监听)。isCurrent =
  // 本 entry 仍在册判据 (身份语义归 manager), 供监听回发前防同 id 复用/
  // 自身销毁竞态
  configureWindow(subWin: window.Window, modalId: number,
                  isCurrent: () => boolean): void;
  titleChanged(subWin: window.Window | null, title: string): void;
  releaseWindow(subWin: window.Window): void;
}

class NoSystemDecor implements ModalDecor {
  readonly useSystemDecor: boolean = false;
  optionsTitle(_modalId: number): string { return ''; }
  heightCompPx(_subWin: window.Window | null, _scale: number): number { return 0; }
  configureWindow(_subWin: window.Window, _modalId: number,
                  _isCurrent: () => boolean): void { }
  titleChanged(_subWin: window.Window | null, _title: string): void { }
  releaseWindow(_subWin: window.Window): void { }
}

class SystemDecor implements ModalDecor {
  readonly useSystemDecor: boolean = true;
  // setWindowDecorHeight 注入值。⚠ 单位是 vp 不是 px (SDK d.ts: 合法区间
  // [37,112], 单位 vp) —— 首版按 40px 补偿, 实际 caption 40vp=80px
  // (scale 2.0), 差 40px 把对话框底部按钮裁了一半 (2026-10-17 MOR-M1
  // 实测 + d.ts 判据)。补偿以 getWindowDecorHeight() 读回为准, 本值只是
  // 注入值与读回失败时的兜底
  private static readonly DECOR_HEIGHT_VP: number = 40;

  optionsTitle(modalId: number): string {
    return ModalWindowManager.getInstance().titleOf(modalId);
  }

  heightCompPx(subWin: window.Window | null, scale: number): number {
    if (!subWin) {
      return 0;
    }
    try {
      const vp = subWin.getWindowDecorHeight();  // [37,112] vp, d.ts 判据
      if (vp >= 37 && vp <= 112) {
        return Math.round(vp * scale);
      }
    } catch (_) { /* 读回失败走兜底 */ }
    return Math.round(SystemDecor.DECOR_HEIGHT_VP * scale);
  }

  configureWindow(subWin: window.Window, modalId: number,
                  isCurrent: () => boolean): void {
    // 逐项独立 try/catch: 装饰 API 设备能力面窄 (801/1300004 各调用不同),
    // 合并 catch 吞精确错误面 (WWA 同款纪律)
    const dstep = (name: string, fn: () => void): void => {
      try {
        fn();
        hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d decor step %{public}s ok', modalId, name);
      } catch (e) {
        hilog.warn(DOMAIN, TAG, '[MODAL] #%{public}d decor step %{public}s FAILED: %{public}s',
          modalId, name, JSON.stringify(e));
      }
    };
    dstep('decorVisible(true)', () => { subWin.setWindowDecorVisible(true); });
    dstep('titleButtons(false,false,true)',
      () => { subWin.setWindowTitleButtonVisible(false, false, true); });
    dstep('titleMove(true)', () => { subWin.setWindowTitleMoveEnabled(true); });
    dstep('decorHeight', () => { subWin.setWindowDecorHeight(SystemDecor.DECOR_HEIGHT_VP); });
    const t = ModalWindowManager.getInstance().titleOf(modalId);
    if (t) {
      dstep('title', () => { subWin.setWindowTitle(t); });
    }
    // 系统标题栏 close 契约 (与 WWA cleanup 同款): WINDOW_DESTROYED →
    // 先回发 WM_DELETE (wine 结束模态循环并恢复 owner) 再本地销毁
    subWin.on('windowEvent', (eventType: number) => {
      if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
        ModalWindowManager.getInstance().systemCloseModal(modalId, isCurrent);
      }
    });
  }

  titleChanged(subWin: window.Window | null, title: string): void {
    if (subWin) {
      try { subWin.setWindowTitle(title); } catch (_) { /* ignore */ }
    }
  }

  releaseWindow(subWin: window.Window): void {
    try { subWin.off('windowEvent'); } catch (_) { /* 未注册则忽略 */ }
  }
}

const NO_SYSTEM_DECOR: ModalDecor = new NoSystemDecor();
const SYSTEM_DECOR: ModalDecor = new SystemDecor();

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
 *
 * 装饰制度差异 (谁画模态对话框的标题栏) 收口在 ModalDecor 接口两实现
 * (本文件, 承载流程体零路线分支):
 *  - NoSystemDecor: wayland 全形态 (wine 自绘 CSD —— incumbent 行为逐字节
 *    不变是隔离红线) + x11 Pad subWindow 承载 (页面自绘顶条, #167 续)
 *  - SystemDecor: x11 + ability 承载 (PC/2in1) —— 系统标题栏, decor 逐项
 *    配置同 WWA 主窗 (2026-10-17 MOR-M1 首装实测: modal subWindow 沿用
 *    false 裸奔无标题栏, 而 WWA 主窗 decor 五步全 ok); 标题按钮只开
 *    close (win32 模态语义无 min/max); 系统 ✕ 回发 WM_DELETE (WWA cleanup
 *    同款契约, 只销毁不回发会让 owner 永久禁用)
 * 判据单点 = WineWindowManager modal 分支 (isX11 ∧ 非 subWindow 承载),
 * 经 setUseSystemDecor 注入; 路线冷启动定死 → 制度会话内稳定, 不逐窗存储。
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
  // modal 窗口标题 (#167 续: x11 modal 承载的自绘顶条标题)。独立于
  // ModalEntry 存表: title 事件可能在 creating 阶段 (modals.set 之前) 到达;
  // WineWindowManager 的 modal 分支经 onTitle 喂入, WineWindow 页经
  // WineWindowManager.getWindowTitle → titleOf 读取
  private titles: Map<number, string> = new Map();
  // 装饰制度 (路线×承载形态差异的唯一收口, 见 ModalDecor): 会话内稳定,
  // WineWindowManager modal 分支每次事件重申注入
  private decor: ModalDecor = NO_SYSTEM_DECOR;

  static getInstance(): ModalWindowManager {
    if (!ModalWindowManager.instance) {
      ModalWindowManager.instance = new ModalWindowManager();
    }
    return ModalWindowManager.instance;
  }

  // 装饰制度注入 (WineWindowManager modal 分支调用; 判据在其侧)
  setUseSystemDecor(v: boolean): void {
    this.decor = v ? SYSTEM_DECOR : NO_SYSTEM_DECOR;
  }

  // 模态阻断查询 (任务 #167): ownerToplevelId 名下有活 modal (已建表 +
  // pending 均算 —— wine 上报 modal=1 时 owner 即已禁用, 不等子窗建完)。
  // 消费点 = WineWindowManager.raiseWindowGroup (阻断时不置前); 与 native
  // FocusClient/restack 门同一事件源。creating 态未覆盖 (评审 M2): 异步
  // 建窗飞行期毫秒级窗口由 native 门兜底, 此处漏判自愈。
  isModalBlocked(ownerToplevelId: number): boolean {
    for (const m of this.modals.values()) {
      if (m.ownerId === ownerToplevelId) return true;
    }
    for (const p of this.pendingShows.values()) {
      if (p.ownerId === ownerToplevelId) return true;
    }
    return false;
  }

  // modal 承载判据 (FusionWindowManager.stripPxOf 的 provider 端, #167 续):
  // 已建表 / 创建中 / owner 未就绪暂存, 三态都算 —— WineWindow 页
  // aboutToAppear 在 loadContent 内执行, 此时 entry 尚未入 modals 表,
  // 只有 creating 覆盖得住。
  isModalToplevel(tl: number): boolean {
    return this.modals.has(tl) || this.creating.has(tl) ||
      this.pendingShows.has(tl);
  }

  // 顶条标题读取口 (WineWindowManager.getWindowTitle 的 modal 兜底)。
  titleOf(tl: number): string {
    return this.titles.get(tl) ?? '';
  }

  // title 事件入口 (WineWindowManager modal 分支; native 在 modal 事件之后
  // 补发 title)。刷新由调用方触发 WineWindowManager 的 listener 通知。
  onTitle(id: number, data: string): void {
    try {
      const obj: Record<string, string> = JSON.parse(data) as Record<string, string>;
      const t = String(obj.title ?? '');
      if (t) {
        this.titles.set(id, t);
        // 标题同步到系统标题栏 (title 事件常晚于 show 到达; 无系统装饰的
        // 制度此调用为 no-op)
        this.decor.titleChanged(this.modals.get(id)?.subWin ?? null, t);
      }
    } catch (_) { /* 坏数据忽略, 顶条标题留空 */ }
  }

  // -- modal=1: 创建模态子窗口 (owner 的 stage/win/scale 由调用方传入) --
  // d 为 JSON.parse 的原始 Record (字符串惯例, 键值数值字符串/数字混存),
  // 内部一律 Number() 化
  show(modalId: number, d: Record<string, string | number>,
       stage: window.WindowStage | null,
       parentWin: window.Window | null, scale: number): void {
    // 嵌套模态兜底: modal 窗口不建 WineWindowManager entry, 当新对话框的
    // owner 是另一个 modal (而非真实主窗口) 时, 宿主 stage + 物理父窗口
    // 从该 modal 继承 — OHOS subWindow 不能嵌套, 但同 stage 多 subWindow
    // 并存且后创建默认在上层, 嵌套对话框"看得到"不受此限
    let hostStage = stage;
    let hostWin = parentWin;
    if (!hostStage || !hostWin) {
      const host = this.modals.get(Number(d.owner ?? 0));
      if (host?.stage && host?.subWin) {
        hostStage = host.stage;
        hostWin = host.subWin;
      }
    }
    // 已存在 (Wine 每次 WindowPosChanged 重发 modal=1, 带新 dx/dy) →
    // 只做位置/尺寸跟随, 不重建子窗口
    const existing = this.modals.get(modalId);
    if (existing) {
      hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d re-show → reposition', modalId);
      if (hostWin) {
        this.reposition(existing, d, hostWin, scale);
      }
      return;
    }
    // 创建进行中: 重复 show 忽略 (hide 走 pendingDestroy 兜底)
    if (this.creating.has(modalId)) {
      return;
    }

    if (!hostStage || !hostWin) {
      // owner (Ability 或上一级 modal) 尚未就绪: 暂存, 由"就绪方"补建 —
      // 真实主窗口经 WineWindowManager.flushPendingEvents 回填,
      // 嵌套 modal 经 show 成功后 flushForOwner 级联补建。drop 会让
      // ModalToplevelIds 吞掉后续一切事件且 owner 已被 wine 禁用,
      // 应用表现为"点不动且无对话框"
      hilog.warn(DOMAIN, TAG, '[MODAL] show #%{public}d: owner #%{public}d stage/win not ready, queue',
        modalId, d.owner ?? 0);
      const pending: PendingShow = { data: d, ownerId: Number(d.owner ?? 0) };
      this.pendingShows.set(modalId, pending);
      return;
    }
    const entry = new ModalEntry();
    entry.modalId = modalId;
    entry.ownerId = Number(d.owner ?? 0);
    entry.stage = hostStage;
    entry.w = Math.max(1, Number(d.w ?? 0));
    entry.h = Math.max(1, Number(d.h ?? 0));
    this.creating.add(modalId);

    // 与 WineWindow 同模式: aboutToAppear 读 pending toplevelId (与 PC 同款 FIFO)
    testNapi.setPendingToplevel(modalId);

    hilog.info(DOMAIN, TAG,
      '[MODAL] show #%{public}d owner=#%{public}d wine=%{public}dx%{public}d rel=(%{public}d,%{public}d) scale=%{public}s',
      modalId, entry.ownerId, entry.w, entry.h, d.dx ?? 0, d.dy ?? 0, scale.toFixed(2));

    hostStage.createSubWindowWithOptions(`wine_modal_${modalId}`, {
      // 装饰制度差异 (标题/开关) 经 ModalDecor 收口, 流程体零路线分支
      title: this.decor.optionsTitle(modalId),
      decorEnabled: this.decor.useSystemDecor,
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
      // 装饰配置 + 系统 close 契约监听 —— ModalDecor 策略面; isCurrent 供
      // 监听回发前防同 id 复用/自身销毁竞态 (身份语义归 manager)
      this.decor.configureWindow(subWin, modalId,
        () => this.modals.get(modalId) === entry);

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
        // 尺寸: wine 逻辑 × scale; 高度 = guest + 承载装饰补偿。两个补偿
        // 位: ① stripPxOf = Pad 自绘顶条 (页面 Row 占补偿区, #167 续);
        // ② heightCompPx = PC 系统标题栏 (系统 resize 按整窗计, 装饰区在
        // 内容区外占位)。位置见 computePosition
        const stripPx = FusionWindowManager.getInstance().stripPxOf(modalId);
        const decorPx = this.decor.heightCompPx(subWin, scale);
        await subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                            Math.max(1, Math.round(entry.h * scale)) + stripPx + decorPx);
        const pr = hostWin!.getWindowProperties().windowRect;
        const pos = this.computePosition(entry.ownerId, pr, d, entry.w, entry.h, scale,
                                         stripPx, decorPx);
        await subWin.moveWindowToGlobal(pos[0], pos[1]);
        await subWin.showWindow();
        hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d shown at screen(%{public}d,%{public}d)',
          modalId, pos[0], pos[1]);
        // 嵌套模态: 自身已可见 → 以自身为 owner 的暂存 show 请求可补建
        // (宿主 stage + 本 subWin 即其物理父/定位基准)
        this.flushForOwner(entry.modalId, entry.stage, entry.subWin, scale);
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d show failed: %{public}s',
          modalId, JSON.stringify(e));
        this.destroyEntry(entry);
      }
    }).catch((e: Error) => {
      this.creating.delete(modalId);
      this.pendingDestroy.delete(modalId);  // 失败路径同样清理
      testNapi.cancelPendingToplevel(modalId);  // 创建失败 → 清 pending 残坑
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

  // 计算 modal 的屏幕位置 [x, y] (物理像素)。
  //
  // PC 模式: wine 的窗口位置即屏幕位置, 定位 = owner 屏幕位置 + 相对差 × scale。
  // Fusion (Pad 多窗口) 模式: 窗口屏幕位置由 FusionWindowManager 层叠摆放
  // (cascadePoint), 与 wine 的虚拟桌面布局是两套坐标 —— C++ 传来的 dx/dy 是
  // wine 自己排版的结果 (含它为避开虚拟桌面边界做的挪动, 实测可达数千逻辑
  // 像素), 直接叠加会把对话框推出屏幕, 而 isModal 的输入阻塞照常生效 ——
  // 表现为"对话框一闪就没了、所有窗口点不动"。该模式下改为相对 owner 居中。
  // 最后统一钳制进屏幕, 兜住任何模式下的越界。
  private computePosition(ownerId: number, pr: window.Rect,
                          d: Record<string, string | number>,
                          w: number, h: number, scale: number,
                          stripPx: number, decorPx: number): number[] {
    const modalW = Math.max(1, Math.round(w * scale));
    const modalH = Math.max(1, Math.round(h * scale)) + stripPx + decorPx;
    let x: number;
    let y: number;
    // owner 内容原点在 frame 原点下方一个 caption (SystemDecor 制度下
    // decorPx 即此值, Pad 制度 decorPx=0 原样), wine 的 dx/dy 与居中基准
    // 都是内容坐标 —— 不补则 PC 对话框整体上移一个标题栏 (#171)
    if (FusionWindowManager.getInstance().has(ownerId)) {
      x = pr.left + Math.round((pr.width - modalW) / 2);
      y = pr.top + decorPx + Math.round((pr.height - decorPx - modalH) / 2);
    } else {
      x = pr.left + Math.round(Number(d.dx ?? 0) * scale);
      y = pr.top + decorPx + Math.round(Number(d.dy ?? 0) * scale);
    }
    try {
      const scr = display.getDefaultDisplaySync();
      x = Math.min(Math.max(x, 0), Math.max(0, scr.width - modalW));
      y = Math.min(Math.max(y, 0), Math.max(0, scr.height - modalH));
    } catch (_) { /* 拿不到屏幕尺寸则不钳制, 保持原值 */ }
    return [x, y];
  }

  // -- 已存在 modal 的尺寸/位置跟随 (Wine 重发 modal=1 带新 dx/dy) --
  // 末尾补 showWindow: 首次 show 与 re-show 之间窗口可能被隐藏 (创建期时序/
  // 父窗口变化), 只 resize+move 不会让它重新可见 —— 配合位置钳制一起兜住
  // "对话框不可见却仍阻塞输入"
  private reposition(entry: ModalEntry, d: Record<string, string | number>,
                     parentWin: window.Window, scale: number): void {
    if (!entry.subWin) return;
    // 占位事件 (modal=1 首发时 w/h 为 0) 不覆盖已知尺寸, 否则窗口被缩成 1×1
    // (onResize 同款守卫)
    const nw = Number(d.w ?? 0);
    const nh = Number(d.h ?? 0);
    const w = nw > 0 ? nw : entry.w;
    const h = nh > 0 ? nh : entry.h;
    entry.w = w;
    entry.h = h;
    const subWin = entry.subWin;
    const stripPx = FusionWindowManager.getInstance().stripPxOf(entry.modalId);
    const decorPx = this.decor.heightCompPx(subWin, scale);
    subWin.resize(Math.max(1, Math.round(w * scale)),
                  Math.max(1, Math.round(h * scale)) + stripPx + decorPx).then(() => {
      const pr = parentWin.getWindowProperties().windowRect;
      const pos = this.computePosition(entry.ownerId, pr, d, w, h, scale, stripPx, decorPx);
      return subWin.moveWindowToGlobal(pos[0], pos[1]);
    }).then(() => subWin.showWindow()).catch(() => { /* ignore */ });
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
                          Math.max(1, Math.round(entry.h * scale)) +
                            FusionWindowManager.getInstance().stripPxOf(modalId) +
                            this.decor.heightCompPx(entry.subWin, scale))
        .catch(() => { /* ignore */ });
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MODAL] #%{public}d onResize bad data: %{public}s',
        modalId, JSON.stringify(e));
    }
  }

  // SystemDecor close 契约落点 (ModalDecor 策略回调): 系统标题栏 ✕ →
  // 先 sendToplevelClose 给 wine 发 WM_DELETE (结束模态循环并恢复 owner),
  // 再本地销毁承载。只销毁不回发会让 wine 侧 owner 永久禁用 —— 表现为
  // 所有窗口点不动。isCurrent 防同 id 复用/自身销毁竞态误发
  systemCloseModal(modalId: number, isCurrent: () => boolean): void {
    if (!isCurrent()) {
      return;
    }
    hilog.info(DOMAIN, TAG, '[MODAL] #%{public}d system close → sendToplevelClose + destroy', modalId);
    try {
      testNapi.sendToplevelClose(modalId);
    } catch (_) { /* 队列不可用则跳过回发, 本地仍要清理 */ }
    const entry = this.modals.get(modalId);
    if (entry) {
      this.destroyEntry(entry);
    }
  }

  // -- modal 自身窗口/宿主 stage 读取 (#168: 对话框自身 popup 的定位锚) --
  winOf(modalId: number): window.Window | null {
    return this.modals.get(modalId)?.subWin ?? null;
  }

  stageOf(modalId: number): window.WindowStage | null {
    return this.modals.get(modalId)?.stage ?? null;
  }

  // -- modal=0 / 窗口销毁: 关闭子窗口 --
  hide(modalId: number): void {
    this.pendingShows.delete(modalId);  // 暂存的 show 一并取消 (hide 晚于 show)
    this.titles.delete(modalId);
    if (this.creating.has(modalId)) {
      this.pendingDestroy.add(modalId);
      // destroy-while-creating: 页面可能未执行 aboutToAppear → 清 pending 残坑
      // (不删会使后续页面出队拿到死 id, 渲染器挂错 toplevel 黑屏)
      testNapi.cancelPendingToplevel(modalId);
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
    // 级联: 以该 modal 为 owner 的嵌套 modal 一并销毁 (Win32 owned 链);
    // 同时清掉其暂存 show 请求 (父已亡, 永不补建, 防泄漏)
    this.modals.forEach((child) => {
      if (child.ownerId === entry.modalId) {
        this.destroyEntry(child);
      }
    });
    this.pendingShows.forEach((p, modalId) => {
      if (p.ownerId === entry.modalId) {
        this.pendingShows.delete(modalId);
      }
    });
    this.modals.delete(entry.modalId);
    this.creating.delete(entry.modalId);
    this.pendingDestroy.delete(entry.modalId);
    this.titles.delete(entry.modalId);
    const subWin = entry.subWin;
    entry.subWin = null;
    if (subWin) {
      this.decor.releaseWindow(subWin);
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

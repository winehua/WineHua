import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'ArgbWM';

class ArgbEntry {
  toplevelId: number = 0;
  subWin: window.Window | null = null;
  // Wine 虚拟屏幕坐标与帧尺寸 (逻辑像素)
  x: number = 0;
  y: number = 0;
  w: number = 0;
  h: number = 0;
}

/**
 * ARGB 异型窗口 (layered/shaped) 管理: 每个 ARGB toplevel 一个独立子窗口。
 *
 * 背景: 2in1 主窗口无法透视 (实测: XComponent surface 无 alpha 通道,
 * 窗口背景色 alpha 被系统钳制, setWindowMask 仅限子窗/悬浮窗)。
 * 因此 ARGB 窗口不走 WineWindowAbility, 而是由子窗口承载:
 *   内容: XComponent 渲染 (XComponent surface 不透明无妨, mask 负责剪影)
 *   剪影: C++ 从 alpha 通道生成 0/1 掩码, 本类 setWindowMask 应用
 *   位置: Wine 虚拟屏幕坐标 (桌面小部件由 Wine 决定位置), moveWindowToGlobal
 * 已知限制 (v1):
 *   - mask 为二值, 半透明过渡被阈值化 (硬边, 抗锯齿边缘略糙)
 *   - 子窗口非自由窗口: 无任务栏条目, 不能系统级拖动/最小化/贴靠,
 *     层级跟随宿主 (主) 窗口; 对桌面小部件类异型窗口恰好合适
 *   - 页面复用 pages/WinePopup, 未接键盘输入
 */
export class ArgbWindowManager {
  private static instance: ArgbWindowManager | null = null;
  private entries: Map<number, ArgbEntry> = new Map();
  private creating: Set<number> = new Set();
  // 异步创建期间收到 destroy 时延迟销毁 (与 PopupWindowManager 同款竞态)
  private pendingDestroy: Set<number> = new Set();

  static getInstance(): ArgbWindowManager {
    if (!ArgbWindowManager.instance) {
      ArgbWindowManager.instance = new ArgbWindowManager();
    }
    return ArgbWindowManager.instance;
  }

  has(id: number): boolean {
    return this.entries.has(id) || this.creating.has(id);
  }

  getWindow(id: number): window.Window | null {
    return this.entries.get(id)?.subWin ?? null;
  }

  // -- argb_created: 创建子窗口 --
  onCreated(id: number, data: string, stage: window.WindowStage, scale: number): void {
    if (this.has(id)) {
      hilog.warn(DOMAIN, TAG, '[ARGB] created #%{public}d: already exists, skip', id);
      return;
    }
    let x = 0, y = 0, w = 0, h = 0;
    try {
      const obj = JSON.parse(data) as Record<string, number>;
      x = obj.x ?? 0; y = obj.y ?? 0; w = obj.w ?? 0; h = obj.h ?? 0;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[ARGB] created #%{public}d bad json: %{public}s', id, data);
      return;
    }
    hilog.info(DOMAIN, TAG, '[ARGB] created #%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d)',
      id, x, y, w, h);

    const entry = new ArgbEntry();
    entry.toplevelId = id;
    entry.x = x; entry.y = y; entry.w = w; entry.h = h;
    this.creating.add(id);

    // 复用 pages/WinePopup 渲染管线 (createRenderer + sendPointerEvent)
    // 与 PC 同款 FIFO: aboutToAppear 同步读 pending toplevelId
    testNapi.setPendingToplevel(id);

    stage.createSubWindowWithOptions(`wine_argb_${id}`, {
      title: '',
      decorEnabled: false,
      isModal: false
    }).then(async (subWin) => {
      try {
        await subWin.loadContent('pages/WinePopup');
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[ARGB] #%{public}d loadContent FAILED: %{public}s', id, JSON.stringify(e));
        this.creating.delete(id);
        try { await subWin.destroyWindow(); } catch (_) { /* ignore */ }
        return;
      }
      try { subWin.setWindowCornerRadius(0); } catch (_) { /* ignore */ }
      try { await subWin.setWindowShadowEnabled(false); } catch (_) { /* ignore */ }
      try { subWin.setWindowBackgroundColor('#00000000'); } catch (_) { /* ignore */ }

      entry.subWin = subWin;
      this.creating.delete(id);
      this.entries.set(id, entry);

      // 创建期间 toplevel 已被销毁 → 立即销毁, 不继续 show
      if (this.pendingDestroy.has(id)) {
        this.pendingDestroy.delete(id);
        hilog.info(DOMAIN, TAG, '[ARGB] #%{public}d destroy requested while creating', id);
        this.destroy(id);
        return;
      }

      subWin.on('windowEvent', (eventType: number) => {
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          hilog.info(DOMAIN, TAG, '[ARGB] #%{public}d WINDOW_DESTROYED', id);
          this.entries.delete(id);
        }
      });

      try {
        await subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                            Math.max(1, Math.round(entry.h * scale)));
        await this.positionEntry(entry, scale);
        await subWin.showWindow();
        // 首帧掩码可能已经就绪, 尝试应用一次
        this.applyMask(entry, scale);
        hilog.info(DOMAIN, TAG, '[ARGB] #%{public}d shown', id);
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[ARGB] #%{public}d show failed: %{public}s', id, JSON.stringify(e));
        this.destroy(id);
      }
    }).catch((e: Error) => {
      this.creating.delete(id);
      testNapi.cancelPendingToplevel(id);  // 创建失败 → 清 pending 残坑
      hilog.error(DOMAIN, TAG, '[ARGB] #%{public}d createSubWindow FAILED: %{public}s', id, JSON.stringify(e));
    });
  }

  // -- argb_move --
  onMove(id: number, data: string, scale: number): void {
    const entry = this.entries.get(id);
    if (!entry) return;
    try {
      const obj = JSON.parse(data) as Record<string, number>;
      entry.x = obj.x ?? entry.x;
      entry.y = obj.y ?? entry.y;
    } catch (_) { return; }
    this.positionEntry(entry, scale).catch(() => { /* ignore */ });
  }

  // -- resize (复用现有 resize 事件) --
  onResize(id: number, data: string, scale: number): void {
    const entry = this.entries.get(id);
    if (!entry || !entry.subWin) return;
    try {
      const obj = JSON.parse(data) as Record<string, number>;
      entry.w = obj.w ?? entry.w;
      entry.h = obj.h ?? entry.h;
    } catch (_) { return; }
    entry.subWin.resize(Math.max(1, Math.round(entry.w * scale)),
                        Math.max(1, Math.round(entry.h * scale))).then(() => {
      // 掩码尺寸须等于窗口尺寸, resize 后重设一次
      this.applyMask(entry, scale);
    }).catch(() => { /* ignore */ });
  }

  // -- mask_dirty: 拉取并应用 --
  onMaskDirty(id: number, scale: number): void {
    const entry = this.entries.get(id);
    if (!entry) return;
    this.applyMask(entry, scale);
  }

  destroy(id: number): void {
    // 还在异步创建中: 标记后由创建完成处销毁 (防止给死窗口留下子窗口)
    if (this.creating.has(id)) {
      this.pendingDestroy.add(id);
      this.creating.delete(id);
      // disconnect-while-creating: 页面可能未执行 aboutToAppear → 清 pending 残坑
      // (不删会使后续页面出队拿到死 id, 渲染器挂错 toplevel 黑屏)
      testNapi.cancelPendingToplevel(id);
      return;
    }
    const entry = this.entries.get(id);
    if (!entry) return;
    hilog.info(DOMAIN, TAG, '[ARGB] destroy #%{public}d', id);
    this.entries.delete(id);
    if (entry.subWin) {
      entry.subWin.destroyWindow().catch(() => { /* ignore */ });
      entry.subWin = null;
    }
  }

  private async positionEntry(entry: ArgbEntry, scale: number): Promise<void> {
    if (!entry.subWin) return;
    // Wine 虚拟屏幕坐标 → 全局物理像素
    // (假设虚拟屏幕原点 = 本机显示原点; PC 模式不考虑多显示器偏移)
    await entry.subWin.moveWindowToGlobal(Math.round(entry.x * scale), Math.round(entry.y * scale));
  }

  // 拉取 C++ 掩码 (帧分辨率/Wine 逻辑像素) 并按 effectiveScale 最近邻放大
  // 到窗口物理尺寸 (setWindowMask 要求掩码宽高 == 窗口宽高) 后应用
  private applyMask(entry: ArgbEntry, scale: number): void {
    if (!entry.subWin) return;
    interface WindowMaskResult {
      w: number;
      h: number;
      buffer: ArrayBuffer;
    }
    const m = testNapi.takeWindowMask(entry.toplevelId) as WindowMaskResult | null;
    if (!m || m.w <= 0 || m.h <= 0) return;
    const pw = Math.max(1, Math.round(m.w * scale));
    const ph = Math.max(1, Math.round(m.h * scale));
    const bytes = new Uint8Array(m.buffer);
    const mask = Array<Array<number>>(ph).fill([]).map((_, row) => {
      const srcRow = Math.min(m.h - 1, Math.floor(row / scale));
      const line = Array<number>(pw);
      for (let i = 0; i < pw; i++) {
        const srcCol = Math.min(m.w - 1, Math.floor(i / scale));
        line[i] = bytes[srcRow * m.w + srcCol];
      }
      return line;
    });
    entry.subWin!.setWindowMask(mask).then(() => {
      hilog.info(DOMAIN, TAG, '[ARGB] #%{public}d setWindowMask OK %{public}dx%{public}d (frame %{public}dx%{public}d)',
        entry.toplevelId, pw, ph, m.w, m.h);
    }).catch((e: Error) => {
      hilog.warn(DOMAIN, TAG, '[ARGB] #%{public}d setWindowMask failed: %{public}s',
        entry.toplevelId, JSON.stringify(e));
    });
  }
}

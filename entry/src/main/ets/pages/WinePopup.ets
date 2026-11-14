import { hilog } from '@kit.PerformanceAnalysisKit';
import hgsNapi from 'libhgs.so';
import { HgsSurface } from 'hgs';

const DOMAIN = 0x0000;

// PC 模式 popup (菜单/tooltip/下拉框) 子窗口页面 (PopupWindowManager 经
// createSubWindowWithOptions 承载 — 子窗口才能越出父窗口边界)。
// 与 WineWindow 同由 WineSurface/WinePointerMachine 公共实现, 差异全在
// 配置: 不反写尺寸 (popup 无 xdg_toplevel, 不能发 configure)、不处理键盘
// (焦点留在父窗口)、不处理 startMoving、rawDelta 不缩放。
@Entry
@Component
struct WinePopup {
  // popupId 由 PopupWindowManager 在 loadContent 前 setPendingToplevel 入队,
  // aboutToAppear 出队取得 (与 PC 多窗口同款, PC 实测 OK)
  @State popupId: number = 0;

  aboutToAppear(): void {
    this.popupId = hgsNapi.getCurrentToplevelId() as number;
    hilog.info(DOMAIN, 'WinePopup', 'aboutToAppear popup=%{public}d', this.popupId);
  }

  build() {
    Column() {
      HgsSurface({
        xcId: 'wine_popup_xc',
        toplevelId: this.popupId,
        onSurfaceCreatedCallback: (sid: string, id: number) => {
          try { hgsNapi.createRenderer(id, BigInt(sid)); } catch (_) {}
        },
        onSurfaceChangedCallback: (_sid: string, id: number, w: number, h: number) => {
          // popup 无 xdg_toplevel: 只 resizeRenderer, 不反写 Wine 尺寸
          try { hgsNapi.resizeRenderer(id, w, h); } catch (_) {}
        },
        onSurfaceDestroyedCallback: (id: number) => {
          try { hgsNapi.destroyRenderer(id); } catch (_) {}
        }
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000')
  }
}

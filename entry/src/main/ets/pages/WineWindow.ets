import { hilog } from '@kit.PerformanceAnalysisKit';
import hgsNapi from 'libhgs.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { HgsSurface } from 'hgs';

const DOMAIN = 0x0000;

// PC 多窗口模式的单窗口页面 (WineWindowAbility, multiton)。
// surface 生命周期与输入状态机由 WineSurface/WinePointerMachine 公共实现
// (与 WinePopup/DesktopWindow/DesktopLayer 共用); 本页只保留差异配置:
// pending-id 消费、raiseWindowGroup、startMoving 接管、拖拽 RESIZING、回车补发。
@Entry
@Component
struct WineWindow {
  @State toplevelId: number = 0;
  @State surfacePxW: number = 1;
  @State surfacePxH: number = 1;

  aboutToAppear(): void {
    // toplevelId 由承载方 (WineWindowAbility/FusionWindowManager/ModalWindowManager)
    // 在 loadContent 前 setPendingToplevel 入队, aboutToAppear 出队取得
    // (aboutToAppear 是 loadContent 的同步调用, 在下一个 Ability 覆盖前读取)
    this.toplevelId = hgsNapi.getCurrentToplevelId() as number;
    hilog.info(DOMAIN, 'WineWindow', 'aboutToAppear tl=%{public}d', this.toplevelId);
  }

  build() {
    Column() {
      HgsSurface({
        xcId: 'wine_sub_xc',
        toplevelId: this.toplevelId,
        keyboardTarget: this.toplevelId,
        onSurfaceCreatedCallback: (sid: string, id: number) => {
          hilog.info(DOMAIN, 'WineWindow', '[Ctrl] onSurfaceCreated tl=%{public}d surfaceId=%{public}s', id, sid);
          try { hgsNapi.createRenderer(id, BigInt(sid)); } catch (_) {}
        },
        onSurfaceChangedCallback: (sid: string, id: number, w: number, h: number) => {
          try {
            hgsNapi.resizeRenderer(id, w, h);
            const s = WineWindowManager.getInstance().getEffectiveScale();
            const resizing = WineWindowManager.getInstance().isResizeDragging(id);
            hgsNapi.notifyToplevelResize(id, Math.round(w / s), Math.round(h / s), resizing);
          } catch (_) {}
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

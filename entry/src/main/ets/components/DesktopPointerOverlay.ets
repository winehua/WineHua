import { DesktopPointerService } from '../service/DesktopPointerService';

@Component
export struct DesktopPointerOverlay {
  @State private x: number = 0;
  @State private y: number = 0;
  private pointer = DesktopPointerService.getInstance();
  private widthVp: number = 0;
  private heightVp: number = 0;
  private scheduled: number = -1;
  private visible: boolean = true;

  aboutToAppear(): void { this.pointer.subscribe(this.onMove); this.sync(); }
  aboutToDisappear(): void {
    this.pointer.unsubscribe(this.onMove);
    if (this.scheduled >= 0) clearTimeout(this.scheduled);
    this.scheduled = -1;
  }
  private sync(): void {
    if (!this.pointer.positioned) return;
    this.x = Math.max(0, Math.min(this.widthVp - 2, px2vp(this.pointer.x)));
    this.y = Math.max(0, Math.min(this.heightVp - 2, px2vp(this.pointer.y)));
  }
  private onMove = (): void => {
    if (!this.visible || this.scheduled >= 0) return;
    // No idle polling and at most one UI update per display frame.
    this.scheduled = setTimeout(() => { this.scheduled = -1; this.sync(); }, 16);
  };
  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Path().commands('M 1 1 L 1 23 L 7 17 L 12 27 L 16 25 L 11 15 L 21 15 Z')
        .fill('#FFFFFF').stroke('#101010').strokeWidth(1.5).width(24).height(30)
        .position({ x: this.x, y: this.y }).id('desktop-mouse-cursor')
    }
    .width('100%').height('100%').clip(true).hitTestBehavior(HitTestMode.None)
    .onAreaChange((_old: Area, area: Area) => {
      this.widthVp = Number(area.width); this.heightVp = Number(area.height);
      if (this.widthVp > 0 && this.heightVp > 0 && !this.pointer.positioned)
        this.pointer.move(vp2px(this.widthVp / 2), vp2px(this.heightVp / 2));
      this.sync();
    })
    .onVisibleAreaChange([0], (visible: boolean) => { this.visible = visible; if (visible) this.sync(); })
  }
}

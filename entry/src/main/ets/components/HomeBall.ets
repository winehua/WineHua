import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'HomeBall';

// 悬浮球 (Step 3.5): 桌面模式下点按回到启动器。
// PanGesture 拖动 (限制在屏幕内), TapGesture 触发 onHome 回调。
// 初始位置: 右缘、球心屏高 40%——避开右上 (最大化窗口的最小化/关闭按钮区)
// 和底部 (Windows 任务栏); 屏幕正中因任务栏压缩视觉参照而显偏下。
@Component
export struct HomeBall {
  onHome?: () => void;
  // 相对初始位置的拖动偏移 (vp)
  @State private offsetX: number = 0;
  @State private offsetY: number = 0;
  // 初始上边距 (vp): aboutToAppear 里按屏幕高度算成垂直居中
  @State private marginTop: number = 80;
  private panStartX: number = 0;
  private panStartY: number = 0;
  private screenW: number = 0; // vp
  private screenH: number = 0;
  private static readonly SIZE: number = 48;
  private static readonly MARGIN_RIGHT: number = 16;

  private ensureScreenSize(): void {
    if (this.screenW > 0) return;
    try {
      const d = display.getDefaultDisplaySync();
      this.screenW = px2vp(d.width);
      this.screenH = px2vp(d.height);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'display query failed: %{public}s', JSON.stringify(e));
      this.screenW = 360;
      this.screenH = 640;
    }
  }

  aboutToAppear(): void {
    this.ensureScreenSize();
    // 球心位于屏高 40%: 屏幕正中在视觉上偏下 (底部 Windows 任务栏
    // 压缩了人眼参照的桌面区), 40% 处仍避开右上角窗口按钮区
    this.marginTop = this.screenH * 0.4 - HomeBall.SIZE / 2;
  }

  build() {
    // 外层 Stack 撑满并对齐右上; hitTestBehavior(Transparent) 使空白处触摸穿透到桌面层
    Stack() {
      // 球体: 系统 house 符号 + 半透明黑底 + 阴影 (旧版 ⌂ 字符在浅色壁纸下几乎不可见)
      Column() {
        SymbolGlyph($r('sys.symbol.house_fill'))
          .fontSize(24)
          .fontColor([Color.White])
      }
      .width(HomeBall.SIZE)
      .height(HomeBall.SIZE)
      .justifyContent(FlexAlign.Center)
      .borderRadius(HomeBall.SIZE / 2)
      .backgroundColor(Color.Black)
      .opacity(0.45)
      .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
      .margin({ top: this.marginTop, right: HomeBall.MARGIN_RIGHT })
      .offset({ x: this.offsetX, y: this.offsetY })
      .gesture(
        GestureGroup(GestureMode.Parallel,
          TapGesture()
            .onAction(() => {
              this.onHome?.();
            }),
          PanGesture()
            .onActionStart(() => {
              this.ensureScreenSize();
              this.panStartX = this.offsetX;
              this.panStartY = this.offsetY;
            })
            .onActionUpdate((ev: GestureEvent) => {
              // 限制球心在屏幕内: center ∈ [SIZE/2, screen - SIZE/2]
              const half = HomeBall.SIZE / 2;
              const cx0 = this.screenW - HomeBall.MARGIN_RIGHT - half;
              const cy0 = this.marginTop + half;
              const tx = this.panStartX + ev.offsetX;
              const ty = this.panStartY + ev.offsetY;
              this.offsetX = Math.min(Math.max(tx, half - cx0), this.screenW - half - cx0);
              this.offsetY = Math.min(Math.max(ty, half - cy0), this.screenH - half - cy0);
            })
        )
      )
    }
    .width('100%')
    .height('100%')
    .alignContent(Alignment.TopEnd)
    .hitTestBehavior(HitTestMode.Transparent)
  }
}

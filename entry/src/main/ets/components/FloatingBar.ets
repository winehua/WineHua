import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { InputSettingsService } from '../service/InputSettingsService';

const DOMAIN = 0x0000;
const TAG = 'FloatingBar';

// 桌面悬浮条 (设计 4.7): 收纳态为单个 ‹ 球 (沿用 HomeBall 的拖动/初始位),
// 展开态为一排入口 — Home / 输入设置 / 游戏手柄模拟(开关) / 触摸模拟鼠标(开关) /
// 收纳 ›。开关状态直接体现在按钮配色上 (accent 底 = 开)。
// 拖动: 收纳态整球可拖; 展开态拖容器空白处 (按钮 onClick 与容器 PanGesture 并存,
// 轻点走按钮, 按住移动走拖动)。
// 注意: 不提供退出/软键盘入口 (设计决策: 关闭会话走"运行中"页)。
@Component
export struct FloatingBar {
  onHome?: () => void;
  @State private expanded: boolean = false;
  @State private touchpadOn: boolean = false;
  @State private gamepadOn: boolean = false;
  @State private cursorSpeed: number = 1.0;
  @State private showSettings: boolean = false;
  // 布局编辑态镜像 (驱动"编辑手柄布局/完成编辑"按钮文案; 直接读 service 字段不刷新)
  @State private editingLayout: boolean = false;
  // 拖动偏移与初始上边距 (与 HomeBall 同一套语义)
  @State private offsetX: number = 0;
  @State private offsetY: number = 0;
  @State private marginTop: number = 80;
  private panStartX: number = 0;
  private panStartY: number = 0;
  private screenW: number = 0;
  private screenH: number = 0;
  // 展开态自身尺寸 (onAreaChange 量得, 拖动用), 收纳态为球径
  private barW: number = FloatingBar.BALL;
  private barH: number = FloatingBar.BALL;
  private input: InputSettingsService = InputSettingsService.getInstance();
  private static readonly BALL: number = 44;
  private static readonly MARGIN_RIGHT: number = 10;

  private mirrorInput = (): void => {
    this.touchpadOn = this.input.touchpadEnabled;
    this.gamepadOn = this.input.gamepadEnabled;
    this.cursorSpeed = this.input.cursorSpeed;
    this.editingLayout = this.input.editingLayout;
  }

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
    // 球心位于屏高 40% (同 HomeBall: 避开右上窗口按钮区与底部任务栏)
    this.marginTop = this.screenH * 0.4 - FloatingBar.BALL / 2;
    this.input.subscribe(this.mirrorInput);
    this.mirrorInput();
  }

  // 展开态按钮: 图标或文字 + 可选 accent 底 (开关"开"态)
  @Builder
  barButton(content: Resource | string, active: boolean, onTap: () => void) {
    Column() {
      if (typeof content === 'string') {
        Text(content)
          .fontSize(13)
          .fontColor(active ? Color.White : $r('app.color.text_primary'))
      } else {
        SymbolGlyph(content)
          .fontSize(20)
          .fontColor([active ? Color.White : $r('app.color.text_primary')])
      }
    }
    .width(40).height(40)
    .justifyContent(FlexAlign.Center)
    .borderRadius(20)
    .backgroundColor(active ? $r('app.color.accent') : Color.Transparent)
    .onClick(onTap)
  }

  // 输入设置半模态 (设计 4.7: 手柄布局编辑入口 + 光标速度等少量项;
  // 布局编辑器在编辑态覆盖层上进行, 见 GamepadOverlay)
  @Builder
  settingsSheet() {
    Column({ space: 16 }) {
      Row() {
        Text('光标速度')
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text(this.cursorSpeed.toFixed(1))
          .fontColor($r('app.color.text_tertiary'))
      }
      .width('100%')
      Slider({ value: this.cursorSpeed, min: 0.3, max: 2.5, step: 0.1 })
        .onChange((value: number, mode: SliderChangeMode) => {
          // 拖动中只更新生效值 (镜像即时刷速度), 松手/点按才落盘
          this.input.setCursorSpeed(value, mode != SliderChangeMode.Moving);
        })
      Text('手柄布局与按键映射：先开「手柄」开关显示覆盖层，再点下方进入编辑。')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
      Row({ space: 12 }) {
        Button(this.editingLayout ? '完成编辑' : '编辑手柄布局')
          .layoutWeight(1)
          .enabled(this.gamepadOn)
          .backgroundColor($r('app.color.accent'))
          .onClick(() => {
            const next = !this.editingLayout;
            this.input.setEditingLayout(next);
            // 进入编辑态时收起半模态, 露出覆盖层; 完成时留在启动器外不动
            if (next) {
              this.showSettings = false;
            }
          })
        Button('重置布局')
          .layoutWeight(1)
          .enabled(this.gamepadOn)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => {
            this.input.resetLayout();
          })
      }
      .width('100%')
    }
    .width('100%')
    .padding({ left: 20, right: 20, top: 18, bottom: 24 })
  }

  build() {
    Stack() {
      Column() {
        if (!this.expanded) {
          // 收纳态: 单个 ‹ 球 (点按展开; 拖动挪位)
          Column() {
            SymbolGlyph($r('sys.symbol.chevron_left'))
              .fontSize(20)
              .fontColor([Color.White])
          }
          .width(FloatingBar.BALL)
          .height(FloatingBar.BALL)
          .justifyContent(FlexAlign.Center)
          .borderRadius(FloatingBar.BALL / 2)
          .backgroundColor(Color.Black)
          .opacity(0.45)
          .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
          .onClick(() => {
            this.expanded = true;
          })
        } else {
          // 展开态: Home / 输入设置 / 手柄 / 触控板 / 收纳
          Row({ space: 4 }) {
            this.barButton($r('sys.symbol.house_fill'), false, () => {
              this.onHome?.();
            })
            this.barButton($r('sys.symbol.gearshape'), false, () => {
              this.showSettings = true;
            })
            this.barButton($r('sys.symbol.record_circle'), this.gamepadOn, () => {
              this.input.setGamepad(!this.gamepadOn);
            })
            this.barButton($r('sys.symbol.hand_draw'), this.touchpadOn, () => {
              this.input.setTouchpad(!this.touchpadOn);
            })
            this.barButton($r('sys.symbol.chevron_right'), false, () => {
              this.expanded = false;
            })
          }
          .padding({ left: 6, right: 6 })
          .height(FloatingBar.BALL + 8)
          .borderRadius((FloatingBar.BALL + 8) / 2)
          .backgroundColor(Color.Black)
          .opacity(0.55)
          .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
        }
      }
      .margin({ top: this.marginTop, right: FloatingBar.MARGIN_RIGHT })
      .offset({ x: this.offsetX, y: this.offsetY })
      .onAreaChange((_oldValue: Area, newValue: Area) => {
        this.barW = newValue.width as number;
        this.barH = newValue.height as number;
      })
      .gesture(
        PanGesture()
          .onActionStart(() => {
            this.ensureScreenSize();
            this.panStartX = this.offsetX;
            this.panStartY = this.offsetY;
          })
          .onActionUpdate((ev: GestureEvent) => {
            // 限制条在屏幕内 (与 HomeBall 同一钳制语义, 尺寸取当前态实测值)
            const halfW = this.barW / 2;
            const halfH = this.barH / 2;
            const cx0 = this.screenW - FloatingBar.MARGIN_RIGHT - halfW;
            const cy0 = this.marginTop + halfH;
            const tx = this.panStartX + ev.offsetX;
            const ty = this.panStartY + ev.offsetY;
            this.offsetX = Math.min(Math.max(tx, halfW - cx0), this.screenW - halfW - cx0);
            this.offsetY = Math.min(Math.max(ty, halfH - cy0), this.screenH - halfH - cy0);
          })
      )
      .bindSheet($$this.showSettings, this.settingsSheet(), {
        // title 让系统渲染标题栏: 关闭钮收进标题栏右侧固定位, 内容区整体
        // 下移 — 从结构上不存在重叠 (不要无 title 裸开 showClose, 那样关闭
        // 钮悬浮在内容区右上角, 会压住内容行右端)
        title: { title: '输入设置' },
        // 内容只有几行, 用自适应高度 — MEDIUM 固定半屏会留出大片空白
        height: SheetSize.FIT_CONTENT,
        showClose: true
      })
    }
    .width('100%')
    .height('100%')
    .alignContent(Alignment.TopEnd)
    .hitTestBehavior(HitTestMode.Transparent)
  }
}

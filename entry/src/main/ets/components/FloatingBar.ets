import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { InputSettingsService } from '../service/InputSettingsService';

const DOMAIN = 0x0000;
const TAG = 'FloatingBar';

// barButton 入参。必须整体作为单个对象字面量传给 @Builder (按引用传递),
// active 变化才会触发按钮 UI 刷新; 拆成多个基本类型参数是按值传递, 会被
// 快照冻结 — 开关点击后不高亮、重启才显示正确状态, 就是踩的这个坑
interface BarBtnSpec {
  content: Resource | string;
  active: boolean;
  onTap: () => void;
}

// 桌面悬浮条 (设计 4.7): 收纳态为单个 ‹ 球 (沿用 HomeBall 的拖动/初始位),
// 展开态为一排入口 — Home / 输入设置 / 游戏手柄模拟(开关) / 模拟键盘(开关) /
// 触摸模拟鼠标(开关) / 收纳 ›。开关状态直接体现在按钮配色上 (accent 底 = 开)。
// 拖动: 收纳态整球可拖; 展开态拖容器空白处 (按钮 onClick 与容器 PanGesture 并存,
// 轻点走按钮, 按住移动走拖动)。
// 注意: 不提供退出/软键盘入口 (设计决策: 关闭会话走"运行中"页)。
@Component
export struct FloatingBar {
  onHome?: () => void;
  @State private expanded: boolean = false;
  @State private touchpadOn: boolean = false;
  @State private gamepadOn: boolean = false;
  @State private keyboardOn: boolean = false;
  @State private cursorSpeed: number = 1.0;
  @State private showSettings: boolean = false;
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
    this.keyboardOn = this.input.keyboardEnabled;
    this.cursorSpeed = this.input.cursorSpeed;
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
  barButton($$: BarBtnSpec) {
    Column() {
      if (typeof $$.content === 'string') {
        Text($$.content)
          .fontSize(13)
          .fontColor($$.active ? Color.White : $r('app.color.text_primary'))
      } else {
        SymbolGlyph($$.content)
          .fontSize(20)
          .fontColor([$$.active ? Color.White : $r('app.color.text_primary')])
      }
    }
    .width(40).height(40)
    .justifyContent(FlexAlign.Center)
    .borderRadius(20)
    .backgroundColor($$.active ? $r('app.color.accent') : Color.Transparent)
    .onClick($$.onTap)
  }

  // 输入设置半模态 (设计: 光标速度 + 手柄布局编辑入口; 布局编辑在独立全屏
  // 编辑页 GamepadLayoutEditor 进行 — 不需要先开手柄开关, 重置布局也挪进编辑页)
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
      Text('手柄覆盖层开关在悬浮条红点按钮；布局编辑进全屏页，不需要先开开关。')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
      Button('编辑手柄布局')
        .width('100%')
        .backgroundColor($r('app.color.accent'))
        .onClick(() => {
          // 防重入: 编辑页已开时不再重复 beginLayoutEdit (避免覆盖快照)
          if (!this.input.editingLayout) {
            this.input.beginLayoutEdit();
          }
          // 进全屏编辑页, 收起半模态
          this.showSettings = false;
        })
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
            this.barButton({ content: $r('sys.symbol.house_fill'), active: false, onTap: () => {
              this.onHome?.();
            } })
            this.barButton({ content: $r('sys.symbol.gearshape'), active: false, onTap: () => {
              this.showSettings = true;
            } })
            this.barButton({ content: $r('sys.symbol.record_circle'), active: this.gamepadOn, onTap: () => {
              this.input.setGamepad(!this.gamepadOn);
            } })
            this.barButton({ content: $r('sys.symbol.keyboard_square'), active: this.keyboardOn, onTap: () => {
              this.input.setKeyboard(!this.keyboardOn);
            } })
            this.barButton({ content: $r('sys.symbol.hand_draw'), active: this.touchpadOn, onTap: () => {
              this.input.setTouchpad(!this.touchpadOn);
            } })
            this.barButton({ content: $r('sys.symbol.chevron_right'), active: false, onTap: () => {
              this.expanded = false;
            } })
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
      // 阻断点击穿透到桌面: 根容器 Transparent, 按钮 onClick 触发的同时同一击
      // 仍透传成桌面点击 (窗口失焦); BLOCK_HIERARCHY 只阻断悬浮条自身区域,
      // 悬浮条外空白仍穿透桌面 (同 KeyboardOverlay/手柄控件的解法)
      .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
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

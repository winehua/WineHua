import { display, promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { InputSettingsService, PadButton, PadZone, KEY_CATALOG, KeyDef } from '../service/InputSettingsService';
import { PadButtonView, DpadView, StickView } from './GamepadOverlay';

const DOMAIN = 0x0000;
const TAG = 'GamepadLayoutEditor';

// 键位选择对话框 (全屏编辑页点按动作键弹出): 当前键高亮 + 冲突检测由编辑页
// 在 onPick 回调里做 (见 GamepadLayoutEditor.onPickKey)。
@CustomDialog
struct KeyPickerDialog {
  controller?: CustomDialogController;
  title: string = '';
  currentKey: number = 0;
  onPick: (code: number) => void = () => {};

  private input: InputSettingsService = InputSettingsService.getInstance();

  build() {
    Column({ space: 12 }) {
      Text(this.title)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
      Text(this.currentKey > 0 ? `当前：${this.input.keyLabelOf(this.currentKey)}` : '当前：无')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
      Grid() {
        ForEach(KEY_CATALOG, (k: KeyDef) => {
          GridItem() {
            Text(k.label)
              .fontSize(13)
              .fontColor(this.currentKey == k.code ? $r('app.color.accent') : $r('app.color.text_primary'))
              .width('100%')
              .height(36)
              .textAlign(TextAlign.Center)
              .backgroundColor(this.currentKey == k.code ? $r('app.color.accent_bg') : $r('app.color.control_bg'))
              .borderRadius(8)
              .border(this.currentKey == k.code ? { width: 1, color: $r('app.color.accent') } : { width: 0, color: Color.Transparent })
              .onClick(() => {
                this.onPick(k.code);
                this.controller?.close();
              })
          }
        }, (k: KeyDef) => k.label)
      }
      .columnsTemplate('1fr 1fr 1fr 1fr 1fr')
      .columnsGap(8)
      .rowsGap(8)
      .height(200)
      Button('取消')
        .width('100%')
        .backgroundColor($r('app.color.control_bg'))
        .onClick(() => this.controller?.close())
    }
    .padding(18)
    .backgroundColor($r('app.color.overlay_bg'))
    .borderRadius(12)
  }
}

// 全屏手柄布局编辑页 (设计: 布局编辑从游戏覆盖层拆出, 独立全屏模态):
// - 顶部固定「取消 / 手柄布局 / 完成」双出口, 编辑态永远出得来
// - 进入时 service 拍快照, 「取消」恢复快照丢改动, 「完成」保留 (即改即存)
// - 不需要先开手柄开关 (与 gamepadEnabled 解耦); 手柄控件即"开着"的样子
// - 首用引导蒙版一次性展示 (service.introShown 持久化)
// 挂载: Index 根 Stack, desktopActive 时渲染; 组件内部按 editingLayout 自门控。
@Component
export struct GamepadLayoutEditor {
  @State private editing: boolean = false;
  @State private buttons: PadButton[] = [];
  @State private zones: PadZone[] = [];
  @State private showIntro: boolean = false;
  private scrW: number = 0;
  private scrH: number = 0;
  private input: InputSettingsService = InputSettingsService.getInstance();
  private remapId: string = '';
  private keyPickerController: CustomDialogController | null = null;

  private mirrorInput = (): void => {
    this.editing = this.input.editingLayout;
    this.buttons = this.input.buttons;
    this.zones = this.input.zones;
  }

  aboutToAppear(): void {
    try {
      const d = display.getDefaultDisplaySync();
      this.scrW = px2vp(d.width);
      this.scrH = px2vp(d.height);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'display query failed: %{public}s', JSON.stringify(e));
      this.scrW = 1280;
      this.scrH = 800;
    }
    this.input.subscribe(this.mirrorInput);
    this.mirrorInput();
    this.showIntro = !this.input.introShown;
  }

  // 兜底: 桌面退出 (desktopActive 翻 false) 组件被卸载时, 残留的编辑态自动
  // 收口 — 用 finish (保留改动) 而非 cancel, 因为不是用户主动放弃
  aboutToDisappear(): void {
    if (this.input.editingLayout) {
      this.input.finishLayoutEdit();
    }
  }

  private onMoveButton = (id: string, relX: number, relY: number): void => {
    this.input.updateButtonPos(id, relX, relY);
  }

  private onMoveZone = (id: string, relX: number, relY: number): void => {
    this.input.updateZonePos(id, relX, relY);
  }

  private onRemap = (id: string): void => {
    this.remapId = id;
    const btn = this.buttons.find((b: PadButton) => b.id == id);
    this.keyPickerController = new CustomDialogController({
      builder: KeyPickerDialog({
        title: `${id} 键映射`,
        currentKey: btn ? btn.key : 0,
        onPick: (code: number) => {
          this.onPickKey(id, code);
        }
      }),
      customStyle: true
    });
    this.keyPickerController.open();
  }

  // 选键回填: 冲突检测 (另一按钮已占用该键 → 确认覆盖), 无冲突直接写
  private onPickKey(id: string, code: number): void {
    const owner = this.input.findKeyOwner(code, id);
    if (owner != '') {
      AlertDialog.show({
        title: '按键已被占用',
        message: `该键已被 ${owner} 按钮使用，是否覆盖？`,
        primaryButton: {
          value: '取消',
          action: () => {}
        },
        secondaryButton: {
          value: '覆盖',
          fontColor: $r('app.color.danger'),
          action: () => {
            this.input.updateButtonKey(id, code);
          }
        }
      });
    } else {
      this.input.updateButtonKey(id, code);
    }
  }

  private onDone(): void {
    this.input.finishLayoutEdit();
    promptAction.showToast({ message: '布局已保存' });
  }

  private onCancel(): void {
    if (this.input.layoutDirty) {
      AlertDialog.show({
        title: '放弃本次修改？',
        message: '取消将恢复进入编辑页之前的布局，本次改动会丢失。',
        primaryButton: {
          value: '继续编辑',
          action: () => {}
        },
        secondaryButton: {
          value: '放弃',
          fontColor: $r('app.color.danger'),
          action: () => {
            this.input.cancelLayoutEdit();
          }
        }
      });
    } else {
      this.input.cancelLayoutEdit();
    }
  }

  private onReset(): void {
    AlertDialog.show({
      title: '还原默认布局',
      message: '将恢复默认布局，当前修改将丢失。',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '还原',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.input.resetLayout();
        }
      }
    });
  }

  private onIntroClose(): void {
    this.showIntro = false;
    this.input.markIntroShown();
  }

  // @Builder 返回 void 不能挂 position (同 Index 注释里的坑), 定位须在 Builder 内部
  @Builder
  topBar() {
    Row() {
      Text('取消')
        .fontSize(15)
        .fontColor($r('app.color.text_primary'))
        .padding({ left: 6, right: 6, top: 4, bottom: 4 })
        .onClick(() => this.onCancel())
      Text('手柄布局')
        .layoutWeight(1)
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .textAlign(TextAlign.Center)
      Text('完成')
        .fontSize(15)
        .fontColor($r('app.color.accent'))
        .fontWeight(FontWeight.Medium)
        .padding({ left: 6, right: 6, top: 4, bottom: 4 })
        .onClick(() => this.onDone())
    }
    .width('100%')
    .height(52)
    .padding({ left: 16, right: 16 })
    .backgroundColor($r('app.color.header_bg'))
    .alignItems(VerticalAlign.Center)
    .position({ x: 0, y: 0 })
  }

  @Builder
  bottomBar() {
    Column({ space: 10 }) {
      Text('拖动控件挪位置 · 点按圆钮改键位')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
      Button('还原默认布局')
        .width(220)
        .height(38)
        .fontSize(13)
        .backgroundColor($r('app.color.control_bg'))
        .onClick(() => this.onReset())
    }
    .width('100%')
    .padding({ top: 8, bottom: 10 })
    .alignItems(HorizontalAlign.Center)
    .backgroundColor($r('app.color.header_bg'))
    .position({ x: 0, y: this.scrH - 82 })
  }

  @Builder
  introGuide() {
    Column() {
      Column({ space: 14 }) {
        Text('第一次使用手柄布局')
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
        Text('1. 拖动控件，可移动它在屏幕上的位置')
          .fontSize(14)
          .fontColor($r('app.color.text_secondary'))
        Text('2. 点按圆钮，可更换它对应的按键')
          .fontSize(14)
          .fontColor($r('app.color.text_secondary'))
        Text('3. 编辑完点右上角「完成」保存')
          .fontSize(14)
          .fontColor($r('app.color.text_secondary'))
        Text('（十字键和摇杆的键位固定，只能移动）')
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
        Text('点任意处开始')
          .fontSize(12)
          .fontColor($r('app.color.accent'))
      }
      .width(340)
      .padding({ left: 24, right: 24, top: 22, bottom: 22 })
      .backgroundColor($r('app.color.overlay_bg'))
      .borderRadius(12)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: $r('app.color.shadow'), offsetX: 0, offsetY: 8 })
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor($r('app.color.overlay_mask'))
    .onClick(() => this.onIntroClose())
  }

  build() {
    Stack() {
      if (this.editing) {
        // 顶部栏 / 底部条先渲染 (z 低), 控件后渲染 (z 高) — 标题栏不再遮住顶部
        // 控件 (默认肩键 LT/RT 贴顶)。位置在 Builder 内部。
        this.topBar()
        this.bottomBar()
        // 控件层: position 相对全屏 Stack, 与游玩态覆盖层同一坐标语义
        // key 含位置: ForEach 用 key=id 复用组件时 @Prop 不更新 (拖动落库/还原默认
        // 后控件显示旧位置 = 松手回弹/还原无效)。位置变化 → key 变 → 组件重建 → 显示新位置。
        // 拖动中 (未落库) relX/relY 不变, key 不变, 组件不重建, 靠本地偏移跟手。
        ForEach(this.buttons, (btn: PadButton) => {
          PadButtonView({
            btn: btn,
            editing: true,
            scrW: this.scrW,
            scrH: this.scrH,
            // 纵向钳制避开顶栏(52)/底栏(82): 圆钮半高 28 + 余量
            clampMinRelY: (52 + 28 + 6) / this.scrH,
            clampMaxRelY: 1 - (82 + 28 + 6) / this.scrH,
            keyLabel: this.input.keyLabelOf(btn.key),
            onMove: this.onMoveButton,
            onRemap: this.onRemap
          })
        }, (btn: PadButton) => `${btn.id}|${btn.relX.toFixed(4)}|${btn.relY.toFixed(4)}`)
        ForEach(this.zones, (zone: PadZone) => {
          if (zone.id == 'dpad') {
            DpadView({
              zone: zone,
              editing: true,
              scrW: this.scrW,
              scrH: this.scrH,
              // 摇杆/十字键半高 60 + 余量
              clampMinRelY: (52 + 60 + 6) / this.scrH,
              clampMaxRelY: 1 - (82 + 60 + 6) / this.scrH,
              onMove: this.onMoveZone
            })
          } else {
            StickView({
              zone: zone,
              editing: true,
              scrW: this.scrW,
              scrH: this.scrH,
              clampMinRelY: (52 + 60 + 6) / this.scrH,
              clampMaxRelY: 1 - (82 + 60 + 6) / this.scrH,
              onMove: this.onMoveZone
            })
          }
        }, (zone: PadZone) => `${zone.id}|${zone.relX.toFixed(4)}|${zone.relY.toFixed(4)}`)
      }
      // 首用引导蒙版 (最顶)
      if (this.showIntro && this.editing) {
        this.introGuide()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.editing ? $r('app.color.overlay_mask') : Color.Transparent)
    // 关键: 非编辑态必须穿透 (Transparent), 否则这个全屏透明 Stack 常驻顶层
    // 会把桌面/软键盘的触摸全吞掉 (GamepadOverlay/KeyboardOverlay 同款设置)。
    // 编辑态用 BLOCK_HIERARCHY (不要用 Block): Block 会连子节点一起阻塞,
    // 导致引导蒙版/顶栏按钮/控件拖动全点不动; BLOCK_HIERARCHY 让子节点正常
    // 响应, 只阻断后方兄弟/父节点 (模态防误触游戏, 同 KeyboardOverlay)
    .hitTestBehavior(this.editing ? HitTestMode.BLOCK_HIERARCHY : HitTestMode.Transparent)
  }
}

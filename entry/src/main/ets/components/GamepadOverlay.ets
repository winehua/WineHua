import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputSettingsService, PadButton, PadZone, KEY_CATALOG, KeyDef } from '../service/InputSettingsService';

const DOMAIN = 0x0000;
const TAG = 'GamepadOverlay';

// 十字键键位 (固定 = 方向键, v1 不开放重映射; 见 InputSettingsService 注释)
const DPAD_KEYS: number[] = [103, 108, 105, 106]; // up down left right
// 摇杆键位 (固定 = WASD): bit0=W bit1=A bit2=S bit3=D
const STICK_KEYS: number[] = [17, 30, 31, 32];

// 注入与物理键盘同通道 (sendKeyEvent 直达桌面 root)
function sendPadKey(code: number, down: boolean): void {
  const rootId = WineWindowManager.getInstance().getDesktopRootId();
  if (rootId <= 0 || code <= 0) {
    return;
  }
  testNapi.sendKeyEvent(rootId, code, down);
}

// 键位选择对话框 (编辑态点按按钮弹出)
@CustomDialog
struct KeyPickerDialog {
  controller?: CustomDialogController;
  title: string = '';
  onPick: (code: number) => void = () => {};

  build() {
    Column({ space: 12 }) {
      Text(this.title)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
      Grid() {
        ForEach(KEY_CATALOG, (k: KeyDef) => {
          GridItem() {
            Text(k.label)
              .fontSize(13)
              .fontColor($r('app.color.text_primary'))
              .width('100%')
              .height(36)
              .textAlign(TextAlign.Center)
              .backgroundColor($r('app.color.control_bg'))
              .borderRadius(8)
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

// 单个动作键 (XYAB/肩键): 游玩态按下注入 key down/up; 编辑态可拖动、点按改键。
// @Prop 字段随父组件 ForEach 重渲染更新 (布局/键位改动经 service → notify → 新数组)。
@Component
struct PadButtonView {
  @Prop btn: PadButton = { id: '', label: '', relX: 0, relY: 0, key: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  onKey: (id: string, down: boolean) => void = () => {};
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  onRemap: (id: string) => void = () => {};
  @State pressed: boolean = false;
  @State dragDx: number = 0;
  @State dragDy: number = 0;

  build() {
    Column() {
      Text(this.btn.label)
        .fontSize(14)
        .fontColor(Color.White)
    }
    .width(56)
    .height(56)
    .justifyContent(FlexAlign.Center)
    .borderRadius(28)
    .backgroundColor(this.pressed ? '#AAFFFFFF' : '#33000000')
    .border({
      width: this.editing ? 2 : 1,
      color: this.editing ? $r('app.color.accent') : '#66FFFFFF',
      style: this.editing ? BorderStyle.Dashed : BorderStyle.Solid
    })
    .position({ x: this.btn.relX * this.scrW - 28, y: this.btn.relY * this.scrH - 28 })
    .offset({ x: this.dragDx, y: this.dragDy })
    .onTouch((ev: TouchEvent) => {
      if (this.editing) {
        return; // 编辑态: 手势全走下方 gesture (拖动/点按改键)
      }
      if (ev.type === TouchType.Down) {
        this.pressed = true;
        this.onKey(this.btn.id, true);
      } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
        this.pressed = false;
        this.onKey(this.btn.id, false);
      }
    })
    .gesture(this.editing ?
      GestureGroup(GestureMode.Parallel,
        PanGesture()
          .onActionStart(() => {
            this.dragDx = 0;
            this.dragDy = 0;
          })
          .onActionUpdate((ev: GestureEvent) => {
            this.dragDx = ev.offsetX;
            this.dragDy = ev.offsetY;
          })
          .onActionEnd((ev: GestureEvent) => {
            // 拖动结束才落库 (拖动中只动本地 offset, 避免每帧 persist+notify)
            const cx = this.btn.relX * this.scrW + ev.offsetX;
            const cy = this.btn.relY * this.scrH + ev.offsetY;
            this.dragDx = 0;
            this.dragDy = 0;
            this.onMove(this.btn.id,
              Math.min(0.96, Math.max(0.04, cx / this.scrW)),
              Math.min(0.94, Math.max(0.06, cy / this.scrH)));
          }),
        TapGesture()
          .onAction(() => {
            this.onRemap(this.btn.id);
          })
      ) : null)
  }
}

// 十字键区: 4 方向固定映射方向键; 编辑态整体可拖
@Component
struct DpadView {
  @Prop zone: PadZone = { id: '', relX: 0, relY: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  // 按位记录按下方向 (bit0up 1down 2left 3right): 双指斜向(如 上+右)是常态,
  // 单值 pressedDir 会让先松开的方向永远收不到 key-up (卡键)
  @State pressedMask: number = 0;
  @State dragDx: number = 0;
  @State dragDy: number = 0;
  private static readonly CELL: number = 40;
  private static readonly SPAN: number = 120;

  @Builder
  dirBtn(dir: number, icon: Resource, px: number, py: number) {
    Column() {
      SymbolGlyph(icon)
        .fontSize(18)
        .fontColor([Color.White])
    }
    .width(DpadView.CELL)
    .height(DpadView.CELL)
    .justifyContent(FlexAlign.Center)
    .borderRadius(10)
    .backgroundColor((this.pressedMask & (1 << dir)) != 0 ? '#AAFFFFFF' : '#33000000')
    .border({
      width: 1,
      color: this.editing ? $r('app.color.accent') : '#66FFFFFF',
      style: this.editing ? BorderStyle.Dashed : BorderStyle.Solid
    })
    .position({ x: px, y: py })
    .onTouch((ev: TouchEvent) => {
      if (this.editing) {
        return;
      }
      if (ev.type === TouchType.Down) {
        this.pressedMask |= (1 << dir);
        sendPadKey(DPAD_KEYS[dir], true);
      } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
        if ((this.pressedMask & (1 << dir)) != 0) {
          this.pressedMask &= ~(1 << dir);
          sendPadKey(DPAD_KEYS[dir], false);
        }
      }
    })
  }

  build() {
    Stack() {
      // 键位: up(40,0) left(0,40) right(80,40) down(40,80)
      this.dirBtn(0, $r('sys.symbol.chevron_up'), 40, 0)
      this.dirBtn(2, $r('sys.symbol.chevron_left'), 0, 40)
      this.dirBtn(3, $r('sys.symbol.chevron_right'), 80, 40)
      this.dirBtn(1, $r('sys.symbol.chevron_down'), 40, 80)
    }
    .width(DpadView.SPAN)
    .height(DpadView.SPAN)
    .position({
      x: this.zone.relX * this.scrW - DpadView.SPAN / 2,
      y: this.zone.relY * this.scrH - DpadView.SPAN / 2
    })
    .offset({ x: this.dragDx, y: this.dragDy })
    .gesture(this.editing ?
      PanGesture()
        .onActionStart(() => {
          this.dragDx = 0;
          this.dragDy = 0;
        })
        .onActionUpdate((ev: GestureEvent) => {
          this.dragDx = ev.offsetX;
          this.dragDy = ev.offsetY;
        })
        .onActionEnd((ev: GestureEvent) => {
          const cx = this.zone.relX * this.scrW + ev.offsetX;
          const cy = this.zone.relY * this.scrH + ev.offsetY;
          this.dragDx = 0;
          this.dragDy = 0;
          this.onMove(this.zone.id,
            Math.min(0.96, Math.max(0.04, cx / this.scrW)),
            Math.min(0.94, Math.max(0.06, cy / this.scrH)));
        }) : null)
  }
}

// 左摇杆: 拖动出死区即按 8 向合成 WASD 组合键, 回中全释放; 编辑态整体可拖
@Component
struct StickView {
  @Prop zone: PadZone = { id: '', relX: 0, relY: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  @State knobDx: number = 0;
  @State knobDy: number = 0;
  @State dragDx: number = 0;
  @State dragDy: number = 0;
  private curMask: number = 0;
  private static readonly RING: number = 120;
  private static readonly KNOB: number = 48;
  private static readonly TRAVEL: number = 36;  // 摇杆头最大行程 (vp)
  private static readonly DEAD: number = 14;    // 死区 (vp)

  // 掩码差量注入: 只发变化的键 (bit0 W / bit1 A / bit2 S / bit3 D)
  private applyMask(next: number): void {
    for (let i = 0; i < 4; i++) {
      const wasDown = (this.curMask & (1 << i)) != 0;
      const isDown = (next & (1 << i)) != 0;
      if (wasDown != isDown) {
        sendPadKey(STICK_KEYS[i], isDown);
      }
    }
    this.curMask = next;
  }

  build() {
    Stack() {
      // 外环
      Circle({ width: StickView.RING, height: StickView.RING })
        .fill('#22000000')
        .stroke(this.editing ? $r('app.color.accent') : '#66FFFFFF')
        .strokeWidth(this.editing ? 2 : 1)
      // 摇杆头
      Circle({ width: StickView.KNOB, height: StickView.KNOB })
        .fill('#44000000')
        .stroke('#88FFFFFF')
        .strokeWidth(1)
        .offset({ x: this.knobDx, y: this.knobDy })
    }
    .width(StickView.RING)
    .height(StickView.RING)
    .position({
      x: this.zone.relX * this.scrW - StickView.RING / 2,
      y: this.zone.relY * this.scrH - StickView.RING / 2
    })
    .offset({ x: this.dragDx, y: this.dragDy })
    .onTouch((ev: TouchEvent) => {
      if (this.editing) {
        return;
      }
      const t = ev.changedTouches[0];
      if (!t) {
        return;
      }
      if (ev.type === TouchType.Down || ev.type === TouchType.Move) {
        // t.x/t.y 相对本组件 (vp), 中心 = RING/2
        let dx = t.x - StickView.RING / 2;
        let dy = t.y - StickView.RING / 2;
        const len = Math.sqrt(dx * dx + dy * dy);
        if (len > StickView.TRAVEL) {
          dx = dx * StickView.TRAVEL / len;
          dy = dy * StickView.TRAVEL / len;
        }
        this.knobDx = dx;
        this.knobDy = dy;
        let mask = 0;
        if (dy < -StickView.DEAD) mask |= 1;       // W
        if (dx < -StickView.DEAD) mask |= 2;       // A
        if (dy > StickView.DEAD) mask |= 4;        // S
        if (dx > StickView.DEAD) mask |= 8;        // D
        this.applyMask(mask);
      } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
        this.knobDx = 0;
        this.knobDy = 0;
        this.applyMask(0);
      }
    })
    .gesture(this.editing ?
      PanGesture()
        .onActionStart(() => {
          this.dragDx = 0;
          this.dragDy = 0;
        })
        .onActionUpdate((ev: GestureEvent) => {
          this.dragDx = ev.offsetX;
          this.dragDy = ev.offsetY;
        })
        .onActionEnd((ev: GestureEvent) => {
          const cx = this.zone.relX * this.scrW + ev.offsetX;
          const cy = this.zone.relY * this.scrH + ev.offsetY;
          this.dragDx = 0;
          this.dragDy = 0;
          this.onMove(this.zone.id,
            Math.min(0.96, Math.max(0.04, cx / this.scrW)),
            Math.min(0.94, Math.max(0.06, cy / this.scrH)));
        }) : null)
  }
}

// 游戏手柄模拟覆盖层 (设计 4.7): 悬浮条"手柄"开关控制显隐; 控件位置/键位
// 持久化在 InputSettingsService。编辑态 (输入设置 → 编辑手柄布局) 控件变为
// 可拖动 + 点按改键。容器 Transparent 穿透: 空白处触摸直达桌面层, 只有控件
// 本身消费触摸 — 这是"边玩边调"与"游玩时不挡操作"的关键。
@Component
export struct GamepadOverlay {
  @State private padEnabled: boolean = false;
  @State private editing: boolean = false;
  @State private buttons: PadButton[] = [];
  @State private zones: PadZone[] = [];
  private scrW: number = 0;
  private scrH: number = 0;
  private input: InputSettingsService = InputSettingsService.getInstance();
  private remapId: string = '';
  private keyPickerController: CustomDialogController | null = null;

  private mirrorInput = (): void => {
    this.padEnabled = this.input.gamepadEnabled;
    this.editing = this.input.editingLayout && this.input.gamepadEnabled;
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
  }

  private onPadKey = (id: string, down: boolean): void => {
    const btn = this.input.buttons.find((b: PadButton) => b.id == id);
    if (btn) {
      sendPadKey(btn.key, down);
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
    const btn = this.input.buttons.find((b: PadButton) => b.id == id);
    this.keyPickerController = new CustomDialogController({
      builder: KeyPickerDialog({
        title: `${id} 键映射 (当前: ${btn ? this.input.keyLabelOf(btn.key) : '未知'})`,
        onPick: (code: number) => {
          this.input.updateButtonKey(this.remapId, code);
        }
      }),
      customStyle: true
    });
    this.keyPickerController.open();
  }

  build() {
    Stack() {
      if (this.padEnabled) {
        if (this.editing) {
          Column() {
            Text('手柄编辑中：拖动控件挪位置，点按圆钮改键位；完成后到悬浮条「输入设置」点完成')
              .fontSize(12)
              .fontColor(Color.White)
              .padding({ left: 12, right: 12, top: 6, bottom: 6 })
              .backgroundColor('#AA000000')
              .borderRadius(8)
          }
          .width('100%')
          .alignItems(HorizontalAlign.Center)
          .position({ x: 0, y: 8 })
          .hitTestBehavior(HitTestMode.None)
        }
        ForEach(this.buttons, (btn: PadButton) => {
          PadButtonView({
            btn: btn,
            editing: this.editing,
            scrW: this.scrW,
            scrH: this.scrH,
            onKey: this.onPadKey,
            onMove: this.onMoveButton,
            onRemap: this.onRemap
          })
        }, (btn: PadButton) => btn.id)
        ForEach(this.zones, (zone: PadZone) => {
          if (zone.id == 'dpad') {
            DpadView({
              zone: zone,
              editing: this.editing,
              scrW: this.scrW,
              scrH: this.scrH,
              onMove: this.onMoveZone
            })
          } else {
            StickView({
              zone: zone,
              editing: this.editing,
              scrW: this.scrW,
              scrH: this.scrH,
              onMove: this.onMoveZone
            })
          }
        }, (zone: PadZone) => zone.id)
      }
    }
    .width('100%')
    .height('100%')
    .hitTestBehavior(HitTestMode.Transparent)
  }
}

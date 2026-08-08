import { display, promptAction } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputSettingsService, PadButton, PadZone } from '../service/InputSettingsService';

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

// 单个动作键 (XYAB/肩键): 游玩态按下注入 key down/up; 编辑态 (GamepadLayoutEditor
// 全屏编辑页) 可拖动、点按改键。@Prop 字段随父组件 ForEach 重渲染更新 (布局/键位
// 改动经 service → notify → 新数组)。
// keyLabel 仅编辑页传入: 圆钮下方小字显示当前键位 (如 X → Space), 游玩态不传。
@Component
export struct PadButtonView {
  @Prop btn: PadButton = { id: '', label: '', relX: 0, relY: 0, key: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  // 编辑页拖动时纵向钳制范围 (避开顶栏/底栏); 游玩态用默认宽松值
  @Prop clampMinRelY: number = 0.06;
  @Prop clampMaxRelY: number = 0.94;
  // 编辑页传入: 圆钮下方小字 (当前键位名); 空则不显示 (游玩态)
  @Prop keyLabel: string = '';
  onKey: (id: string, down: boolean) => void = () => {};
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  onRemap: (id: string) => void = () => {};
  @State pressed: boolean = false;
  // 拖动增量 (相对初始 relX/relY, 归一化 0-1)。直接参与 position 计算,
  // 不用 .offset 暂存 — KeyboardOverlay 实测 offset 拖动中不刷新 (抬起才跳变)
  @State dragRelX: number = 0;
  @State dragRelY: number = 0;
  // 编辑态 onTouch 拖动状态 (不用 gesture: onTouch 与 gesture 同组件会抢事件,
  // 实测 PanGesture 不触发; 事件走 onTouch 畅通)
  private startX: number = 0;
  private startY: number = 0;
  private startRelX: number = 0;
  private startRelY: number = 0;
  private dragMoved: boolean = false;

  // 编辑态拖动/点击判定 (onTouch 驱动): 位移 > 5vp 判定为拖动 (落库), 否则点击改键
  private onEditTouch(ev: TouchEvent): void {
    if (ev.type === TouchType.Down) {
      const t = ev.touches[0];
      if (t) {
        // 用 windowX/windowY (相对窗口): x/y 相对组件, 拖动中组件 position 变化会导致位移算错 (松手弹回)
        this.startX = t.windowX;
        this.startY = t.windowY;
      }
      this.startRelX = this.btn.relX;
      this.startRelY = this.btn.relY;
      this.dragRelX = 0;
      this.dragRelY = 0;
      this.dragMoved = false;
    } else if (ev.type === TouchType.Move) {
      const t = ev.touches[0];
      if (t) {
        if (!this.dragMoved &&
          (Math.abs(t.windowX - this.startX) > 5 || Math.abs(t.windowY - this.startY) > 5)) {
          this.dragMoved = true;
        }
        if (this.dragMoved) {
          this.dragRelX = (t.windowX - this.startX) / this.scrW;
          this.dragRelY = (t.windowY - this.startY) / this.scrH;
        }
      }
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      if (this.dragMoved) {
        const cx = this.startRelX + this.dragRelX;
        const cy = this.startRelY + this.dragRelY;
        this.dragRelX = 0;
        this.dragRelY = 0;
        this.onMove(this.btn.id,
          Math.min(0.96, Math.max(0.04, cx)),
          Math.min(this.clampMaxRelY, Math.max(this.clampMinRelY, cy)));
      } else {
        this.onRemap(this.btn.id);
      }
      this.dragRelX = 0;
      this.dragRelY = 0;
    }
  }

  build() {
    Stack() {
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
      .onTouch((ev: TouchEvent) => {
        if (this.editing) {
          this.onEditTouch(ev);
          return; // 编辑态: 拖动/点按全走 onEditTouch (onTouch 与 gesture 同组件会抢事件)
        }
        if (ev.type === TouchType.Down) {
          this.pressed = true;
          this.onKey(this.btn.id, true);
        } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
          this.pressed = false;
          this.onKey(this.btn.id, false);
        }
      })
      if (this.keyLabel != '') {
        Text(this.keyLabel)
          .width(56)
          .fontSize(9)
          .fontColor($r('app.color.text_tertiary'))
          .textAlign(TextAlign.Center)
          .position({ x: 0, y: 58 })
          .hitTestBehavior(HitTestMode.None)
      }
    }
    .width(56)
    .height(56)
    .position({
      x: (this.btn.relX + this.dragRelX) * this.scrW - 28,
      y: (this.btn.relY + this.dragRelY) * this.scrH - 28
    })
    // BLOCK_HIERARCHY: 阻断点击穿透到桌面 (覆盖层容器 Transparent, 控件命中仍
    // 会透传 → 点手柄按钮时桌面窗口失焦; 同 KeyboardOverlay 面板的解法)。
    // 只阻断控件区域, 空白处仍穿透桌面 (游玩时不挡操作)
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
  }
}

// 十字键区: 4 方向固定映射方向键; 编辑态整体可拖 (键位固定, 点按仅提示)
@Component
export struct DpadView {
  @Prop zone: PadZone = { id: '', relX: 0, relY: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  // 编辑页拖动时纵向钳制范围 (避开顶栏/底栏); 游玩态用默认宽松值
  @Prop clampMinRelY: number = 0.06;
  @Prop clampMaxRelY: number = 0.94;
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  // 按位记录按下方向 (bit0up 1down 2left 3right): 双指斜向(如 上+右)是常态,
  // 单值 pressedDir 会让先松开的方向永远收不到 key-up (卡键)
  @State pressedMask: number = 0;
  // 拖动增量 (相对初始 relX/relY, 归一化); 直接参与 position 计算, 不用 .offset
  @State dragRelX: number = 0;
  @State dragRelY: number = 0;
  // 编辑态 onTouch 拖动状态 (不用 gesture: onTouch 与 gesture 同组件会抢事件)
  private startX: number = 0;
  private startY: number = 0;
  private startRelX: number = 0;
  private startRelY: number = 0;
  private dragMoved: boolean = false;
  private static readonly CELL: number = 40;
  private static readonly SPAN: number = 120;

  // 编辑态拖动 (onTouch 驱动, 挂在整组 Stack): 位移 > 5vp 拖动落库, 否则点击提示键位固定
  private onEditTouch(ev: TouchEvent): void {
    if (ev.type === TouchType.Down) {
      const t = ev.touches[0];
      if (t) {
        // 用 windowX/windowY (相对窗口): x/y 相对组件, 拖动中组件 position 变化会导致位移算错 (松手弹回)
        this.startX = t.windowX;
        this.startY = t.windowY;
      }
      this.startRelX = this.zone.relX;
      this.startRelY = this.zone.relY;
      this.dragRelX = 0;
      this.dragRelY = 0;
      this.dragMoved = false;
    } else if (ev.type === TouchType.Move) {
      const t = ev.touches[0];
      if (t) {
        if (!this.dragMoved &&
          (Math.abs(t.windowX - this.startX) > 5 || Math.abs(t.windowY - this.startY) > 5)) {
          this.dragMoved = true;
        }
        if (this.dragMoved) {
          this.dragRelX = (t.windowX - this.startX) / this.scrW;
          this.dragRelY = (t.windowY - this.startY) / this.scrH;
        }
      }
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      if (this.dragMoved) {
        const cx = this.startRelX + this.dragRelX;
        const cy = this.startRelY + this.dragRelY;
        this.dragRelX = 0;
        this.dragRelY = 0;
        this.onMove(this.zone.id,
          Math.min(0.96, Math.max(0.04, cx)),
          Math.min(this.clampMaxRelY, Math.max(this.clampMinRelY, cy)));
      } else {
        promptAction.showToast({ message: '十字键键位固定，只能移动位置' });
      }
      this.dragRelX = 0;
      this.dragRelY = 0;
    }
  }

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
      x: (this.zone.relX + this.dragRelX) * this.scrW - DpadView.SPAN / 2,
      y: (this.zone.relY + this.dragRelY) * this.scrH - DpadView.SPAN / 2
    })
    // 阻断点击穿透到桌面 (同 PadButtonView)
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
    .onTouch((ev: TouchEvent) => {
      if (this.editing) {
        this.onEditTouch(ev);
      }
    })
  }
}

// 左摇杆: 拖动出死区即按 8 向合成 WASD 组合键, 回中全释放; 编辑态整体可拖
// (键位固定, 点按仅提示)
@Component
export struct StickView {
  @Prop zone: PadZone = { id: '', relX: 0, relY: 0 };
  @Prop editing: boolean = false;
  @Prop scrW: number = 0;
  @Prop scrH: number = 0;
  // 编辑页拖动时纵向钳制范围 (避开顶栏/底栏); 游玩态用默认宽松值
  @Prop clampMinRelY: number = 0.06;
  @Prop clampMaxRelY: number = 0.94;
  onMove: (id: string, relX: number, relY: number) => void = () => {};
  @State knobDx: number = 0;
  @State knobDy: number = 0;
  // 拖动增量 (相对初始 relX/relY, 归一化); 直接参与 position 计算, 不用 .offset
  @State dragRelX: number = 0;
  @State dragRelY: number = 0;
  // 编辑态 onTouch 拖动状态 (不用 gesture: onTouch 与 gesture 同组件会抢事件)
  private startX: number = 0;
  private startY: number = 0;
  private startRelX: number = 0;
  private startRelY: number = 0;
  private dragMoved: boolean = false;
  private curMask: number = 0;
  private static readonly RING: number = 120;
  private static readonly KNOB: number = 48;
  private static readonly TRAVEL: number = 36;  // 摇杆头最大行程 (vp)
  private static readonly DEAD: number = 14;    // 死区 (vp)

  // 编辑态拖动 (onTouch 驱动): 位移 > 5vp 拖动落库, 否则点击提示键位固定
  private onEditTouch(ev: TouchEvent): void {
    if (ev.type === TouchType.Down) {
      const t = ev.touches[0];
      if (t) {
        // 用 windowX/windowY (相对窗口): x/y 相对组件, 拖动中组件 position 变化会导致位移算错 (松手弹回)
        this.startX = t.windowX;
        this.startY = t.windowY;
      }
      this.startRelX = this.zone.relX;
      this.startRelY = this.zone.relY;
      this.dragRelX = 0;
      this.dragRelY = 0;
      this.dragMoved = false;
    } else if (ev.type === TouchType.Move) {
      const t = ev.touches[0];
      if (t) {
        if (!this.dragMoved &&
          (Math.abs(t.windowX - this.startX) > 5 || Math.abs(t.windowY - this.startY) > 5)) {
          this.dragMoved = true;
        }
        if (this.dragMoved) {
          this.dragRelX = (t.windowX - this.startX) / this.scrW;
          this.dragRelY = (t.windowY - this.startY) / this.scrH;
        }
      }
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      if (this.dragMoved) {
        const cx = this.startRelX + this.dragRelX;
        const cy = this.startRelY + this.dragRelY;
        this.dragRelX = 0;
        this.dragRelY = 0;
        this.onMove(this.zone.id,
          Math.min(0.96, Math.max(0.04, cx)),
          Math.min(this.clampMaxRelY, Math.max(this.clampMinRelY, cy)));
      } else {
        promptAction.showToast({ message: '摇杆键位固定（WASD），只能移动位置' });
      }
      this.dragRelX = 0;
      this.dragRelY = 0;
    }
  }

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
      x: (this.zone.relX + this.dragRelX) * this.scrW - StickView.RING / 2,
      y: (this.zone.relY + this.dragRelY) * this.scrH - StickView.RING / 2
    })
    // 阻断点击穿透到桌面 (同 PadButtonView)
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
    .onTouch((ev: TouchEvent) => {
      if (this.editing) {
        this.onEditTouch(ev);
        return; // 编辑态: 拖动/点按提示全走 onEditTouch
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
  }
}

// 游戏手柄模拟覆盖层 (设计 4.7): 悬浮条"手柄"开关控制显隐; 仅游玩态 —
// 布局编辑在独立全屏编辑页 GamepadLayoutEditor 进行 (不进本组件)。
// 容器 Transparent 穿透: 空白处触摸直达桌面层, 只有控件本身消费触摸 —
// 游玩时不挡操作的关键。
@Component
export struct GamepadOverlay {
  @State private padEnabled: boolean = false;
  @State private buttons: PadButton[] = [];
  @State private zones: PadZone[] = [];
  private scrW: number = 0;
  private scrH: number = 0;
  private input: InputSettingsService = InputSettingsService.getInstance();

  private mirrorInput = (): void => {
    this.padEnabled = this.input.gamepadEnabled;
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

  build() {
    Stack() {
      if (this.padEnabled) {
        ForEach(this.buttons, (btn: PadButton) => {
          PadButtonView({
            btn: btn,
            editing: false,
            scrW: this.scrW,
            scrH: this.scrH,
            onKey: this.onPadKey
          })
        }, (btn: PadButton) => btn.id)
        ForEach(this.zones, (zone: PadZone) => {
          if (zone.id == 'dpad') {
            DpadView({
              zone: zone,
              editing: false,
              scrW: this.scrW,
              scrH: this.scrH
            })
          } else {
            StickView({
              zone: zone,
              editing: false,
              scrW: this.scrW,
              scrH: this.scrH
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

import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputSettingsService } from '../service/InputSettingsService';

const DOMAIN = 0x0000;
const TAG = 'KeyboardOverlay';

// 注入与物理键盘/手柄同通道 (sendKeyEvent 直达桌面 root)
function sendKbKey(code: number, down: boolean): void {
  const rootId = WineWindowManager.getInstance().getDesktopRootId();
  if (rootId <= 0 || code <= 0) {
    return;
  }
  testNapi.sendKeyEvent(rootId, code, down);
}

interface KbKey {
  label: string;
  code: number;  // evdev key code
  w: number;     // 行内 layoutWeight
  mod: number;   // 0 = 普通键; 非 0 = 修饰键锁存位 (1 Shift / 2 Ctrl / 4 Alt)
}

// keyView 入参。必须整体作为单个对象字面量传给 @Builder (按引用传递),
// pressed/latched 变化才会刷新键帽配色 — 拆成基本类型参数会被快照冻结
// (同 FloatingBar barButton 踩过的坑)
interface KeySpec {
  k: KbKey;
  pressed: boolean;   // 正被按住
  latched: boolean;   // 修饰键锁存中
}

function key(label: string, code: number, w: number = 1, mod: number = 0): KbKey {
  return { label: label, code: code, w: w, mod: mod };
}

function letters(start: number, labels: string): KbKey[] {
  const out: KbKey[] = [];
  for (let i = 0; i < labels.length; i++) {
    out.push(key(labels[i], start + i));
  }
  return out;
}

// 紧凑 QWERTY 布局 (evdev code): 数字行 + 三行字母 + 空格/方向行;
// 只保留桌面操作高频键 (Esc/Tab/Enter/Backspace/方向), 符号仅 , . /
const KB_ROWS: KbKey[][] = [
  [key('Esc', 1, 1.3)].concat(letters(2, '1234567890')).concat([key('⌫', 14, 1.7)]),
  [key('Tab', 15, 1.5)].concat(letters(16, 'QWERTYUIOP')),
  [key('Ctrl', 29, 1.7, 2)].concat(letters(30, 'ASDFGHJKL')).concat([key('Enter', 28, 1.8)]),
  [key('Shift', 42, 1.9, 1)].concat(letters(44, 'ZXCVBNM')).concat([key(',', 51), key('.', 52), key('/', 53)]),
  [key('Alt', 56, 1.7, 4), key('空格', 57, 5.6), key('←', 105), key('↑', 103), key('↓', 108), key('→', 106)]
];

// 模拟键盘覆盖层: 悬浮条"键盘"开关控制显隐, 面板可拖动 (拖顶部把手),
// 位置持久化在 InputSettingsService。点按/长按键帽即注入 key down/up;
// Shift/Ctrl/Alt 为一次性锁存 — 点按点亮, 下一个普通键按下时先行注入
// 修饰键 down, 该键抬起后再补 up 并解除锁存 (与手机输入法 Shift 同款
// 语义, 避免忘松修饰键导致后续输入全带修饰)。容器 Transparent 穿透:
// 空白处触摸直达桌面层, 只有键帽/把手消费触摸。
@Component
export struct KeyboardOverlay {
  @State private kbEnabled: boolean = false;
  @State private relX: number = 0.5;
  @State private relY: number = 0.9;
  @State private pressedCodes: number[] = [];
  @State private modMask: number = 0;    // 锁存中的修饰键位掩码 (1/2/4)
  private scrW: number = 0;
  private scrH: number = 0;
  // 面板实测尺寸 — 必须是 @State: onAreaChange 量得后驱动 position 重算,
  // 普通字段不触发刷新, 首帧会按 0 宽渲染 (面板甩到屏幕中心右侧出界)
  @State private panelW: number = 0;
  @State private panelH: number = 0;
  // 普通键按下瞬间捕获的锁存掩码: 抬起时按此补 up (锁存视觉态按下时已清,
  // 若抬起再读 modMask 会漏发修饰键 up → 卡修饰键)
  private modsAtDown: Map<number, number> = new Map();
  // 拖动起手时的面板中心 (onActionUpdate 以此为基准累加位移)
  private panRelX: number = 0.5;
  private panRelY: number = 0.9;
  private input: InputSettingsService = InputSettingsService.getInstance();

  private mirrorInput = (): void => {
    this.kbEnabled = this.input.keyboardEnabled;
    this.relX = this.input.kbRelX;
    this.relY = this.input.kbRelY;
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

  private sendMods(mask: number, down: boolean): void {
    if ((mask & 1) != 0) sendKbKey(42, down);  // Shift
    if ((mask & 2) != 0) sendKbKey(29, down);  // Ctrl
    if ((mask & 4) != 0) sendKbKey(56, down);  // Alt
  }

  private onKeyTouch(k: KbKey, ev: TouchEvent): void {
    if (k.mod != 0) {
      // 修饰键: 仅翻转锁存态, 不发事件 (单独按 Shift 对 wine 无意义,
      // 且锁存语义保证不会出现"按下没抬起"的卡键)
      if (ev.type === TouchType.Down) {
        this.modMask = this.modMask ^ k.mod;
      }
      return;
    }
    if (ev.type === TouchType.Down) {
      const m = this.modMask;
      this.modsAtDown.set(k.code, m);
      this.sendMods(m, true);
      sendKbKey(k.code, true);
      this.modMask = 0;
      this.pressedCodes = this.pressedCodes.concat([k.code]);
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      if (this.pressedCodes.indexOf(k.code) < 0) {
        return;
      }
      sendKbKey(k.code, false);
      this.sendMods(this.modsAtDown.get(k.code) ?? 0, false);
      this.modsAtDown.delete(k.code);
      this.pressedCodes = this.pressedCodes.filter((c: number) => c != k.code);
    }
  }

  @Builder
  keyView($$: KeySpec) {
    Text($$.k.label)
      .fontSize($$.k.label.length > 1 ? 12 : 15)
      .fontColor(Color.White)
      .textAlign(TextAlign.Center)
      .width('100%')
      .height(42)
      .backgroundColor($$.pressed || $$.latched ? $r('app.color.accent') : '#1A000000')
      .borderRadius(8)
      .border({ width: 1, color: '#66FFFFFF' })
      .layoutWeight($$.k.w)
      .onTouch((ev: TouchEvent) => {
        this.onKeyTouch($$.k, ev);
      })
  }

  build() {
    Stack() {
      if (this.kbEnabled) {
        Column({ space: 6 }) {
          // 顶部把手: 拖动挪位。手势只挂把手 — 挂整个面板会和键帽 onTouch 抢事件
          // (按键微动即拖走面板)。拖动中直接改 relX/relY (position 的数据源),
          // 面板逐帧跟手; 不要绕 .offset 暂存位移 (实测拖动中不刷新, 抬起才跳变)。
          // 落盘仍在 onActionEnd — 拖动中 persist 是磁盘写风暴
          Row() {
            Row()
              .width(36)
              .height(4)
              .borderRadius(2)
              .backgroundColor('#88FFFFFF')
          }
          .width('100%')
          .height(18)
          .justifyContent(FlexAlign.Center)
          .gesture(
            PanGesture()
              .onActionStart(() => {
                this.panRelX = this.relX;
                this.panRelY = this.relY;
              })
              .onActionUpdate((ev: GestureEvent) => {
                this.relX = Math.min(0.96, Math.max(0.04,
                  (this.panRelX * this.scrW + ev.offsetX) / this.scrW));
                this.relY = Math.min(0.94, Math.max(0.06,
                  (this.panRelY * this.scrH + ev.offsetY) / this.scrH));
              })
              .onActionEnd(() => {
                this.input.updateKeyboardPos(this.relX, this.relY);
              })
          )
          ForEach(KB_ROWS, (row: KbKey[]) => {
            Row({ space: 4 }) {
              ForEach(row, (k: KbKey) => {
                this.keyView({
                  k: k,
                  pressed: this.pressedCodes.indexOf(k.code) >= 0,
                  latched: k.mod != 0 && (this.modMask & k.mod) != 0
                })
              }, (k: KbKey) => k.code.toString())
            }
            .width('100%')
          }, (row: KbKey[], idx: number) => idx.toString())
        }
        .width(Math.min(this.scrW * 0.94, 860))
        .padding({ left: 8, right: 8, top: 2, bottom: 8 })
        // 半透明面板: 压低不透明度让桌面/游戏画面透出来 (键帽同理)
        .backgroundColor('#4D000000')
        .borderRadius(12)
        // 面板区域必须阻断下层: 容器是 Transparent (自身和子节点均响应, 且不阻塞
        // 下层 — 实测键帽 onTouch 触发的同时, 同一击仍穿透成桌面左键点击
        // [ArkTS] TAP->LEFTCLICK)。注意不能用 Block (它会阻塞子节点触摸测试,
        // 键帽全哑); BLOCK_HIERARCHY = 自身+子节点响应, 阻止下层兄弟/父节点参与
        .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
        .position({
          // 中心坐标钳制在屏幕内: 持久化位置 + 不同屏尺寸时仍保证整面板可见
          x: Math.min(Math.max(this.relX, this.panelW / 2 / this.scrW),
            1 - this.panelW / 2 / this.scrW) * this.scrW - this.panelW / 2,
          y: Math.min(Math.max(this.relY, this.panelH / 2 / this.scrH),
            1 - this.panelH / 2 / this.scrH) * this.scrH - this.panelH / 2
        })
        .onAreaChange((_oldValue: Area, newValue: Area) => {
          this.panelW = newValue.width as number;
          this.panelH = newValue.height as number;
        })
      }
    }
    .width('100%')
    .height('100%')
    .hitTestBehavior(HitTestMode.Transparent)
  }
}

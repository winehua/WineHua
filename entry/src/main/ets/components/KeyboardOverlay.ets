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
  shiftLabel: string; // Shift 锁存时键帽切换显示的副符号 ('' = 无副符号键)
}

// keyView 入参。必须整体作为单个对象字面量传给 @Builder (按引用传递),
// pressed/latched 变化才会刷新键帽配色 — 拆成基本类型参数会被快照冻结
// (同 FloatingBar barButton 踩过的坑)
interface KeySpec {
  k: KbKey;
  pressed: boolean;   // 正被按住
  latched: boolean;   // 修饰键锁存中
  shiftActive: boolean; // Shift 锁存中 → 键帽显示副符号 (shiftLabel)
}

function key(label: string, code: number, w: number = 1, mod: number = 0, shiftLabel: string = ''): KbKey {
  return { label: label, code: code, w: w, mod: mod, shiftLabel: shiftLabel };
}

function letters(start: number, labels: string): KbKey[] {
  const out: KbKey[] = [];
  for (let i = 0; i < labels.length; i++) {
    out.push(key(labels[i], start + i));
  }
  return out;
}

// 标准 104 键紧凑布局 (evdev code): F 行 (Esc 同行) + 符号数字行 + 三行字母 +
// 编辑/方向行。覆盖桌面高频键 (Esc/F1-F12 / Tab / Enter / Backspace /
// Home-End-PgUp-PgDn / Del / 全方向); 带 Shift 组合符号的键 (数字行/标点)
// 键帽平时只显示主字符, Shift 锁存时切换显示副符号 (1→!, ;→:), 按下即打出
// 副符号 (由 wine keymap 解析, 与真实键盘同语义)。
const KB_ROWS: KbKey[][] = [
  [key('Esc', 1, 1.3),
   key('F1', 59), key('F2', 60), key('F3', 61), key('F4', 62),
   key('F5', 63), key('F6', 64), key('F7', 65), key('F8', 66),
   key('F9', 67), key('F10', 68), key('F11', 69), key('F12', 70),
   key('Del', 111, 1.1)],
  // 数字行: 主字符 + shiftLabel 组合符号 (shift 后 = !@#$%^&*()_-+)
  [key('`', 41, 0.9, 0, '~'),
   key('1', 2, 1, 0, '!'), key('2', 3, 1, 0, '@'), key('3', 4, 1, 0, '#'),
   key('4', 5, 1, 0, '$'), key('5', 6, 1, 0, '%'), key('6', 7, 1, 0, '^'),
   key('7', 8, 1, 0, '&'), key('8', 9, 1, 0, '*'), key('9', 10, 1, 0, '('),
   key('0', 11, 1, 0, ')'),
   key('-', 12, 1, 0, '_'), key('=', 13, 1, 0, '+'), key('⌫', 14, 1.4)],
  [key('Tab', 15, 1.5)].concat(letters(16, 'QWERTYUIOP'))
    .concat([key('[', 26, 1, 0, '{'), key(']', 27, 1, 0, '}'), key('\\', 43, 1.05, 0, '|')]),
  [key('Ctrl', 29, 1.7, 2)].concat(letters(30, 'ASDFGHJKL'))
    .concat([key(';', 39, 1.05, 0, ':'), key("'", 40, 1.05, 0, '"'), key('Enter', 28, 1.8)]),
  [key('Shift', 42, 1.9, 1)].concat(letters(44, 'ZXCVBNM'))
    .concat([key(',', 51, 1, 0, '<'), key('.', 52, 1, 0, '>'), key('/', 53, 1, 0, '?'),
      key('↑', 103, 1.1), key('Shift', 54, 1.5)]),
  [key('Alt', 56, 1.5, 4), key('空格', 57, 3.2),
   key('Home', 102, 1.1), key('End', 107, 1.1),
   key('PgUp', 104, 1.1), key('PgDn', 109, 1.1),
   key('←', 105, 1.1), key('↓', 108, 1.1), key('→', 106, 1.1)]
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

  private keyLabel(k: KbKey, shiftActive: boolean): string {
    return shiftActive && k.shiftLabel ? k.shiftLabel : k.label;
  }

  @Builder
  keyView($$: KeySpec) {
    Text(this.keyLabel($$.k, $$.shiftActive))
      .fontSize(this.keyLabel($$.k, $$.shiftActive).length > 1 ? 12 : 15)
      .fontColor(Color.White)
      .textAlign(TextAlign.Center)
      .width('100%')
      .height(38)
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
        Column({ space: 4 }) {
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
                  latched: k.mod != 0 && (this.modMask & k.mod) != 0,
                  shiftActive: (this.modMask & 1) != 0
                })
              }, (k: KbKey) => k.code.toString())
            }
            .width('100%')
          }, (row: KbKey[], idx: number) => idx.toString())
        }
        .width(Math.min(this.scrW * 0.9, 810))
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

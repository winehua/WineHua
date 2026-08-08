import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const STORE_NAME = 'input_settings';
const KEY_TOUCHPAD = 'touchpadEnabled';
const KEY_GAMEPAD = 'gamepadEnabled';
const KEY_KEYBOARD = 'keyboardEnabled';
const KEY_SPEED = 'cursorSpeed';
const KEY_BUTTONS = 'padButtons';
const KEY_ZONES = 'padZones';
const KEY_KB_POS = 'keyboardPos';
const KEY_INTRO = 'layoutEditIntroShown';

// 手柄可改键按钮 (方向键区/摇杆键位固定在 GamepadOverlay: 方向键/WASD,
// v1 不开放重映射 — "逐键配置"覆盖 8 个动作键已满足主流 RPG/RTS)
export interface PadButton {
  id: string;    // 'A'|'B'|'X'|'Y'|'LB'|'RB'|'LT'|'RT'
  label: string;
  relX: number;  // 中心相对屏宽 0-1
  relY: number;  // 中心相对屏高 0-1
  key: number;   // evdev key code (注入走 sendKeyEvent, 与物理键盘同通道)
}

// dpad / stick 只可挪位置 (键位固定)
export interface PadZone {
  id: string;    // 'dpad' | 'stick'
  relX: number;
  relY: number;
}

// 布局快照 (进入编辑页时深拷贝, 取消编辑时恢复); 显式类型规避
// arkts-no-obj-literals-as-types
export interface LayoutSnapshot {
  buttons: PadButton[];
  zones: PadZone[];
}

// 重映射候选键 (evdev code): 覆盖常用确认/取消/技能/方向键
export interface KeyDef {
  label: string;
  code: number;
}

export const KEY_CATALOG: KeyDef[] = [
  { label: 'Enter', code: 28 }, { label: 'Esc', code: 1 },
  { label: 'Space', code: 57 }, { label: 'Tab', code: 15 },
  { label: 'Shift', code: 42 }, { label: 'Ctrl', code: 29 }, { label: 'Alt', code: 56 },
  { label: 'W', code: 17 }, { label: 'A', code: 30 }, { label: 'S', code: 31 }, { label: 'D', code: 32 },
  { label: 'Q', code: 16 }, { label: 'E', code: 18 }, { label: 'R', code: 19 }, { label: 'F', code: 33 },
  { label: 'Z', code: 44 }, { label: 'X', code: 45 }, { label: 'C', code: 46 }, { label: 'V', code: 47 },
  { label: '↑', code: 103 }, { label: '↓', code: 108 }, { label: '←', code: 105 }, { label: '→', code: 106 },
  { label: '1', code: 2 }, { label: '2', code: 3 }, { label: '3', code: 4 }, { label: '4', code: 5 }
];

// 默认布局 (横屏相对坐标): 十字键/摇杆在左下, XYAB 菱形在右下, 肩键顶角
function defaultButtons(): PadButton[] {
  return [
    { id: 'Y', label: 'Y', relX: 0.855, relY: 0.70, key: 15 },  // Tab
    { id: 'X', label: 'X', relX: 0.900, relY: 0.60, key: 57 },  // Space
    { id: 'A', label: 'A', relX: 0.945, relY: 0.70, key: 28 },  // Enter
    { id: 'B', label: 'B', relX: 0.900, relY: 0.80, key: 1 },   // Esc
    { id: 'LB', label: 'LB', relX: 0.075, relY: 0.14, key: 16 }, // Q
    { id: 'RB', label: 'RB', relX: 0.870, relY: 0.14, key: 18 }, // E
    { id: 'LT', label: 'LT', relX: 0.075, relY: 0.05, key: 42 }, // Shift
    { id: 'RT', label: 'RT', relX: 0.870, relY: 0.05, key: 29 }  // Ctrl
  ];
}

function defaultZones(): PadZone[] {
  return [
    { id: 'dpad', relX: 0.10, relY: 0.66 },
    { id: 'stick', relX: 0.25, relY: 0.80 }
  ];
}

// 输入设置 (全局, 不做单应用覆盖 — 用户决策): 触摸模拟鼠标 / 游戏手柄模拟 /
// 模拟键盘开关、光标速度、手柄布局与按键映射、键盘面板位置。preferences 持久化;
// 悬浮条开关、DesktopLayer 触控路由、GamepadOverlay / KeyboardOverlay 均经
// subscribe 镜像。editingLayout 为全屏布局编辑页瞬态标记 (打开期间控件变为可拖动
// /可改键), 不持久化; 与 gamepadEnabled 解耦 — 编辑页不需要先开手柄开关。
// 进入编辑页时拍 buttons/zones 快照 (layoutSnapshot), "取消"恢复快照丢改动。
export class InputSettingsService {
  private static instance: InputSettingsService | null = null;

  static getInstance(): InputSettingsService {
    if (!InputSettingsService.instance) {
      InputSettingsService.instance = new InputSettingsService();
    }
    return InputSettingsService.instance;
  }

  touchpadEnabled: boolean = false;
  gamepadEnabled: boolean = false;
  // 模拟键盘 (KeyboardOverlay): 开关 + 面板中心相对屏坐标 (持久化)
  keyboardEnabled: boolean = false;
  kbRelX: number = 0.5;
  kbRelY: number = 0.9;
  cursorSpeed: number = 1.0;
  buttons: PadButton[] = defaultButtons();
  zones: PadZone[] = defaultZones();
  // 全屏布局编辑页开关 (瞬态, 不持久化); 与 gamepadEnabled 解耦
  editingLayout: boolean = false;
  // 进入编辑页后是否改过布局 (驱动"取消"是否需要确认弹窗)
  layoutDirty: boolean = false;
  // 首用引导是否看过 (持久化)
  introShown: boolean = false;

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;
  private listeners: Array<() => void> = [];
  // 进入编辑页时的布局快照 (取消编辑时恢复), 编辑中不更新
  private layoutSnapshot: LayoutSnapshot | null = null;

  subscribe(listener: () => void): void {
    if (this.listeners.indexOf(listener) >= 0) {
      return;
    }
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      this.touchpadEnabled = this.store.getSync(KEY_TOUCHPAD, false) as boolean;
      this.gamepadEnabled = this.store.getSync(KEY_GAMEPAD, false) as boolean;
      this.keyboardEnabled = this.store.getSync(KEY_KEYBOARD, false) as boolean;
      this.introShown = this.store.getSync(KEY_INTRO, false) as boolean;
      this.cursorSpeed = this.store.getSync(KEY_SPEED, 1.0) as number;
      const kbPosJson = this.store.getSync(KEY_KB_POS, '') as string;
      if (kbPosJson) {
        const pos = JSON.parse(kbPosJson) as Record<string, number>;
        if (typeof pos['relX'] === 'number' && typeof pos['relY'] === 'number') {
          this.kbRelX = pos['relX'];
          this.kbRelY = pos['relY'];
        }
      }
      const buttonsJson = this.store.getSync(KEY_BUTTONS, '') as string;
      if (buttonsJson) {
        const list = JSON.parse(buttonsJson) as PadButton[];
        if (Array.isArray(list) && list.length > 0) {
          this.buttons = list;
        }
      }
      const zonesJson = this.store.getSync(KEY_ZONES, '') as string;
      if (zonesJson) {
        const list = JSON.parse(zonesJson) as PadZone[];
        if (Array.isArray(list) && list.length > 0) {
          this.zones = list;
        }
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'input settings load fail: %{public}s', JSON.stringify(e));
    }
  }

  private persist(): void {
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_TOUCHPAD, this.touchpadEnabled);
      this.store.putSync(KEY_GAMEPAD, this.gamepadEnabled);
      this.store.putSync(KEY_KEYBOARD, this.keyboardEnabled);
      this.store.putSync(KEY_SPEED, this.cursorSpeed);
      this.store.putSync(KEY_BUTTONS, JSON.stringify(this.buttons));
      this.store.putSync(KEY_ZONES, JSON.stringify(this.zones));
      this.store.putSync(KEY_KB_POS, JSON.stringify({ relX: this.kbRelX, relY: this.kbRelY }));
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'input settings persist fail: %{public}s', JSON.stringify(e));
    }
  }

  setTouchpad(enabled: boolean): void {
    this.touchpadEnabled = enabled;
    this.persist();
    this.notify();
  }

  setGamepad(enabled: boolean): void {
    this.gamepadEnabled = enabled;
    // 不再强退布局编辑态: 编辑页是全屏模态 (盖住悬浮条), 不受覆盖层开关门控
    this.persist();
    this.notify();
  }

  setKeyboard(enabled: boolean): void {
    this.keyboardEnabled = enabled;
    this.persist();
    this.notify();
  }

  updateKeyboardPos(relX: number, relY: number): void {
    this.kbRelX = relX;
    this.kbRelY = relY;
    this.persist();
    this.notify();
  }

  // persist=false 用于滑条拖动中 (onChange Moving 每帧触发, 逐帧 flush 是磁盘写风暴);
  // 字段+notify 始终更新 — DesktopLayer 镜像的光标速度拖动中立竿见影
  setCursorSpeed(speed: number, persist: boolean = true): void {
    this.cursorSpeed = Math.min(2.5, Math.max(0.3, speed));
    if (persist) {
      this.persist();
    }
    this.notify();
  }

  // 进入全屏布局编辑页: 拍快照 (取消时恢复), 置脏标记与编辑态。不依赖手柄开关。
  beginLayoutEdit(): void {
    this.layoutSnapshot = {
      buttons: JSON.parse(JSON.stringify(this.buttons)) as PadButton[],
      zones: JSON.parse(JSON.stringify(this.zones)) as PadZone[]
    };
    this.layoutDirty = false;
    this.editingLayout = true;
    this.notify();
  }

  // 完成编辑: 改动已即改即存, 仅关编辑态并清快照
  finishLayoutEdit(): void {
    this.editingLayout = false;
    this.layoutSnapshot = null;
    this.notify();
  }

  // 取消编辑: 恢复进入时的快照 (必须 persist, 否则重启读到改动值), 关编辑态
  cancelLayoutEdit(): void {
    if (this.layoutSnapshot) {
      this.buttons = this.layoutSnapshot.buttons;
      this.zones = this.layoutSnapshot.zones;
      this.persist();
    }
    this.editingLayout = false;
    this.layoutSnapshot = null;
    this.notify();
  }

  // 冲突检测: 返回占用指定键的其他按钮 id (排除自身), 无则空串
  findKeyOwner(code: number, excludeId: string): string {
    const hit = this.buttons.find((b: PadButton) => b.key == code && b.id != excludeId);
    return hit ? hit.id : '';
  }

  markIntroShown(): void {
    this.introShown = true;
    if (this.store) {
      try {
        this.store.putSync(KEY_INTRO, true);
        this.store.flush();
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'input settings intro persist fail: %{public}s', JSON.stringify(e));
      }
    }
  }

  updateButtonPos(id: string, relX: number, relY: number): void {
    this.layoutDirty = true;
    this.buttons = this.buttons.map((b: PadButton): PadButton =>
      b.id == id ? { id: b.id, label: b.label, relX: relX, relY: relY, key: b.key } : b);
    this.persist();
    this.notify();
  }

  updateButtonKey(id: string, key: number): void {
    this.layoutDirty = true;
    this.buttons = this.buttons.map((b: PadButton): PadButton =>
      b.id == id ? { id: b.id, label: b.label, relX: b.relX, relY: b.relY, key: key } : b);
    this.persist();
    this.notify();
  }

  updateZonePos(id: string, relX: number, relY: number): void {
    this.layoutDirty = true;
    this.zones = this.zones.map((z: PadZone): PadZone =>
      z.id == id ? { id: z.id, relX: relX, relY: relY } : z);
    this.persist();
    this.notify();
  }

  resetLayout(): void {
    this.layoutDirty = true;
    this.buttons = defaultButtons();
    this.zones = defaultZones();
    this.persist();
    this.notify();
  }

  keyLabelOf(code: number): string {
    const hit = KEY_CATALOG.find((k: KeyDef) => k.code == code);
    return hit ? hit.label : `键${code}`;
  }
}

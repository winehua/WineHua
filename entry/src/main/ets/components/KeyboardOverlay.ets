import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { InputSettingsService, KeyOp, KeyDef, PadZone, KEY_CATALOG,
  MOD_SHIFT, MOD_CTRL, MOD_ALT, MOD_META, keyOpLabel } from '../service/InputSettingsService';
import { WineWindowManager } from '../service/WineWindowManager';
import { TouchKeyRouter } from '../common/TouchKeyRouter';
import { TouchMouseRouter } from '../common/TouchMouseRouter';
import { DesktopPointerService, sendDesktopPointerEvent, desktopKeyboardTarget } from '../service/DesktopPointerService';
import { TouchControlView } from './TouchControlView';

@Component
export struct KeyboardOverlay {
  @Prop targetId: number = 0;
  @State private controlsEnabled: boolean = false;
  @State private touchpadOn: boolean = false;
  @State private open: boolean = false;
  @State private editing: boolean = false;
  @State private catalog: boolean = false;
  @State private ops: KeyOp[] = [];
  @State private zones: PadZone[] = [];
  @State private widthVp: number = 640;
  @State private heightVp: number = 360;
  @State private inputEpoch: number = 0;
  @State private modifiers: number = 0;
  private settings: InputSettingsService = InputSettingsService.getInstance();
  private keys: TouchKeyRouter = new TouchKeyRouter((target: number, code: number, down: boolean) => {
    try { testNapi.sendKeyEvent(target, code, down); }
    catch (e) { hilog.warn(0, 'TouchControls', 'key dispatch failed: %{public}s', JSON.stringify(e)); }
  });
  private mouse: TouchMouseRouter = new TouchMouseRouter((target: number, button: number, down: boolean) => {
    const point = DesktopPointerService.getInstance();
    try {
      if (down) testNapi.raiseToplevel(target);
      sendDesktopPointerEvent(target, down ? MouseAction.Press : MouseAction.Release,
        Math.round(point.x), Math.round(point.y), button);
    }
    catch (e) { hilog.warn(0, 'TouchControls', 'mouse dispatch failed: %{public}s', JSON.stringify(e)); }
  });

  aboutToAppear(): void { this.settings.subscribe(this.syncSettings); this.syncSettings(); }
  aboutToDisappear(): void { this.settings.unsubscribe(this.syncSettings); this.releaseInput(); }

  private syncSettings = (): void => {
    if (this.touchpadOn !== this.settings.touchpadEnabled) this.releaseInput();
    this.touchpadOn = this.settings.touchpadEnabled;
    if (!this.settings.keyboardEnabled) this.releaseInput();
    this.controlsEnabled = this.settings.keyboardEnabled;
    this.ops = this.settings.keyOps.map((op: KeyOp, index: number): KeyOp => ({
      id: op.id, label: op.label, key: op.key, mods: op.mods,
      mouseButton: op.mouseButton,
      relX: op.relX ?? (0.5 + Math.floor(index / 4) % 4 * 0.14),
      relY: op.relY ?? (0.26 + index % 4 * 0.18)
    }));
    this.zones = this.settings.zones.map((zone: PadZone): PadZone => ({
      id: zone.id, relX: zone.relX, relY: zone.relY, keyMode: zone.keyMode
    }));
  };

  private releaseInput(): void { this.keys.releaseAll(); this.mouse.releaseAll(); this.inputEpoch++; }
  private target(): number {
    return this.targetId > 0 ? this.targetId : desktopKeyboardTarget(WineWindowManager.getInstance().getDesktopRootId());
  }
  private routeKeys = (source: string, codes: number[]): void => {
    this.keys.set(source, this.target(), this.editing ? [] : codes);
  };
  private routeMouse = (source: string, button: number, down: boolean): void => {
    const point = DesktopPointerService.getInstance();
    const target = this.targetId > 0 ? this.targetId :
      (testNapi.findToplevelAt(Math.round(point.x), Math.round(point.y)) as number);
    this.mouse.set(source, target, button, down && !this.editing);
  };
  private savePosition = (id: string, x: number, y: number): void => {
    if (id.startsWith('key:')) this.settings.updateKeyOpPos(id.substring(4), x, y);
    else this.settings.updateZonePos(id, x, y);
  };
  private removeControl = (id: string): void => {
    this.keys.releasePrefix(id);
    this.mouse.releasePrefix(id);
    if (id.startsWith('key:')) this.settings.removeKeyOp(id.substring(4));
    else this.settings.removeZone(id);
  };
  private toggleMapping = (id: string): void => {
    this.releaseInput();
    const zone = this.zones.find((item: PadZone) => item.id === id);
    if (zone) this.settings.updateZoneKeyMode(id, this.usesArrows(zone) ? 'wasd' : 'arrows');
  };
  private usesArrows(zone: PadZone): boolean {
    return (zone.keyMode ?? (zone.id === 'dpad' ? 'arrows' : 'wasd')) === 'arrows';
  }
  private setEditing(value: boolean): void {
    this.releaseInput(); this.editing = value; this.catalog = false;
  }
  private close(): void { this.setEditing(false); this.open = false; }
  private addKey(def: KeyDef): void {
    this.settings.addKeyOp(keyOpLabel(def.label, this.modifiers), def.code, this.modifiers, 0.65, 0.5);
    this.modifiers = 0;
  }

  @Builder
  modifier(label: string, bit: number) {
    Text(label).fontSize(11).fontColor('#ffffff')
      .backgroundColor(this.modifiers & bit ? '#42668d' : '#343b46')
      .padding(5).borderRadius(5)
      .onClick(() => { this.modifiers ^= bit; })
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (this.controlsEnabled && this.open) {
        ForEach(this.ops, (op: KeyOp) => {
          TouchControlView({
            controlId: `key:${op.id}`, kind: 'key', label: op.label, code: op.key, mods: op.mods,
            mouseButton: op.mouseButton ?? 0,
            relX: op.relX ?? 0.8, relY: op.relY ?? 0.7,
            viewportWidth: this.widthVp, viewportHeight: this.heightVp,
            editing: this.editing, inputEpoch: this.inputEpoch,
            onKeys: this.routeKeys, onMouseButton: this.routeMouse,
            onPosition: this.savePosition, onRemove: this.removeControl
          })
        // ArkUI ForEach keeps the old item closure when only the array object
        // changes under the same key. Position is committed only on touch-up:
        // include persisted values so the view immediately receives that commit.
        // Live MOVE geometry stays in TouchControlView and never changes this key.
        }, (op: KeyOp) => `${op.id}:${op.relX}:${op.relY}:${op.key}:${op.mods}:${op.mouseButton}:${op.label}`)
        ForEach(this.zones, (zone: PadZone) => {
          TouchControlView({
            controlId: zone.id, kind: zone.id, arrows: this.usesArrows(zone),
            relX: zone.relX, relY: zone.relY,
            viewportWidth: this.widthVp, viewportHeight: this.heightVp,
            editing: this.editing, inputEpoch: this.inputEpoch,
            onKeys: this.routeKeys, onPosition: this.savePosition, onRemove: this.removeControl,
            onMapping: this.toggleMapping
          })
        }, (zone: PadZone) => `${zone.id}:${zone.relX}:${zone.relY}:${zone.keyMode}`)

        Column({ space: 5 }) {
          Flex({ wrap: FlexWrap.Wrap }) {
            Text(this.editing ? '完成编辑' : '布局编辑').id('touch-layout-edit')
              .fontSize(12).fontColor(this.editing ? '#102019' : '#ffffff')
              .padding(7).borderRadius(5).margin(2)
              .backgroundColor(this.editing ? '#8fe3b0' : '#343b46')
              .onClick(() => this.setEditing(!this.editing))
            if (this.targetId === 0) {
              Text(this.touchpadOn ? '鼠标：相对手指' : '鼠标：紧贴手指').id('touch-mouse-mode')
                .fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                .backgroundColor(this.touchpadOn ? '#42668d' : '#343b46')
                .onClick(() => this.settings.setTouchpad(!this.touchpadOn))
            }
            if (this.editing) {
              Text('＋左键').id('touch-layout-add-left').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                .backgroundColor('#343b46').onClick(() => this.settings.addMouseOp('左键', 0x110, 0.60, 0.88))
              Text('＋中键').id('touch-layout-add-middle').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                .backgroundColor('#343b46').onClick(() => this.settings.addMouseOp('中键', 0x112, 0.68, 0.88))
              Text('＋右键').id('touch-layout-add-right').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                .backgroundColor('#343b46').onClick(() => this.settings.addMouseOp('右键', 0x111, 0.76, 0.88))
              Text('＋单键').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                .backgroundColor('#343b46').onClick(() => { this.catalog = !this.catalog; })
              if (!this.settings.hasZone('stick')) {
                Text('＋摇杆').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                  .backgroundColor('#343b46').onClick(() => this.settings.addZone('stick', 0.17, 0.65))
              }
              if (!this.settings.hasZone('dpad')) {
                Text('＋方向键').fontSize(12).fontColor('#ffffff').padding(7).borderRadius(5).margin(2)
                  .backgroundColor('#343b46').onClick(() => this.settings.addZone('dpad', 0.4, 0.65))
              }
              Text('恢复默认').fontSize(12).fontColor('#f4c27a').padding(7).borderRadius(5).margin(2)
                .backgroundColor('#343b46')
                .onClick(() => { this.releaseInput(); this.settings.resetGameControls(); })
            }
            Text('收起').id('touch-layout-close').fontSize(12).fontColor('#ffffff')
              .padding(7).borderRadius(5).margin(2).backgroundColor('#343b46')
              .onClick(() => this.close())
          }
          .width('100%')
          if (this.editing && this.catalog) {
            Row({ space: 4 }) {
              this.modifier('Shift', MOD_SHIFT)
              this.modifier('Ctrl', MOD_CTRL)
              this.modifier('Alt', MOD_ALT)
              this.modifier('Meta', MOD_META)
            }
            Scroll() {
              Flex({ wrap: FlexWrap.Wrap }) {
                ForEach(KEY_CATALOG, (def: KeyDef) => {
                  Text(def.label).fontSize(11).fontColor('#ffffff').width(48).height(32)
                    .textAlign(TextAlign.Center).backgroundColor('#cc303844')
                    .borderRadius(5).margin(2).onClick(() => this.addKey(def))
                }, (def: KeyDef) => `${def.code}`)
              }
            }
            .width('100%').height(Math.min(112, this.heightVp * 0.35))
          }
        }
        .width(Math.min(this.editing ? 420 : 310, this.widthVp - 12))
        .padding(4).backgroundColor('#d91b2028').borderRadius(9)
        .position({ x: 6, y: 6 })
        .onTouch((event: TouchEvent) => event.stopPropagation())
      }
      if (this.controlsEnabled && !this.open) {
        Text('⌨').id('touch-layout-open').fontSize(20).fontColor('#ffffff')
          .width(48).height(48).textAlign(TextAlign.Center).backgroundColor('#bb202936')
          .borderRadius(24).border({ width: 1, color: '#bbffffff' })
          .position({ x: Math.max(4, this.widthVp - 56), y: Math.max(4, this.heightVp - 56) })
          .hitTestBehavior(HitTestMode.Block)
          .onTouch((event: TouchEvent) => event.stopPropagation())
          .onClick(() => { this.open = true; })
      }
    }
    // Only actual controls block the XComponent; empty overlay space is pass-through.
    .width('100%').height('100%').hitTestBehavior(HitTestMode.Transparent)
    .onAreaChange((_old: Area, area: Area) => {
      const width = Number(area.width);
      const height = Number(area.height);
      if (width > 0 && height > 0 && (width !== this.widthVp || height !== this.heightVp)) {
        this.releaseInput(); this.widthVp = width; this.heightVp = height;
      }
    })
    .onVisibleAreaChange([0], (visible: boolean) => { if (!visible) this.releaseInput(); })
  }
}

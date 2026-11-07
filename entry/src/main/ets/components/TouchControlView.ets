import { hilog } from '@kit.PerformanceAnalysisKit';
import { clampControlCenter, sampleKeyboardStick } from '../common/TouchControlMath';
import { MinimumKeyHold } from '../common/MinimumKeyHold';

// Each control owns its live geometry. ForEach reuses children by stable ID;
// builder arguments must not be used as the mutable position of an active drag.
@Component
export struct TouchControlView {
  @Prop controlId: string = '';
  @Prop kind: string = 'key';
  @Prop label: string = '';
  @Prop code: number = 0;
  @Prop mods: number = 0;
  @Prop mouseButton: number = 0;
  @Prop arrows: boolean = false;
  @Prop @Watch('syncPosition') relX: number = 0.5;
  @Prop @Watch('syncPosition') relY: number = 0.5;
  @Prop @Watch('syncPosition') viewportWidth: number = 640;
  @Prop @Watch('syncPosition') viewportHeight: number = 360;
  @Prop @Watch('modeChanged') editing: boolean = false;
  @Prop @Watch('cancelInput') inputEpoch: number = 0;
  onPosition: (id: string, x: number, y: number) => void = () => {};
  onKeys: (id: string, codes: number[]) => void = () => {};
  onMouseButton: (id: string, button: number, down: boolean) => void = () => {};
  onRemove: (id: string) => void = () => {};
  onMapping: (id: string) => void = () => {};

  @State private centerX: number = 0;
  @State private centerY: number = 0;
  @State private thumbX: number = 0;
  @State private thumbY: number = 0;
  @State private pressed: boolean = false;
  private dragging: boolean = false;
  private finger: number = -1;
  private startX: number = 0;
  private startY: number = 0;
  private originX: number = 0;
  private originY: number = 0;
  private keyHold: MinimumKeyHold = new MinimumKeyHold(100);

  private controlSize(): number { return this.kind === 'stick' ? 144 : (this.kind === 'dpad' ? 130 : 56); }

  aboutToAppear(): void { this.syncPosition(); }
  aboutToDisappear(): void { this.cancelInput(); }

  private syncPosition(): void {
    if (this.dragging) return;
    const point = clampControlCenter(this.relX * this.viewportWidth, this.relY * this.viewportHeight,
      this.controlSize(), this.viewportWidth, this.viewportHeight);
    this.centerX = point.x;
    this.centerY = point.y;
  }

  private cancelInput(): void {
    this.keyHold.cancel();
    this.onKeys(this.controlId, []);
    this.onMouseButton(this.controlId, this.mouseButton, false);
    this.finger = -1;
    this.thumbX = 0;
    this.thumbY = 0;
    this.pressed = false;
    if (this.dragging) {
      this.dragging = false;
      this.syncPosition();
    }
  }

  private modeChanged(): void { this.cancelInput(); this.syncPosition(); }

  private changedTouch(ev: TouchEvent): TouchObject | undefined {
    return ev.changedTouches.find((touch: TouchObject) => this.finger < 0 || touch.id === this.finger) ??
      ev.touches.find((touch: TouchObject) => this.finger < 0 || touch.id === this.finger);
  }

  private dragTouch(ev: TouchEvent): void {
    ev.stopPropagation();
    if (ev.type === TouchType.Up && this.finger >= 0 &&
        !ev.changedTouches.some((touch: TouchObject) => touch.id === this.finger)) return;
    const touch = this.changedTouch(ev);
    if (ev.type === TouchType.Down) {
      if (this.finger >= 0 || !touch) return;
      this.finger = touch.id;
      this.dragging = true;
      this.startX = touch.windowX;
      this.startY = touch.windowY;
      this.originX = this.centerX;
      this.originY = this.centerY;
      return;
    }
    if (!this.dragging) return;
    if (ev.type === TouchType.Cancel) { this.cancelInput(); return; }
    if (touch && (ev.type === TouchType.Move || ev.type === TouchType.Up)) {
      const point = clampControlCenter(this.originX + touch.windowX - this.startX,
        this.originY + touch.windowY - this.startY, this.controlSize(), this.viewportWidth, this.viewportHeight);
      this.centerX = point.x;
      this.centerY = point.y;
    }
    if (ev.type === TouchType.Up) {
      this.dragging = false;
      this.finger = -1;
      this.onPosition(this.controlId, this.centerX / this.viewportWidth, this.centerY / this.viewportHeight);
      hilog.info(0, 'TouchControls', 'saved %{public}s center=%{public}d,%{public}d',
        this.controlId, Math.round(this.centerX), Math.round(this.centerY));
    }
  }

  private playTouch(ev: TouchEvent): void {
    ev.stopPropagation();
    if (ev.type === TouchType.Cancel) { this.cancelInput(); return; }
    if (ev.type === TouchType.Up && this.finger >= 0 &&
        !ev.changedTouches.some((touch: TouchObject) => touch.id === this.finger)) return;
    const touch = this.changedTouch(ev);
    if (ev.type === TouchType.Down) {
      if (this.finger >= 0 || !touch) return;
      if (this.keyHold.pending) this.cancelInput();
      this.finger = touch.id;
      this.keyHold.begin();
    }
    if (this.finger < 0) return;
    if (ev.type === TouchType.Up) {
      this.finger = -1;
      if (this.kind === 'key' && this.mouseButton <= 0 && this.pressed)
        this.keyHold.end(() => this.cancelInput());
      else this.cancelInput();
      return;
    }
    if (!touch) return;
    if (this.kind === 'stick') {
      const sample = sampleKeyboardStick(touch.x, touch.y, this.controlSize(), this.arrows);
      this.thumbX = sample.x;
      this.thumbY = sample.y;
      this.onKeys(this.controlId, sample.codes);
    } else if (ev.type === TouchType.Down) {
      if (this.mouseButton > 0) {
        this.onMouseButton(this.controlId, this.mouseButton, true);
        this.pressed = true;
        return;
      }
      const codes: number[] = [];
      if (this.mods & 1) codes.push(42);
      if (this.mods & 2) codes.push(29);
      if (this.mods & 4) codes.push(56);
      if (this.mods & 8) codes.push(125);
      codes.push(this.code);
      this.onKeys(this.controlId, codes);
      this.pressed = true;
    }
  }

  @Builder
  stickVisual() {
    Circle().width(144).height(144).fill('#55252c37').stroke('#aaffffff').strokeWidth(2)
      .hitTestBehavior(HitTestMode.None)
    Circle().width(68).height(68).fill('#99708299').stroke('#ccffffff').strokeWidth(1)
      .offset({ x: this.thumbX, y: this.thumbY }).hitTestBehavior(HitTestMode.None)
    Text(this.arrows ? '↑↓←→' : 'WASD').fontSize(10).fontColor('#ffffff')
      .offset({ x: this.thumbX, y: this.thumbY }).hitTestBehavior(HitTestMode.None)
  }

  @Builder
  arrowVisual(label: string, x: number, y: number) {
    Text(label).width(42).height(42).textAlign(TextAlign.Center)
      .fontSize(18).fontColor('#ffffff').backgroundColor('#b3323946')
      .borderRadius(5).border({ width: 1, color: '#aaffffff' })
      .position({ x: x, y: y }).hitTestBehavior(HitTestMode.None)
  }

  @Builder
  arrowButton(label: string, code: number, x: number, y: number) {
    Text(label).width(42).height(42).textAlign(TextAlign.Center)
      .fontSize(18).fontColor('#ffffff').backgroundColor('#b3323946')
      .borderRadius(5).border({ width: 1, color: '#aaffffff' })
      .position({ x: x, y: y })
      .onTouch((ev: TouchEvent) => {
        ev.stopPropagation();
        const source = `${this.controlId}:${code}`;
        if (ev.type === TouchType.Down) this.onKeys(source, [code]);
        else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) this.onKeys(source, []);
      })
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (this.editing) {
        // A single stable editing surface owns the whole gesture. Decorative
        // children never participate in hit testing or feed duplicate drags.
        Stack({ alignContent: Alignment.Center }) {
          if (this.kind === 'stick') {
            this.stickVisual()
          } else if (this.kind === 'dpad') {
            this.arrowVisual('↑', 44, 0)
            this.arrowVisual('←', 0, 44)
            this.arrowVisual('→', 88, 44)
            this.arrowVisual('↓', 44, 88)
          } else {
            Text(this.label).fontSize(12).fontColor('#ffffff').hitTestBehavior(HitTestMode.None)
          }
        }
        .width(this.controlSize()).height(this.controlSize())
        .id(`touch-control-${this.controlId}`)
        .hitTestBehavior(HitTestMode.Block)
        .backgroundColor(this.kind === 'key' ? '#b3202936' : '#191f2834')
        .borderRadius(this.kind === 'key' ? this.controlSize() / 2 : 6)
        .border({ width: 1, color: '#aaffffff' })
        .position({ x: this.centerX - this.controlSize() / 2, y: this.centerY - this.controlSize() / 2 })
        .onTouch((ev: TouchEvent) => this.dragTouch(ev))
        Text('×').fontSize(12).fontColor('#ffffff').width(22).height(22)
          .hitTestBehavior(HitTestMode.Block)
          .onTouch((event: TouchEvent) => event.stopPropagation())
          .textAlign(TextAlign.Center).backgroundColor('#d34b4b').borderRadius(11)
          .position({ x: this.centerX + this.controlSize() / 2 - 22, y: this.centerY - this.controlSize() / 2 })
          .onClick(() => { this.cancelInput(); this.onRemove(this.controlId); })
        if (this.kind !== 'key') {
          Text(this.arrows ? '↑↓←→ ⇄ WASD' : 'WASD ⇄ ↑↓←→')
            .hitTestBehavior(HitTestMode.Block)
            .onTouch((event: TouchEvent) => event.stopPropagation())
            .fontSize(10).fontColor('#ffffff').backgroundColor('#da303844')
            .padding(4).borderRadius(4)
            .position({ x: this.centerX - this.controlSize() / 2,
              y: this.centerY + this.controlSize() / 2 - 22 })
            .onClick(() => { this.cancelInput(); this.onMapping(this.controlId); })
        }
      } else if (this.kind === 'dpad') {
        Stack({ alignContent: Alignment.TopStart }) {
          this.arrowButton('↑', this.arrows ? 103 : 17, 44, 0)
          this.arrowButton('←', this.arrows ? 105 : 30, 0, 44)
          this.arrowButton('→', this.arrows ? 106 : 32, 88, 44)
          this.arrowButton('↓', this.arrows ? 108 : 31, 44, 88)
        }
        .width(130).height(130).hitTestBehavior(HitTestMode.Transparent)
        .id(`touch-control-${this.controlId}`)
        .hitTestBehavior(HitTestMode.Block)
        .position({ x: this.centerX - 65, y: this.centerY - 65 })
      } else {
        Stack({ alignContent: Alignment.Center }) {
          if (this.kind === 'stick') {
            this.stickVisual()
          } else {
            Text(this.label).fontSize(12).fontColor('#ffffff').hitTestBehavior(HitTestMode.None)
          }
        }
        .width(this.controlSize()).height(this.controlSize())
        .id(`touch-control-${this.controlId}`)
        .backgroundColor(this.kind === 'key' ? (this.pressed ? '#cc5079ad' : '#b3202936') : '#00000000')
        .borderRadius(this.controlSize() / 2)
        .border({ width: this.kind === 'key' ? 1.5 : 0, color: '#b8ffffff' })
        .position({ x: this.centerX - this.controlSize() / 2, y: this.centerY - this.controlSize() / 2 })
        .onTouch((ev: TouchEvent) => this.playTouch(ev))
      }
    }
    .width('100%').height('100%').hitTestBehavior(HitTestMode.Transparent)
    .onVisibleAreaChange([0], (visible: boolean) => { if (!visible) this.cancelInput(); })
  }
}

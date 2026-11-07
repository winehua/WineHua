// Direct game touch keeps button-down alive until up/cancel, with one owner.
export class GameTouchRouter {
  private target: number = 0;
  private x: number = 0;
  private y: number = 0;
  constructor(private emit: (target: number, action: number, x: number, y: number, button: number) => void,
    private press: number, private moveAction: number, private release: number, private leftButton: number) {}
  get active(): boolean { return this.target > 0; }
  begin(target: number, x: number, y: number): void {
    this.cancel();
    if (target <= 0) return;
    this.target = target; this.x = x; this.y = y;
    this.emit(target, this.press, x, y, this.leftButton);
  }
  move(x: number, y: number): void {
    if (!this.target) return;
    this.x = x; this.y = y;
    this.emit(this.target, this.moveAction, x, y, 0);
  }
  end(x: number, y: number): void {
    const target = this.target;
    this.target = 0; this.x = x; this.y = y;
    if (target > 0) this.emit(target, this.release, x, y, this.leftButton);
  }
  cancel(): void { this.end(this.x, this.y); }
}

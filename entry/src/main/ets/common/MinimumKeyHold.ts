// Fast touch taps can be shorter than a game's keyboard polling interval.
// Only virtual single-key controls use this; physical keyboards and sticks
// keep their original timing. Teardown always cancels the pending release.
export class MinimumKeyHold {
  private pressedAt: number = -1;
  private timer: number = -1;
  constructor(private minimumMs: number = 100) {}
  get pending(): boolean { return this.timer >= 0; }
  begin(): void { this.cancel(); this.pressedAt = Date.now(); }
  end(release: () => void): void {
    const remaining = this.pressedAt >= 0 ? this.minimumMs - (Date.now() - this.pressedAt) : 0;
    this.pressedAt = -1;
    if (remaining <= 0) { release(); return; }
    this.timer = setTimeout(() => { this.timer = -1; release(); }, remaining);
  }
  cancel(): void {
    if (this.timer >= 0) clearTimeout(this.timer);
    this.timer = -1; this.pressedAt = -1;
  }
}

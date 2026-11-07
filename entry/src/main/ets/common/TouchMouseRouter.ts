interface MouseSource { target: number; button: number; }

// Mouse operations use pointer events, never keyboard scan codes. Release is
// paired with the original target even after focus changes or layout teardown.
export class TouchMouseRouter {
  private sources: Map<string, MouseSource> = new Map();
  private counts: Map<string, number> = new Map();
  constructor(private emit: (target: number, button: number, down: boolean) => void) {}
  set(source: string, target: number, button: number, down: boolean): void {
    const old = this.sources.get(source);
    if (old && down && old.target === target && old.button === button) return;
    if (old) {
      this.sources.delete(source);
      const key = `${old.target}:${old.button}`;
      const count = this.counts.get(key) ?? 0;
      if (count <= 1) { this.counts.delete(key); this.emit(old.target, old.button, false); }
      else this.counts.set(key, count - 1);
    }
    if (!down || target <= 0 || (button !== 0x110 && button !== 0x111 && button !== 0x112)) return;
    const key = `${target}:${button}`;
    const count = this.counts.get(key) ?? 0;
    this.sources.set(source, { target, button }); this.counts.set(key, count + 1);
    if (count === 0) this.emit(target, button, true);
  }
  releasePrefix(prefix: string): void {
    const matches: string[] = [];
    this.sources.forEach((_value: MouseSource, source: string) => {
      if (source === prefix || source.startsWith(prefix + ':')) matches.push(source);
    });
    for (const source of matches) this.set(source, 0, 0, false);
  }
  releaseAll(): void {
    const sources: string[] = [];
    this.sources.forEach((_value: MouseSource, source: string) => sources.push(source));
    for (const source of sources) this.set(source, 0, 0, false);
  }
}

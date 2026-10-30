interface KeySource { target: number; codes: number[]; }

// Two controls may hold W/Shift simultaneously. Releasing one must not release
// the other; key-up must also go to the same target that received key-down.
export class TouchKeyRouter {
  private sources: Map<string, KeySource> = new Map<string, KeySource>();
  private counts: Map<string, number> = new Map<string, number>();
  constructor(private emit: (target: number, code: number, down: boolean) => void) {}

  set(source: string, target: number, codes: number[]): void {
    const old = this.sources.get(source);
    const next = codes.filter((code: number, index: number) => code > 0 && codes.indexOf(code) === index);
    if (old) {
      for (let i = old.codes.length - 1; i >= 0; --i) {
        const code = old.codes[i];
        if (old.target === target && next.indexOf(code) >= 0) continue;
        const key = `${old.target}:${code}`;
        const count = this.counts.get(key) ?? 0;
        if (count <= 1) { this.counts.delete(key); this.emit(old.target, code, false); }
        else this.counts.set(key, count - 1);
      }
    }
    if (target > 0) {
      for (const code of next) {
        if (old && old.target === target && old.codes.indexOf(code) >= 0) continue;
        const key = `${target}:${code}`;
        const count = this.counts.get(key) ?? 0;
        this.counts.set(key, count + 1);
        if (count === 0) this.emit(target, code, true);
      }
    }
    if (next.length > 0 && target > 0) this.sources.set(source, { target: target, codes: next });
    else this.sources.delete(source);
  }

  releasePrefix(prefix: string): void {
    const matches: string[] = [];
    this.sources.forEach((_value: KeySource, source: string) => {
      if (source === prefix || source.startsWith(prefix + ':')) matches.push(source);
    });
    for (const source of matches) this.set(source, 0, []);
  }

  releaseAll(): void {
    const matches: string[] = [];
    this.sources.forEach((_value: KeySource, source: string) => { matches.push(source); });
    for (const source of matches) this.set(source, 0, []);
  }
}

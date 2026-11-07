import testNapi from 'libentry.so';

// Physical XComponent coordinates, shared by the visible desktop cursor and
// layout mouse buttons. Controls themselves must not move the game cursor.
export class DesktopPointerService {
  private static instance: DesktopPointerService | null = null;
  static getInstance(): DesktopPointerService {
    if (!DesktopPointerService.instance) DesktopPointerService.instance = new DesktopPointerService();
    return DesktopPointerService.instance;
  }
  x: number = 0;
  y: number = 0;
  positioned: boolean = false;
  private listeners: Array<() => void> = [];
  subscribe(listener: () => void): void {
    if (!this.listeners.includes(listener)) this.listeners.push(listener);
  }
  unsubscribe(listener: () => void): void {
    const index = this.listeners.indexOf(listener);
    if (index >= 0) this.listeners.splice(index, 1);
  }
  move(x: number, y: number): void {
    if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || y < 0) return;
    if (this.positioned && this.x === x && this.y === y) return;
    this.x = x; this.y = y; this.positioned = true;
    this.listeners.forEach((listener: () => void) => listener());
  }
}

export function sendDesktopPointerEvent(target: number, action: number, x: number, y: number,
  button: number, dx: number = 0, dy: number = 0, physicalMouse: boolean = false): void {
  DesktopPointerService.getInstance().move(x, y);
  testNapi.sendPointerEvent(target, action, x, y, button, dx, dy, physicalMouse);
}

export function desktopKeyboardTarget(rootId: number): number {
  if (rootId <= 0) return 0;
  const target: number = testNapi.getKeyboardTarget() as number;
  return target > 0 ? target : rootId;
}

export interface ControlPoint { x: number; y: number; }
export interface StickSample extends ControlPoint { codes: number[]; }

export function clampControlCenter(x: number, y: number, size: number,
  width: number, height: number): ControlPoint {
  const halfX = Math.min(size / 2, width / 2);
  const halfY = Math.min(size / 2, height / 2);
  return {
    x: Math.max(halfX, Math.min(width - halfX, x)),
    y: Math.max(halfY, Math.min(height - halfY, y))
  };
}

// x/y are relative to the outer stick's top-left, in vp. Output is an
// eight-direction keyboard stick; no analog gamepad capability is advertised.
export function sampleKeyboardStick(x: number, y: number, size: number,
  arrows: boolean): StickSample {
  const dx = x - size / 2;
  const dy = y - size / 2;
  const length = Math.sqrt(dx * dx + dy * dy);
  const radius = Math.max(1, (size - 68) / 2);
  if (length <= radius * 0.22) return { x: 0, y: 0, codes: [] };
  const scale = Math.min(1, radius / length);
  const codes: number[] = [];
  // tan(22.5 degrees) gives eight sectors without accidental diagonal input
  // close to a cardinal direction.
  if (Math.abs(dx) >= Math.abs(dy) * 0.41421356237)
    codes.push(dx < 0 ? (arrows ? 105 : 30) : (arrows ? 106 : 32));
  if (Math.abs(dy) >= Math.abs(dx) * 0.41421356237)
    codes.push(dy < 0 ? (arrows ? 103 : 17) : (arrows ? 108 : 31));
  return { x: dx * scale, y: dy * scale, codes: codes };
}

export const smokeMapPoint: (guestX: number, guestY: number) => PointMap | null;
export const smokeMapPointX11: (guestX: number, guestY: number) => CanvasPoint | null;

export interface PointMap {
  px: number;
  py: number;
}

export interface CanvasPoint {
  nx: number;
  ny: number;
}

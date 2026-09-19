// A container name is an identifier, never a filesystem path. Keep this
// mapping identical to wine/container_session.cpp.
export const DEFAULT_GAME_CONTAINER_ID = 'default';
export const WINE_FILES_ROOT = '/data/storage/el2/base/files';

export function isValidGameContainerId(value: string): boolean {
  return /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/.test(value);
}

export function normalizeGameContainerId(value: string | undefined): string | null {
  if (value === undefined || value.length === 0) return DEFAULT_GAME_CONTAINER_ID;
  return isValidGameContainerId(value) ? value : null;
}

export function winePrefixForContainer(containerId: string): string {
  if (containerId === DEFAULT_GAME_CONTAINER_ID) return `${WINE_FILES_ROOT}/.wine`;
  return `${WINE_FILES_ROOT}/containers/${containerId}/prefix`;
}

export function wineSocketForContainer(containerId: string): string {
  return `${winePrefixForContainer(containerId)}/wine-wayland`;
}

export function wineDriveCForContainer(containerId: string): string {
  return `${winePrefixForContainer(containerId)}/drive_c`;
}

// Shared by launch requests, saved settings and the backend selector.
export function normalizeD3dBackend(value: string): string {
  return value === 'dxvk_legacy' || value === 'dxvk_modern_2_6' ||
    value === 'dxvk_legacy_d3d9' || value === 'wined3d_zink' ||
    value === 'vkd3d_limited_500k' ? value : 'wined3d';
}

export function isDxvkProfile(value: string): boolean {
  return value === 'dxvk_legacy' || value === 'dxvk_modern_2_6';
}

export function graphicsBackendHelp(value: string): string {
  switch (value) {
    case 'dxvk_legacy_d3d9': return '用于 32 位 DirectX 9 游戏，使用 DXVK 1.10.3。';
    case 'wined3d_zink': return '用于 OpenGL 与 DirectX 9 兼容性对照，使用内置优化配置。';
    case 'dxvk_legacy': return 'DirectX 10/11 兼容档，DirectX 9 沿用 WineD3D。';
    case 'dxvk_modern_2_6': return 'DirectX 10/11 较新后端，兼容性取决于设备 Vulkan 支持。';
    case 'vkd3d_limited_500k': return 'DirectX 12 实验后端，DirectX 11 使用已保存的 DXVK 档位。';
    default: return '适合老游戏和 DirectX 9 兼容性对照。';
  }
}

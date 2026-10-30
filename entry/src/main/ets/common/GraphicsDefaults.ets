import { deviceInfo } from '@kit.BasicServicesKit';

// Phone startup and OpenGL games use the VirGL baseline. Vulkan backends
// remain selectable after the desktop is running.
export const DEFAULT_D3D_BACKEND: string = deviceInfo.deviceType === 'phone' ?
  'wined3d' : 'vkd3d_limited_500k';

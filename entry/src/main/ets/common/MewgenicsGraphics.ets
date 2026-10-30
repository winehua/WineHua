import { deviceInfo } from '@kit.BasicServicesKit';

// Qualification is pinned to the device/driver used for real-game validation.
export function hasQualifiedMewgenicsGpu(): boolean {
  return deviceInfo.deviceType === 'phone' && deviceInfo.productModel === 'CRS-AL00' &&
    deviceInfo.incrementalVersion === '6.1.1.125';
}

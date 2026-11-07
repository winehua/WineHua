import { deviceInfo } from '@kit.BasicServicesKit';

// Keep the Windows taskbar above the phone's system gesture area.
export const PHONE_DESKTOP_BOTTOM_VP: number = deviceInfo.deviceType === 'phone' ? 16 : 0;

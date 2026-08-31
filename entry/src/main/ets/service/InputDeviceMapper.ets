import { hilog } from '@kit.PerformanceAnalysisKit';
import { inputDevice } from '@kit.InputKit';

const DOMAIN = 0x0000;
const TAG = 'DEV-MAP';

// InputDeviceMapper: 鼠标事件的 deviceId → 输入设备类型 映射, 以及 rawDelta →
// 注入增量的最终缩放系数 (统一在 ArkTS, C++ 只做透传)。来源: ArkUI MouseEvent
// 运行时携带 deviceId (类型定义无此字段, 20260822 实测鼠标=17/触控板=15),
// 官方 API inputDevice.getDeviceInfoSync 的 InputDeviceData.sources 含
// 'keyboard'|'mouse'|'touchpad'|... 可判类型。
//
// 系数组成 (20260822 从 C++ 迁入):
//   - MOUSE_SCALE=2.5: 原 C++ kSensRel, raw 硬件单位 → 游戏内增量的全局校准
//   - 触控板系数 0.75 = 2.5 × 0.3 (0.3 为设备差): 实测触控板 raw 每事件
//     4~14 而鼠标 0.5~2, 不衰减则"移动一点点光标跑很多", 手感不合适调
//     TOUCHPAD_DEVICE_SCALE
class InputDeviceMapper {
  // deviceId → sources 是否含 touchpad。首次见到该 id 时同步查一次系统
  // (getDeviceInfoSync 有 IPC 成本), 之后纯 Map 命中。设备插拔/重启 id 会变,
  // 新 id 自动重新查询, 无需失效逻辑。
  private touchpadCache: Map<number, boolean> = new Map();
  private static readonly MOUSE_SCALE: number = 2.5;
  private static readonly TOUCHPAD_DEVICE_SCALE: number = 0.25;
  private static inst: InputDeviceMapper | null = null;

  static getInstance(): InputDeviceMapper {
    if (InputDeviceMapper.inst === null) {
      InputDeviceMapper.inst = new InputDeviceMapper();
    }
    return InputDeviceMapper.inst;
  }

  // 读事件 deviceId → 增量缩放系数。deviceId 无效/查询失败一律按鼠标系数
  getScale(deviceId: number): number {
    if (!deviceId) return InputDeviceMapper.MOUSE_SCALE;
    let isTouchpad: boolean | undefined = this.touchpadCache.get(deviceId);
    if (isTouchpad === undefined) {
      isTouchpad = this.queryIsTouchpad(deviceId);
      this.touchpadCache.set(deviceId, isTouchpad);
    }
    return isTouchpad
      ? InputDeviceMapper.MOUSE_SCALE * InputDeviceMapper.TOUCHPAD_DEVICE_SCALE
      : InputDeviceMapper.MOUSE_SCALE;
  }

  private queryIsTouchpad(deviceId: number): boolean {
    try {
      const data = inputDevice.getDeviceInfoSync(deviceId);
      if (data && Array.isArray(data.sources)) {
        const is = data.sources.indexOf('touchpad') >= 0;
        hilog.info(DOMAIN, TAG, 'dev=%{public}d sources=%{public}s touchpad=%{public}d',
          deviceId, JSON.stringify(data.sources), is ? 1 : 0);
        return is;
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'getDeviceInfo(%{public}d) fail: %{public}s',
        deviceId, JSON.stringify(e));
    }
    return false;
  }
}

export { InputDeviceMapper };

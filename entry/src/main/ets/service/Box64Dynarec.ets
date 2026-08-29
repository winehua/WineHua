// ==== 所有权约定 (单源) ====
// 本文件是 BOX64_DYNAREC_* 可调键清单与各档取值的唯一来源 (policy 层,
// 留给 UI 档位/未来逐键微调演进)。native 只持有两样机制层能力, 不复制本表:
//   1. 出厂基线默认 (wine_env_baseline.h Box64PerfTable) — automation/broker
//      等无 ArkTS 参与路径的安全底, 语义是"box64 默认值偏离"而非"档位";
//   2. 前缀白名单注入 (env_profiles.cpp FilterCompatLines) — 只认
//      BOX64_DYNAREC_ 前缀, 不认识档位名, 新增可调键无需改 native。
// 维护规则: 新增/删除可调键、调整档位取值 → 只改本文件; 仅当出厂默认值
// 需要偏离 box64 编译默认时才动 Box64PerfTable。
// 未来 per-app UI 逐键微调: 在预设之上叠加用户覆盖 Record 再拼串, 仍走
// box64DynarecEnvString 同一 K=V 通道下发, native 零改动。
//
// Box64 动态翻译档位 → BOX64_DYNAREC_* 完整参数组合 (对齐 .temp/winlator
// Box64PresetManager.getEnvVars 的 4 档位, 每档完整 11 参数, 不含全局基线
// 的 BOX64_AVX / BOX64_DYNAREC_VOLATILE_METADATA — 那两项随全局基线保留)。
// 仅 arm64 (Box64 模拟) 生效; x86_64 原生跑忽略。档位语义从保守到激进:
// stability 最稳, performance 最快。注入时整体覆盖对应 key (完整档位语义,
// 不受全局基线残留参数干扰); 唯一例外: WEAKBARRIER 在 DXVK/vkd3d 桌面会话
// 链 (explorer 及桌面内程序) 被 AppendStableDesktopDxvkEnv 强制 clamp 为 0
// (Venus 共享内存 ring 的 x86 定序约束), 档位值只在 wineboot/wineserver
// (无图形) 生效。
// 独立模块/共享表: AppLibraryService (自定义启动 per-process 注入) 与
// WineEnvService (全局基线, 经 launchClient 下发 native) 都引用 — 避免两
// service 循环 import。
export const BOX64_DYNAREC_PRESETS: Record<string, Record<string, string>> = {
  'stability': {
    'BOX64_DYNAREC_SAFEFLAGS': '2',
    'BOX64_DYNAREC_FASTNAN': '0',
    'BOX64_DYNAREC_FASTROUND': '0',
    'BOX64_DYNAREC_X87DOUBLE': '1',
    'BOX64_DYNAREC_BIGBLOCK': '0',
    'BOX64_DYNAREC_STRONGMEM': '2',
    'BOX64_DYNAREC_FORWARD': '128',
    'BOX64_DYNAREC_CALLRET': '0',
    'BOX64_DYNAREC_WAIT': '0',
    'BOX64_DYNAREC_NATIVEFLAGS': '0',
    'BOX64_DYNAREC_WEAKBARRIER': '0'
  },
  'conservative': {
    'BOX64_DYNAREC_SAFEFLAGS': '2',
    'BOX64_DYNAREC_FASTNAN': '0',
    'BOX64_DYNAREC_FASTROUND': '0',
    'BOX64_DYNAREC_X87DOUBLE': '1',
    'BOX64_DYNAREC_BIGBLOCK': '1',
    'BOX64_DYNAREC_STRONGMEM': '1',
    'BOX64_DYNAREC_FORWARD': '128',
    'BOX64_DYNAREC_CALLRET': '0',
    'BOX64_DYNAREC_WAIT': '1',
    'BOX64_DYNAREC_NATIVEFLAGS': '0',
    'BOX64_DYNAREC_WEAKBARRIER': '1'
  },
  'intermediate': {
    'BOX64_DYNAREC_SAFEFLAGS': '2',
    'BOX64_DYNAREC_FASTNAN': '1',
    'BOX64_DYNAREC_FASTROUND': '0',
    'BOX64_DYNAREC_X87DOUBLE': '1',
    'BOX64_DYNAREC_BIGBLOCK': '2',
    'BOX64_DYNAREC_STRONGMEM': '0',
    'BOX64_DYNAREC_FORWARD': '128',
    'BOX64_DYNAREC_CALLRET': '0',
    'BOX64_DYNAREC_WAIT': '1',
    'BOX64_DYNAREC_NATIVEFLAGS': '0',
    'BOX64_DYNAREC_WEAKBARRIER': '2'
  },
  'performance': {
    'BOX64_DYNAREC_SAFEFLAGS': '1',
    'BOX64_DYNAREC_FASTNAN': '1',
    'BOX64_DYNAREC_FASTROUND': '1',
    'BOX64_DYNAREC_X87DOUBLE': '0',
    'BOX64_DYNAREC_BIGBLOCK': '3',
    'BOX64_DYNAREC_STRONGMEM': '0',
    'BOX64_DYNAREC_FORWARD': '512',
    'BOX64_DYNAREC_CALLRET': '1',
    'BOX64_DYNAREC_WAIT': '1',
    'BOX64_DYNAREC_NATIVEFLAGS': '1',
    'BOX64_DYNAREC_WEAKBARRIER': '2'
  }
};

// 档位值 (不含 ''), 与 BOX64_DYNAREC_PRESETS 键一一对应
export type DynarecMode = 'stability' | 'conservative' | 'intermediate' | 'performance';
// 全局兼容模式值: '' = 出厂基线 (不注入, 当前默认行为)
export type CompatMode = '' | DynarecMode;

// 白名单归一 (写入与 preferences 读回两处都用, 防脏键/旧数据污染 env 串)
export function normalizeCompatMode(m: string): CompatMode {
  if (m == 'stability' || m == 'conservative' || m == 'intermediate' || m == 'performance') {
    return m;
  }
  return '';
}

// 档位 → "K=V;K=V;..." 行串 (native 零表, 直接拆行 UpsertEnvLine);
// ''/未知档 = 空串 = 不注入 (保持出厂基线)
export function box64DynarecEnvString(mode: string): string {
  const preset = BOX64_DYNAREC_PRESETS[mode];
  if (!preset) {
    return '';
  }
  return Object.keys(preset).map((k: string) => `${k}=${preset[k]}`).join(';');
}

// 档位 UI 元数据: 4 档 (按 BOX64_DYNAREC_PRESETS 顺序), 第 0 项由各 UI 自定义
// (设置页「默认」= 出厂基线不注入; 弹窗「跟随全局」= 弹窗不覆盖落全局档)。
// index↔档位映射据此派生, 避免 4 处手写三元链。按钮文本因 SegmentButton
// buttons 要求静态元组, 无法 spread 动态数组, label 留在各 UI 手写。
interface CompatModeOption {
  label: string;
  value: DynarecMode;
}

const COMPAT_MODE_OPTIONS: CompatModeOption[] = [
  { label: '稳定', value: 'stability' },
  { label: '保守', value: 'conservative' },
  { label: '均衡', value: 'intermediate' },
  { label: '性能', value: 'performance' }
];

// 档位值 → 段选 index (第 0 项=自定义项): 未知/'' → 0, 与 normalizeCompatMode
// 兜底同向 (显示「默认/跟随全局」与不注入的实际行为一致)
export function compatIndexOfMode(mode: string): number {
  const idx = COMPAT_MODE_OPTIONS.findIndex((o: CompatModeOption) => o.value == mode);
  return idx >= 0 ? idx + 1 : 0;
}

// 段选 index → 档位值: 第 0 项/越界 → undefined (调用方自行映射: 设置页=基线, 弹窗=跟随全局)
export function compatModeFromIndex(idx: number): DynarecMode | undefined {
  const opt = COMPAT_MODE_OPTIONS[idx - 1];
  return opt?.value;
}

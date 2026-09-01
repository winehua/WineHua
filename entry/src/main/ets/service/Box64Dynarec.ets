// ==== 所有权约定 (单源) ====
// 本文件是 BOX64_DYNAREC_* 可调键清单与各档取值的唯一来源 (policy 层,
// 留给 UI 档位/未来逐键微调演进)。native 只持有出厂基线默认
// (wine_env_baseline.h Box64PerfTable) — automation/broker 等无 ArkTS
// 参与路径的安全底, 语义是"box64 默认值偏离"而非"档位";
// 档位注入自身经 ArkTS per-app environment 下发, native 零表。
// 维护规则: 新增/删除可调键、调整档位取值 → 只改本文件; 仅当出厂默认值
// 需要偏离 box64 编译默认时才动 Box64PerfTable。
// 未来 per-app UI 逐键微调: 在预设之上叠加用户覆盖 Record 再拼串,
// 仍经 runWineProgram 的程序级 environment 下发, native 零改动。
//
// Box64 动态翻译档位 → BOX64_DYNAREC_* 完整参数组合 (对齐 .temp/winlator
// Box64PresetManager.getEnvVars 的 4 档位, 每档完整 11 参数, 不含全局基线
// 的 BOX64_AVX / BOX64_DYNAREC_VOLATILE_METADATA — 那两项随全局基线保留)。
// 仅 arm64 (Box64 模拟) 生效; x86_64 原生跑忽略。档位语义从保守到激进:
// stability 最稳, performance 最快。注入时整体覆盖对应 key (完整档位语义,
// 不受全局基线残留参数干扰)。
// 注入范围: 仅程序级 (runWineProgram 的 per-app environment)。
// 会话启动链 (wineserver/wineboot/explorer) 不注入档位, 保持出厂基线
// (Winlator 同款: 档位不进 XServer)。
// 独立模块/共享表: 自定义启动 (runWineProgram 调用方) 与 WineEnvService
// (全局基线, 经 launchClient 下发 native) 都引用 — 避免两 service 循环 import。
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

// 全局兼容模式值: '' = 出厂基线 (不注入, 当前默认行为);
// 其余与 BOX64_DYNAREC_PRESETS 档位一一对应
export type CompatMode = '' | 'stability' | 'conservative' | 'intermediate' | 'performance';

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

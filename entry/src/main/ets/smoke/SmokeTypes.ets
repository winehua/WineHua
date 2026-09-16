// Smoke 自动化请求的公共类型 (EntryAbility 解析 Want → AppStorage →
// SmokeHook 消费 → SmokeRunner 消费)。独立文件避免 SmokeHook ↔
// SmokeRunner 的循环 import。

// job: host 生成的"跑什么、怎么跑"描述 (docs/SMOKE_V2_DESIGN.md §5)。
// 三种选测方式互斥, 优先级 inline > tests > suite:
//   inline —— 内联测试定义 (调试临时用例: exe 先推送到 C:\smoke, 定义随 job 下发,
//             不经过 suites.json)
//   tests  —— 按 testId 选测 (跑 suite 的子集)
//   suite  —— 跑整个 suite
// params 对选中的所有测试生效 (env 合并 / argv 追加 / 后端与时长覆盖),
// 用于调试参数组合, 不需要改仓库任何文件。
export interface SmokeJobParams {
  env?: Record<string, string>;
  argv?: string[];
  d3dBackend?: string;
  dxvkBackend?: string;
  seconds?: number;
  timeoutMs?: number;
}

// 测试实例定义：suites.json 条目与 job.inline 条目同一形状（SmokeRunner 消费）
export interface SmokeSuiteTest {
  testId: string;
  exe: string;
  env?: Record<string, string>;
  // 'raw' = 原样用 argv（d3d12 等自有参数的程序）；缺省 = 生成统一 smoke 协议 argv
  argvMode?: string;
  argv?: string[];
  // 'present'（默认，带 --seconds）/ 'offscreen'（wine-vulkan offscreen 系）
  mode?: string;
  // 追加参数（如 d3d9-cube 的 --d3d9）
  extraArgs?: string[];
  d3dBackend?: string;
  dxvkBackend?: string;
  seconds?: number;
  timeoutMs?: number;
}

export interface SmokeJob {
  suite?: string;
  tests?: string[];
  inline?: SmokeSuiteTest[];
  params?: SmokeJobParams;
  prefix?: string;
  longSeconds?: number;
}

export interface SmokeRequest {
  suite: string;
  runId: string;
  // 'want' = 外部脚本 aa start 协议; 'ui' = 侧边栏手动入口
  source: string;
  // 'reuse' = 复用当前 .wine; 'clean' = 先清 prefix (只清 C 盘, 不删运行时)
  prefix?: string;
  // dxvk-long/dxvk-modern-long 的墙钟目标 (seconds=-1 哨兵)
  longSeconds?: number;
  // job 内容 (host 经 winehua.job_file 下发; 无 job 时按 suite + 5 键走旧协议)
  job?: SmokeJob;
}

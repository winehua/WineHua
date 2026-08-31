// Smoke 自动化请求的公共类型 (EntryAbility 解析 Want → AppStorage →
// SmokeHook 消费 → SmokeRunner 消费)。独立文件避免 SmokeHook ↔
// SmokeRunner 的循环 import。
export interface SmokeRequest {
  suite: string;
  runId: string;
  // 'want' = 外部脚本 aa start 协议; 'ui' = 侧边栏手动入口
  source: string;
  // 'reuse' = 复用当前 .wine; 'clean' = 先清 prefix (只清 C 盘, 不删运行时)
  prefix?: string;
  // dxvk-long/dxvk-modern-long 的墙钟目标 (seconds=-1 哨兵)
  longSeconds?: number;
}

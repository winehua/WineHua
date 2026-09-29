# 平板 Direct 性能基准准备（2026-09-29）

已增加真实 AMD64/FEX 与原生 ARM64 两个测试 EXE、稳态计时/原始记录、HAP Native CPU/present 快照和 ABBA runner。用于执行 [性能方案](direct-performance-gate-20260929.md)，不是已经取得性能收益。

- signed HAP SHA-256：`b741c40f669798a6311dc7f4c383c061891406ccafb64b0165148044d24f9f0d`，native/ArkTS 构建、签名、candidate 校验通过。
- 本候选 runtime manifest 与平板已解压内容完全一致，SHA-256 `b97cd72c2d3ee4ee8534ddc78103e3423910c264d439a8c3831fa652e0b043ea`；没有 Wine/FEX 更新或 prefix 重置。
- 完整 host payload 为 `smoke-v2-474c8f475ecd`，共 36 EXE；原有 34 个 EXE 字节保持不变，只新增 AMD64 `0x8664` 与 ARM64 `0xaa64` benchmark。
- 原 ARM64 cube/scene 的 `x64` 槽标签不能作为实际 AMD64/FEX 性能证据。历史截图和 NativeBuffer 链路验证仍有效；真实 AMD64 基准已通过两条短工具检查与第一组四轮 fixed60 ABBA，完整序列在第五轮超时中止，见 [初步数据与失败](direct-performance-results-20260929.md)。
- 几何 87 checks、统计函数 checks、benchmark artifact 的 5 项 Python 测试通过；两种 PE 都用 `-Wall -Wextra -Werror` 编译通过。没有重跑受既有删除 API 阻止的完整 `make test`。

手机仍搁置；Steam、FEX 默认引擎和子模块保持原有状态。此前锁屏返回 10106102；本次已能启动并运行真实 AMD64 基准，当前障碍是第五轮程序启动后的测量超时。尚未完成完整性能验收和 GPU 裁剪显示矩阵。

在工作区根目录可先检查 sanity 命令（该模式不启动设备）：

```powershell
python -X utf8 automation/run_direct_performance.py `
  --device 5KPBB25818203996 --archive F:/WineHua/.temp/direct-performance-pad-20260929/sanity `
  --hap entry/build/default/outputs/default/entry-default-signed.hap `
  --payload build/smoke-performance-payload --architecture amd64 `
  --profile light --sanity --plan-only
```

去掉 `--plan-only` 执行两个 12 秒测量工具检查（3 秒预热），结果不用于性能验收。正式 fixed60 比较换新归档目录，去掉 `--sanity`、选 `--profile fixed60 --blocks 3`；默认 15 秒预热 + 60 秒计时。全部命令经唯一 smoke runner 执行，不在计时期间启动截图观察器。

baseline 与 Direct 的输出队列/合成器调度不同，因此正式结论还需核对实际 accepted-output FPS、热状态、前台/刷新条件、噪声与真实游戏覆盖；当前不报告任何速度百分比。

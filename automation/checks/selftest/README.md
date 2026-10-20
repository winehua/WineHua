# checks 判定器合成自测

`automation/checks/` 各判定器的合成形态自测资产：不依赖设备，纯主机端
生成合成帧/数据并跑判定器，断言「期望判定 = 实际判定」，退出码非零即
RED。判据改动后必须复跑对应自测（判定只读归档数据，合成帧即归档）。

## 现有自测

| 目录 | 覆盖判定器 | 形态数 | 运行 |
|---|---|---|---|
| `popup/` | `fusion-popup-menu` (frame.py) | 11（含真机像素形态，`.temp/m4c-popup-1.jpeg` 不在场时优雅跳过为 10） | `python3 automation/checks/selftest/popup/gen_and_run.py` |
| `auto_retry/` | `auto-retry` (auto_retry.py) | 8（合成 hilog 形态日志；含 SKIP×3 / PASS×4 / FAIL×1） | `python3 automation/checks/selftest/auto_retry/gen_and_run.py` |
| `presented_route/` | `presented-route` (__init__.py) | 15（D49 x86 启动器/承载进程归属、几何矛盾、证据缺口形态；含既有守卫回归锚） | `python3 automation/checks/selftest/presented_route/gen_and_run.py` |
| `fusion_cube/` | `fusion-cube` (frame.py) | 5（合成正例/小彩块/诱饵/空白窗反例 + 真机帧形态，`.temp/fusion-cube-1.jpeg` 不在场时优雅跳过为 4） | `python3 automation/checks/selftest/fusion_cube/gen_and_run.py` |

产物落在各自 `build/*-selftest/`（popup 为合成帧 JPEG，auto_retry /
presented_route 为合成日志与归档目录），build/ gitignored，每次运行重新
生成——`rm -rf build/` 后从零复跑即毁灭性重建演练。

## TODO（同债未迁）

- **M4a**：fusion 视觉判定器合成数据自测（A 正例 / B 正例 / B 象限错位
  反例，见 `.superpowers/sdd/2026-10-10-x11-multimode-m4a/progress.md`
  「Final: minor」I2 延伸）——生成脚本只在 build/ 现场未入库。
- **M4b**：`fusion-state` 判定器合成自测 7 形态（见
  `.superpowers/sdd/2026-10-10-x11-multimode-m4b/progress.md`）——同上。

两期生成器下次触碰对应判定器时按 `popup/` 本模板迁入（每期一个目录，
脚本头部写明运行方式与退出码语义）。

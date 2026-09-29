# Direct GPU 图像 viewport 采样（2026-09-29）

原 Direct 桌面 snapshot 只使用 Wine client subsurface 的 `set_destination`，没有把 `set_source` 传给 shader，裁剪会变成缩放整张图像。本轮新增 GPU 采样映射：源矩形、buffer scale/transform 转成两个仿射 UV 行，通过 32 字节 fragment push constant 采样同一导入图像。没有游戏图像 readback、CPU 裁剪或重新上传。

`DirectViewportState` 保留 Wayland fixed-point 转成 double 后的精度，协议设置写 pending，surface commit 在 toplevel manager 锁下发布 current，scene 只读 current。源坐标以 transform/scale 后的 surface-local 空间解释；映射方向参考 [Weston 14.0 matrix](https://github.com/wayland-mirror/weston/blob/14.0/shared/matrix.c) 与 [surface buffer matrix](https://github.com/wayland-mirror/weston/blob/14.0/libweston/compositor.c)。八种 transform、自然尺寸和显式目标尺寸由无平台依赖的 `ComputeDirectViewport` 处理。越界或非整数自然尺寸返回无效，不猜测为整张图。

scene 的 idle 比较加入 source extent 与采样矩阵，因此只有 viewport 改变、没有新 GPU buffer 时也会重画。窗口位置、全屏 fit 和输入仍使用目标 surface 尺寸；pointer 坐标不加源裁剪偏移。SHM/GDI 快照保留原有裁剪路径与 identity sampling，避免二次裁剪。fusion 单图 presenter 尚未扩展这一窗口 viewport 能力。

viewport resource 加入 surface destroy listener：surface 先销毁后，source/destination 请求报告 `no_surface`；销毁 viewport 本身不解引用旧 surface。surface scale/transform 和 viewport source/destination 加入参数范围检查。Direct current 在 commit 时应用；既有 Wayland subsurface synchronized parent commit 的完整语义不属于本次新增的保证。

验证状态：

- `make test-direct-viewport`：87 checks、0 failures，覆盖非方形图像、scale=2、小数源矩形、八种 transform 的角点、目标尺寸与非法输入。
- shader 已由 `glslangValidator` 重新生成；完整 ARM64 HAP 构建、签名及 candidate 检查通过。
- 此阶段 signed HAP SHA-256：`c64d3c8567e5ccf444e80f5550bab25cafa20bfe893b24e56fcace3c8507ea62`，曾安装 MatePad Mini；当时 payload 为 `smoke-v2-df9a09211262`，Wine/runtime 未更新。当前已被包含同一采样代码的 [性能基准候选](direct-performance-preparation-20260929.md) `b741c40f…9f0d` 替换，host payload 为 `smoke-v2-474c8f475ecd`。
- 首次显示回归启动被锁屏阻止（`10106102`），没有运行 fixture，不能标为 PASS。实际 GPU 裁剪/transform 的画面验收也尚未完成；现有双窗口 fixture 主要覆盖 identity sampling。

截图观察器已增加捕获前后 phase 核对，短阶段结束后的截图会标 `phaseStable=false` 并返回失败，避免之前第二轮 windowed 漏拍被文件数量掩盖。此前 [启动 gate 的三轮结果](direct-desktop-startup-20260929.md) 属于 f5dbf252 包，不能当成本包回归。

这项改动解决采样正确性，不构成性能提升证据。用户当前关心的收益应按 [同设备 Direct/Venus 性能验收](direct-performance-gate-20260929.md) 测量，再决定投入和默认选择。

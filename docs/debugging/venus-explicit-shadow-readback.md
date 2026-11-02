# Vulkan 显式映射读回与真实游戏首帧停滞

2026-11-02，PR #79 产品候选在 Maleoon 920C 上复现：MewGenics 有声音，
爪印及三个星环停在第一帧；Darkest Dungeon 有声音但缺少画面。地牢正常的
黑屏、黑红触手、logo 顺序本身不是加载失败依据。

## 根因与修复

托管 Zink 经 Venus 使用 Host Vulkan。默认配置的
`vkInvalidateMappedMemoryRanges` 曾沿用旧 DXVK 的“Guest 写入开始”重载，
没有履行 Vulkan 的 Host→Guest 显式读回契约。同时，无关 fence 的隐式全分配
读回可能覆盖 remote 映射尚未发布的 Guest 数据和上传快照，并清除脏区间。

`virgl-shadow-explicit-readback.patch` 同时修复两处：

- 默认及普通 profile 执行真正的区间 invalidate；旧重载只允许显式诊断值
  `VKR_WINEHUA_SHADOW_FROM_HOST=legacy-write-begin`。
- remote 映射由显式 flush/invalidate 管理区间，不参与 fence 的隐式整块回写；
  非 remote 兼容路径保留。

仅加 remote guard 的候选仍有 52 个像素断言失败，不能作为有效修复。
最终组合在默认 baseline 下通过 GL 3.2 transfer/instancing/FBO 探针：
240 帧连续像素、静态网格及四象限检查，`RESULT failures=0`。
托管 Zink 同时采用与 DXVK/VKD3D 相同的 Box64 ring 发布屏障策略；
该屏障调整单独不能解决此故障。显式逐程序环境覆盖仍在最后应用。

## 回归与设备证据范围

`scripts/test_virgl_shadow_readback.py` 从实际 pinned renderer 源码提取读回分支和
invalidate dispatcher，验证八种 remote/write/dirty 组合及区间读回。
旧广播行为和旧 invalidate 行为分别作为必须失败的负对照。
构建入口自动应用补丁，`validate_desktop_rendering.py` 纳入该回归。

修复后产品 HAP 1.0.31 中，MewGenics 已显示 Play/Settings/Meow/Quit 主菜单，
连续截图显示游戏区域变化；Darkest Dungeon 已显示“战役”主菜单。
用户也确认动画恢复。以上证明启动画面与动画恢复，不代表新战役、存档读写、
长时间游戏、所有输入/IME 或其他设备已经通过验收。

设备结果来自默认 baseline/wined3d 的托管 Zink 路径；没有启用强制
precise/legacy-host-sync 诊断 profile。原始日志及设备路径仅保留本地。

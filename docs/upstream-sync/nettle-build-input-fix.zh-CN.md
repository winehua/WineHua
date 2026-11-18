# Nettle ARM64 构建依赖补齐（2026-11-19）

## 原因与修复

`scripts/build_gnutls.sh` 已迁入 WineHua，但迁移清单的补丁目录白名单遗漏了
`patches/nettle/`。因此干净环境在计算补丁 SHA-256 时停止；本地复用的 VPP
GnuTLS/Nettle 缓存使此前的包验证没有暴露此问题。此前对子模块的核对只覆盖
主要运行时，不能据此宣称完整构建依赖没有漏项。

从 VPP 2.0.4 源码提交 `043a1cc0cab8e52b011faf2c611d3c16562ecb0a` 原样恢复：

- `patches/nettle/0001-ohos-arm64-fat-dispatch.patch`
  （SHA-256 `3212b697e5bfc92f93123ecde0c5694d1e045c494a8ecbb691e9d374ebc8712d`）。
- `host_tests/nettle_arm64_dispatch_test.py`。

补丁在 OHOS/musl 下使用经过编译链接探测的 `getauxval(AT_HWCAP)`，避开
SDK 中缺少的 `asm/hwcap.h`，并禁止环境变量强行启用 CPU 不支持的指令。
保留 `VP_NETTLE_ASM=auto/off` 和探测失败后的 C 模式回退。

构建脚本在加载工具链、下载依赖前明确检查补丁存在。新增
`make runtime-patch-check`，检查脚本、Makefile 和有效锁定配方中的字面量补丁
路径同时存在于 Git 索引及工作树，防止本地未提交文件掩盖遗漏。
历史 `productionAdaptations` 元数据不作为有效构建输入。
检查已接入 `prepare-runtime-sources`、`runtime-host-check` 和 CI checkout 后步骤；
`make runtime-crypto-check` 另外运行 Nettle 行为测试。

## 本地验证

- 170 个字面量补丁依赖均已跟踪且存在；未发现第二个同类遗漏。
- 缺失、空文件和存在但未跟踪的补丁被拒绝；缺失 Nettle 补丁时
  `auto/off` 两模式都在执行 `env.sh` 前失败。
- 128 组 HWCAP 组合、OHOS 有/无 `__MUSL__`、探测成功/失败、架构切换、
  环境覆盖保护、配置对象选择和缓存身份变化均通过现有行为测试。
- 使用现有 OHOS SDK 通过真实 ARM64 `getauxval` 编译链接探测；补丁后的
  `fat-arm64.c` 通过 ARM64 编译检查。
- 生产 `build_gnutls.sh --check-identity` 与现有缓存身份一致。
- `runtime_pipeline_test.py`、脚本语法和源码空白检查通过。

本轮复用现有 3.10.2 官方源码和构建缓存，没有重编完整依赖链或 HAP，
没有进行真机测试，也不将本地主机验证表述为远端 CI 通过。
字面量路径预检不替代各配方对动态、通配符补丁列表及补丁内容的校验。

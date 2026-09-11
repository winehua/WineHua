# P2 真机验证：UnixLib 加载与内核能力实测

> 执行：2026-09-11，真机 USB hdc
> 结论：**UnixLib 已被 wine 真实加载**；硬件 TSO 与未对齐原子在真机上**明确不支持**（有真实返回值）。

## 1. 被测对象

| 项 | 值 |
| --- | --- |
| 设备 | `MLR-AL10`，API 26，HarmonyOS `7.0.0.105(SP10C00E105R1P2)` |
| 连接 | USB，hdc `Ver 3.2.0c`，target `5KPBB25818203996` |
| 应用 | `app.hackeris.winehua` |
| 被测 HAP | 重打包签名版，418 964 156 B（仅替换 FEX 产物，其余与原包一致） |

```text
HAP sha256          25f522dee297219c4b4c89119bbc1ec824f8da5c86159b7df8a52c26e895d15f
payloadSha256       88dc953b... -> b90c4e6700c9bfd2a07653a8fa5a5f4ae99bcf61de6f53781bbf44b51ea2b5f8
```

产物哈希（构建端与设备端一致）：

```text
libarm64ecfex.dll  9e9276b712201c0ec4f2a21aad156567acdc2dec099b44ab5fc96b35fcf74c30
libwow64fex.dll    1674c5b42746bb629e7dd370a80202cdd0229de901318c492f5e4b172ca1cf30
libarm64ecfex.so   c4eaa204601c4c1a3b4fe1cf6ef14e773c256963a0bdaaad5d9ec2301124bbc9
libwow64fex.so     1324a61ca3423c9674d7876de04a717525e5efa8a3a356ff627e8e9baecaac7a
```

设备侧 `sha256sum` 校验已确认两个 DLL 就是本次构建的产物（非旧包残留）。

## 2. 怎么取证

FEX 的 UnixLib 加载**默认没有任何输出**，成功与否都无法从日志区分。因此加了一个
**只在被 dlopen 时才触发**的构造函数探针（临时诊断，见 §5）：

```cpp
#define WINEHUA_FEX_PROBE(...) ::dprintf(2, "[WINEHUA-FEX-PROBE] " __VA_ARGS__)
__attribute__((constructor)) static void WineHuaFexUnixLibLoadedProbe() { ... }
```

它写在 `Source/Windows/UnixLib/FEXUnixLib.cpp`（即 `.so` 内），只有 Wine 真的
`dlopen` 了这个 `.so` 才会执行；输出走 fd 2，由 WineHua 的 `wine_child` 落到
`files/temp/wine_stderr_YYYYMMDD.log`。

> 也尝试过直接读 `/proc/<pid>/maps` 作为证据，但 hdc shell（uid 2000）读其他应用
> 进程的 maps 是 `Operation not permitted`，此路不通。

## 3. 实测结果

`/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_20260911.log`：

```text
[WINEHUA-FEX-PROBE] unixlib-dotso loaded pid=3754
[WINEHUA-FEX-PROBE] unixlib-dotso loaded pid=3847
[WINEHUA-FEX-PROBE] hardware-TSO enable get=0xffffffff already errno=22 result=NOT_SUPPORTED
[WINEHUA-FEX-PROBE] unaligned-atomic flags=0x3 ret=-1 errno=22 result=NOT_SUPPORTED
```

### 3.1 UnixLib 真的被加载了 ✅

构造函数在**两个 wine 进程**里执行 ⇒ Wine 的
`load_unixlib_by_name()` 找到了 `.so` 并 `dlopen` 成功，FEX 侧
`MemoryWineLoadUnixLibByName` 这条新链路是通的。

对应方案 P2 的验收项"能确认正式 UnixLib 成功加载，不能仅凭游戏进入菜单判断" —— 通过。

离线依赖也一并满足：`.so` 的 `DT_NEEDED` 是 `libc++_shared.so` + `libc.so`，
HAP 的 `libs/arm64-v8a/` 已带 `libc++_shared.so`。

### 3.2 硬件 TSO：真机不支持（真实返回值）❌

```text
prctl(PR_GET_MEM_MODEL, ...) → 0xffffffff, errno=22 (EINVAL)
```

关键点：**这不是"接口没接通"，而是内核不认这个 prctl 选项。** UnixLib 已经加载、
调用确实发生、返回值可读。所以方案 §5.5 的"`hardware_tso=false` 不等于接口未接通"
在这一台设备上得到了实测确认。

实际回退：FEX 源码默认 `TSOEnabled=true`，在硬件 TSO 不可用时走软件 TSO。

### 3.3 未对齐原子控制：同样不支持 ❌

```text
prctl(PR_ARM64_SET_UNALIGN_ATOMIC, 0x3, ...) → -1, errno=22 (EINVAL)
```

### 3.4 SHM 统计：这次没有触发 ⏳

日志里既没有 `shm-stats` 也没有 `shm_open ... FAILED`，说明 `GetSHMStatsVMA`
根本没有被调用。原因与预期一致：`ProfileStats` 默认 false，而且
`ENABLE_FEXCORE_PROFILER` 在构建时是 OFF。所以 SHM 统计仍卡在"要开编译开关 + 配置文件"，
不是被平台挡住。

### 3.5 应用仍然可用 ✅

29 个 `Native_libwine_child` 进程在跑，`hilog` 无 `CRASH/SIGSEGV/SIGABRT/fatal`。

## 4. 顺带确认的运行期事实

从 `[Broker]` / `[WineChild]` 日志取到的真实环境（此前只有代码推断）：

```text
WINEDLLPATH = .../wine/bin/aarch64-windows:.../wine/bin/i386-windows:.../wine/bin:.../wine/bin/aarch64-unix:/data/storage/el1/bundle/libs/arm64
WINEDLLDIR  = .../wine/bin/aarch64-unix
WINEUNIXDIR = .../wine/bin
LD_LIBRARY_PATH = /data/storage/el1/bundle/libs/arm64
HODLL64 = libarm64ecfex.dll     HODLL = libwow64fex.dll     engine=(default fex)
```

两点直接支撑了前面的打包决定：

1. `WINEDLLPATH` 里确实包含 `/data/storage/el1/bundle/libs/arm64`，所以把 UnixLib 放进
   `entry/libs/<arch>/` 就能被 `load_unixlib_by_name` 找到 —— `assemble.sh` 的归位位置正确。
2. 设备上并没有 `aarch64-unix/lib*wow64fex.so`，加载走的是 `<libs>/libwow64fex.so` 这条退化路径。

## 5. 本次新增的实验件（非常规构建路径）

| 文件 | 作用 |
| --- | --- |
| `scripts/patches/fex-unixlib-probe.patch` | 探针补丁（+30/-2），只在 `FEX_UNIXLIB_PROBE=1` 时随构建应用 |
| `docs/proton-parity/tools/repack_hap_unixlib.py` | 把新 FEX 产物注入已有 HAP 的最小重打包工具 |

两者都**不进产品构建路径**：`make hap` 不会用 `FEX_UNIXLIB_PROBE`，重打包脚本只是替代
一次 8GB build 目录复制 + hvigor 全量构建，用于快速真机对照。正式出包仍应走完整 `make`。

## 6. 部署流程上的一个坑（已实测）

覆盖安装新运行时**不会自动生效**。应用在启动时比对
`packaged wine-runtime-manifest.json payloadSha256` 与已解包运行时记录的哈希，
不一致时只提示：

```text
upgrade pending: packaged wine runtime differs from extracted data.
Use "重置 Wine" to apply the update (full factory reset).
```

不按下"重置 Wine"就不会重新解包。本次采用文档既有的
`bm uninstall` → `bm install` → 启动 流程，代价是**清掉 WineHua 的 prefix**。
实测该 prefix 只是当天 wineboot 出来的空壳（`Program Files` 下无任何用户安装项），
游戏数据在共享存储上，未受影响。

## 7. 结论与下一步

| 项 | 之前 | 现在 |
| --- | --- | --- |
| OHOS-03 UnixLib 加载 | blocked | **pass_new**（真机加载成功） |
| OHOS-11 硬件 TSO / 未对齐原子 | not_tested | **unsupported_with_verified_fallback**（G2，返回值齐全） |
| OHOS-12 SHM 统计 | not_tested | not_tested（未触发，需开 profiler + 配置） |

下一步按收益排序：

1. 开 `ENABLE_FEXCORE_PROFILER=True` + 落 `FEX_Config.json`，把 SHM 统计真正打开，
   再做 x86/x64 分开测量（方案 P3/P4）。
2. 顺手把 `build_fex.sh` 的构建类型对齐到 Release（`p2-release-probe.md` 已验证参数可用）。
3. 把探针换成正式可关的开关（或直接删除），不要让诊断输出留在产物里。

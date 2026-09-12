# FEX 构建参数对齐官方（方案 §5.2）

> 执行：2026-09-12
> 结论：`build_fex.sh` 已按 Proton 官方 FEX 段参数构建，产物与真机回归均通过；
> 参数对齐**没有**改变性能结论（再次印证 CPU 转译不是瓶颈）。

## 1. 变更

旧基线与官方参数的差异（方案 §5.2）：

| 参数 | 旧基线 | 官方 / 现状 |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | **`Release`** |
| `ENABLE_FEXCORE_PROFILER` | `OFF` | **`True`** |
| `ENABLE_LTO` | `False` | `False` |
| `BUILD_TESTING` | `ON`（旧缓存残留） | **`False`** |
| `TUNE_CPU` | `native` | **`none`** |
| `RANGES_NATIVE` | `ON` | **`OFF`** |
| `OVERRIDE_VERSION/HASH` | 未传（stats 里 fex_version 为空） | 传 `FEX-2604-99-g86ff33b (86ff33b)` |

`scripts/build_fex.sh` 现在：

- 把上述参数集中成 `FEX_CMAKE_ARGS`，可用 `FEX_BUILD_TYPE` / `FEX_PROFILER` 覆盖做 A/B；
- 新增 `fex_configure()`：以「构建类型 | profiler | 版本标记 | triple」**签名**判断是否需要
  重新 configure，参数一变就重配 —— **修掉方案 §5.2 点名的旧缓存坑**
  （旧写法只看 `CMakeCache.txt` 是否存在，改脚本参数会被静默忽略，实测
  `-DBUILD_TESTING=False` 就这样失效过）；
- 优化等级断言按构建类型选变量（`Release → CMAKE_CXX_FLAGS_RELEASE`）。

## 2. 产物

| 产物 | 旧（RelWithDebInfo） | 新（Release，对齐官方） |
| --- | --- | --- |
| `libarm64ecfex.dll` | 41 811 968 B | **5 107 712 B** |
| `libwow64fex.dll` | 40 939 520 B | **4 579 328 B** |

CMakeCache 复核（`build/fex-pe/CMakeCache.txt`）：

```text
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG
ENABLE_FEXCORE_PROFILER:BOOL=True
ENABLE_LTO:BOOL=False
BUILD_TESTING:BOOL=False
TUNE_CPU:STRING=none
RANGES_NATIVE:BOOL=OFF
```

四个产物（两个 PE DLL + 两个 AArch64 UnixLib）全部构建通过并带
`WINEHUA-FEX-PROBE` 探针与 `__wine_unix_call_funcs` 导出断言。

## 3. 真机回归（run `p5e`）

换成 Release 对齐产物后重跑 `p3-wow64-ab`（30 s/项，同一 cube / 同一 DXVK legacy）：

| 测试 | frames | fps | avgMs | renderMs | presentMs |
| --- | --- | --- | --- | --- | --- |
| `p3-bench-x86-wowbox64` | 2425 | 82.97 | 12.0527 | 0.1296 | 11.7655 |
| `p3-bench-x86-fex`（Release 对齐产物） | 2361 | 81.10 | 12.3306 | 0.1634 | 11.7081 |
| `p3-bench-x64-native` | 2382 | 81.78 | 12.2274 | 0.0966 | 11.9770 |

与对齐前的 `p3c`（12.1538 / 12.2953 / 12.1590）相比在噪声范围内
（屏幕节拍 11.129 ms 依旧主导，见 `p3-p4-smoke-ab.md` §3.5）。

**所以：构建参数对齐是"该做的事"，但按方案 §5.2 的告诫，
不能把它当成提速手段；实测也没有提速。**

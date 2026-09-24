# 测试与回归

自动化设施在 `automation/` 和 `smoke/` 下，用法见 `automation/README.md`；
完整要求见 `docs/engineering/quality.md` 和 `docs/engineering/testing-cases.md`。
这里只留操作要点。

## 改了什么，就跑什么

提交前至少跑一轮相关套件；不确定跑什么时先跑 `core`（一分钟内出结果）。

| 改动范围 | 构建 | 套件 |
|---|---|---|
| `entry/src/main/ets/`（界面、服务） | `make NATIVE_ARCH=arm64-v8a hap` | core |
| `entry/src/main/cpp/`（合成、输入、渲染） | 同上 | core + wine-vulkan + dxvk |
| `thirdparty/wine/`（Wine 源码） | 完整 `make NATIVE_ARCH=arm64-v8a` | core + dxvk + d3d12 |
| DXVK legacy / modern | 完整构建 | dxvk / dxvk-modern-baseline |
| 只改 `smoke/tests/`、`smoke/suites/` | 不用重装 | `smoke.py build` 后直接 run |
| 只改判定 `automation/checks/` | 不用碰设备 | `smoke.py check <归档目录>` |

```bash
make NATIVE_ARCH=arm64-v8a hap                  # 1. 构建
python3 automation/smoke.py install             # 2. 装到设备
python3 automation/smoke.py run --suite core    # 3. 跑套件
```

## 硬约束

1. **设备端只跑不判**：设备端产出原始数据，通过还是失败由主机判定。
   能力探针报 `UNSUPPORTED` 是合法答案，不算失败。
2. **判定只读归档数据，不依赖设备现场**——守住这条，改判定规则不用重跑设备。
3. **产品不为测试让步**：产品代码不引用测试程序，测试 exe 不进 wine 的 `bin/`。
4. **套件要钉死档位**：声明了 `backend.d3d` 就一起声明 `backend.dxvk`。不声明会退回
   「设备当前设置」，而它由机型与系统版本决定——同一个套件在不同设备上测的不是一回事。
5. **`WINEDEBUG` / `WINEHUA_WINEDEBUG` 不能声明**：设备端读不到（前者被显式忽略，
   后者在环境变量应用之前就被读了），`smoke.py` 装载套件时直接拦下。Wine 日志看
   hilog 的 `WineChild-stderr`。
6. **用例要独立、可复现**：不依赖上一个用例的残留，不依赖时间/网络；改完先跑通再提交。
7. **自动化测试通过不等于功能可用**：涉及真实程序、输入操作、画面显示的改动，
   必须人工跑一遍验证。

设施本身是否正常：`python3 automation/smoke.py gate`（3 次复用环境 + 1 次全新环境跑 core）。

临时调试的覆盖开关（`--tests` / `--d3d` / `--env` / `--seconds` / `--inline`）见
`automation/README.md`。

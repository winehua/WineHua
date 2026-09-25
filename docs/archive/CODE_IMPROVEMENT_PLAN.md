# 代码改进实施计划（CODE IMPROVEMENT PLAN）

> 状态：部分实施后停止维护（2026-09-22 归档）。改进项 D（dxvk 双轨）已经落地、项 2（配置管线）已部分落地，但文末的执行状态表没有跟进更新。

> 日期：2026-07-31
> 分支：立项时基于 `feature/d3d8-virtual-display-compat`，该分支已合入 master（PR #47），改进项面向 master 有效
> 目的：落实 [SUBMODULE_MAINTAINABILITY.md](SUBMODULE_MAINTAINABILITY.md) 中识别的代码类改进（A/E/F/G/H/I），分阶段实施，每阶段有独立验收门禁。
> 前提文档：SUBMODULE_MAINTAINABILITY.md（风险分析 + 证据索引）。本计划是它的执行篇，风险依据、证据索引、改进项编号均以它为准。
> 关联文档：[PHASE2_DXVK_STATUS_MEMO.md](PHASE2_DXVK_STATUS_MEMO.md)（KNOWN_GOOD 分类系统）

---

## 0. 总原则

1. **每项改进一个原子 commit**，可单独回滚
2. **重构类改动禁止改行为**——用"黄金快照"和 A/B 验证兜底（见 §7）
3. **每阶段以"构建绿 + 测试绿"为 gate**，不过不进入下一阶段
4. **每阶段完成归档 KNOWN_GOOD**（签名 HAP + 测试数据，遵循 STATUS_MEMO 已立规则）

## 1. 阶段总览

| 阶段 | 内容 | 耗时 | 风险 | 产物 |
|------|------|------|------|------|
| 0 | 验证基线冻结 | 0.5 天 | 无 | 黄金快照 + KNOWN_GOOD 基线 |
| 1 | 门禁与回归基建 | 1-2 天 | 低 | CI job + 回归脚本 |
| 2 | Profile 单一数据源（E） | 2-3 天 | 中 | 注册表 + 一致性测试 |
| 3 | 诊断分离（G）+ 物理隔离（A） | 5-10 天 | **高** | 独立文件 + 上游 hook 化 |
| 4 | 配置握手（F）+ 收尾 | 3-5 天 | 中 | 握手校验 + 全量回归 |

**改进项对应**：A=物理隔离，E=Profile 单一数据源，F=env 目录+握手，G=诊断分离，H=CI submodule job，I=known-regressions 脚本，J=smoke 治理（随 1b 一并立规则）。

---

## 2. 阶段 0：验证基线冻结（0.5 天）

**目的**：让后续所有重构有"对拍照"，重构前的行为必须与重构后完全一致。

1. 完整构建当前分支，HAP 存档（记录 SHA256）
2. 写脚本 dump 全部 ~20 个 profile 的**完整 env 快照**（profile 名 → 它产生的所有 guest env 组合）——即"黄金快照"，阶段 2 的验收依据
3. 设备上跑一遍现有 smoke 套件（`dxvk/reuse` + `dxvk/clean`），记录：`angleRegressions`、`fallbackDetected`、cube FPS、Heaven 5min 帧率
4. 归档为 `docs/KNOWN_GOOD_<date>.md`

**完成定义**：基线文档含 HAP hash + env 黄金快照 + 全套测试数据。

---

## 3. 阶段 1：门禁与回归基建（1-2 天，低风险先做）

### 1a. CI submodule 一致性 job（改进 H）

```
.github/workflows/build.yml 新增 job: submodule-check（与 build 并行）
  ├─ ./scripts/check-submodules.sh          （已有脚本，直接入 CI）
  ├─ 分支约定检查：每个 submodule 指针在 .gitmodules 声明分支的历史中
  │   （wine=master, box64=main, virglrenderer=master, libepoxy=master,
  │    mesa=main, libdrm=OpenHarmony-6.0-Beta1, dxvk=dxvk-legacy-1.10.3）
  └─ 工作树干净检查：submodule 无未提交改动
```

**验证（构造失败场景）**：临时把某个 submodule 指针指到一个未推送的 commit → push 观察 CI 应红；恢复后应绿。验证完删除测试 commit。

### 1b. 已知坑回归脚本（改进 I）

```
scripts/qa/known-regressions.sh
  ├─ 01_env_protocol     → 验证 SPAWN\n 协议 env 注入生效（回归 0x887a0004）
  ├─ 02_crysis_dllpath   → 启动 Crysis 3 断言无 0xC0000135
  ├─ 03_safeflags        → 启动 steam_api64 目标断言无 AV
  └─ 输出：hilog 证据 + HAP hash 归档到带时间戳目录
```

**验证（脚本本身有效性）**：先在**已知正常**的当前 HAP 上跑应全绿；然后**临时破坏**一个修复（如去掉 ntdll/loader.c 的 DllPath 追加）重建 → 脚本应准确变红。确认脚本"能抓"后才算完成——一个抓不住问题的回归脚本比没有更危险。

**附带立规则（改进 J）**：smoke 新测试进新文件（按主题 present/shadow/format/sync），不在 `winehua_d3d11_smoke/main.c`（5862 行）里继续追加；每个测试在清单里记录"验证的不变式"（与补丁清单呼应）。

---

## 4. 阶段 2：Profile 单一数据源（改进 E，2-3 天）

**目标**：消灭 `napi_init.cpp` 与 `wine_launch.cpp` 的双份映射（SUBMODULE_MAINTAINABILITY §2.1）。

```
新增 entry/src/main/cpp/shadow_profiles.h    ← 唯一事实源
  ├─ struct ShadowProfile（name + 全部派生字段）
  ├─ constexpr ShadowProfile kShadowProfiles[]（一行一个 profile）
  └─ 三个函数：
      FindShadowProfile(name)
      ApplyProfileToVirglConfig(profile, VirglHostConfig*)   ← napi_init 用
      ApplyProfileToGuestEnv(profile)                        ← wine_launch 用

改造：
  napi_init.cpp:SetHostShadowProfile      → 删 ~150 行条件分支，改为查表
  wine_launch.cpp:AppendStableDesktopDxvkEnv → 删 ~40 行重复映射，改为查表
```

**验证（核心是黄金快照 diff）**：
1. 重构前：阶段 0 的黄金快照已存档
2. 重构后：重新 dump 全部 profile env 快照 → **与黄金快照逐字节 diff，必须为零**
3. `profile_config_test`（新增单元测试，CI 可跑、无需 GPU）：遍历每个 profile，断言 napi_init 侧输出 == wine_launch 侧输出——此测试从此成为防漂移的永久防线
4. 设备 smoke 回归

**风险与对策**：抄录映射时最容易抄错细节 → 靠黄金快照 diff 兜底。diff 非零就说明抄错，**不允许"人工确认一下算了"**。

**完成定义**：黄金快照 diff 为零 + profile_config_test 入库 + smoke 全绿。

---

## 5. 阶段 3：诊断分离 + 物理隔离（5-10 天，最高风险）

**核心纪律**：一次只搬一个函数族，每步"构建 + 设备验证"后才搬下一个。**3b 动刀前先打 tag**（如 `pre-shadow-isolation`），任何一步验证失败整体回退，不修补继续。

### 3a. 诊断分离（改进 G，2 天）

```
新增 thirdparty/virglrenderer/src/venus/vkr_winehua_perf.h
  ├─ struct vkr_winehua_perf_counters（全部 atomic 集中定义）
  ├─ VKR_PERF_INC(name) 宏（禁用时展开为空）
  └─ 采样/输出函数（perf summary JSON）

改造：
  vkr_queue.c       → 删文件顶部几十个 vkr_ohos_perf_* 全局 atomic，留 VKR_PERF_INC
  mesa vn_ring.c    → 512 槽 perf 数组移出 struct vn_ring，改为可选的独立分配
```

**验证**：重构前后在设备上跑 `perf` 模式 profile，**对比 perf summary JSON 输出逐字段一致**；热路径无性能回退（同场景 FPS ±5% 内）。

### 3b. 物理隔离试点：vkr_device_memory.c（改进 A，3-5 天）

```
新增 src/venus/vkr_winehua_shadow.c/.h
  ├─ 全部 shadow alloc / dirty-tracking / coverage-sort / msync 逻辑移入
  └─ 对外接口（≈10 个函数）

改造 vkr_device_memory.c：
  ├─ 恢复上游 ~57 行骨架
  └─ 保留 ≤3 处 hook：#ifdef __OHOS__ vkr_winehua_shadow_xxx(...)
```

**验证（A/B 双产物对比）**：
1. 重构前产物 A（阶段 0 的 HAP）与重构后产物 B，**同设备、同场景、同 profile** 跑：
   - cube：FPS 差异 ≤5%、无视觉差异（截帧对比可选）
   - Heaven 5 分钟：无 camera rollback、帧率曲线一致
   - shadow 脏页路径：perf 输出字节级一致
2. smoke 全绿
3. **完成定义**：`vkr_device_memory.c` 与上游文件的 diff 中 **WineHua 引用从 46 处降到 ≤5 处**

### 3c. 物理隔离扩展：vkr_queue.c / vn_ring.c（3-5 天）

同 3b 方法，优先级：vkr_queue.c → vn_ring.c。每步独立 commit + 独立 A/B 验证。

---

## 6. 阶段 4：配置握手 + 收尾（3-5 天）

### 4a. guest/host 配置握手（改进 F 第二步）

```
现状：fingerprint 只做 host 侧自检（graphics_broker.cpp:1205）
目标：guest 侧 DXVK 把实际生效的 env 组合回传 host，host 对比期望值
  ├─ 不匹配 → WL-ERR 级日志（不是静默跑）
  └─ 复用已有 vtest 扩展通道（VCMD_WINEHUA_PRESENT 同款机制）
```

**验证（构造失败场景）**：正常 profile 跑 → 无噪音；人为让 guest env 与 host 配置不一致（如改 guest 侧 DXVK_WINEHUA_PRECISE_SHADOW）→ 应出现明确错误日志。确认"能报错"后完成。

### 4b. env var 目录 + dxvk 双轨决策文档（F-1 + D，1 天）

- `docs/ENV_VAR_CATALOG.md`：~30 个 env var —— 读取者（guest DXVK / host virglrenderer / box64 / 主仓库）、默认值、由哪个 profile 字段派生、引入版本
- dxvk 双轨通道决策记录：`dxvk-legacy-1.10.3`（产品，只收 cherry-pick 的上游 bugfix，记录来源 commit）+ `feature/dxvk-modern`（2.x 重写试点，独立 manifest profile）；**禁止两条通道在同一分支共存**

### 4c. 全量回归收尾（1 天）

完整构建 → 全部 smoke（x64+x86）→ 双设备（910 tablet + 920 phone）→ 归档 KNOWN_GOOD 终版。

---

## 7. 验证总纲

### 7.1 验证金字塔（从轻到重，全部走一遍才叫完成）

```
L0 本地无 GPU 可跑            profile 黄金快照 diff（零差异）
   ├─ profile_config_test（双侧输出一致性断言）
   └─ bash -n / 编译期检查
L1 CI 自动                    构建绿 + submodule job 绿
L2 设备 smoke（每次部署必跑）  dxvk/reuse + dxvk/clean：
   ├─ angleRegressions=0
   └─ fallbackDetected=false
L3 设备基准场景                cube FPS 对比（±5%）
   ├─ Heaven 5min（无 camera rollback）
   └─ perf 输出 JSON 逐字段一致（诊断/隔离类改动专属）
L4 机制验证（新功能类改动）    构造失败场景 → 必须报错（CI 红 / 握手报错 / 回归脚本变红）
```

### 7.2 三类改动的验证侧重

| 改动类型 | 验证侧重 | 判定 |
|---------|---------|------|
| **重构类**（E/A/G，不改行为） | L0 黄金快照 diff + L3 性能对比 | 快照零差异；FPS ±5% 内 |
| **基建类**（H/I） | L4 构造失败场景 | 故意破坏 → 机制抓住 → 恢复 → 绿 |
| **机制类**（F 握手） | L4 构造不匹配 | 预期错误日志出现 |

### 7.3 门禁通过定义

- **构建**：`make NATIVE_ARCH=arm64-v8a` 全链路绿，产出签名 HAP
- **smoke**：`dxvk/reuse` + `dxvk/clean` 全绿，`angleRegressions=0`、`fallbackDetected=false`
- **性能**：cube/Heaven 与阶段 0 基线对比，FPS 波动 ≤5%，无 camera rollback
- **行为等价**（仅重构类）：黄金快照逐字节一致、perf JSON 逐字段一致

### 7.4 回滚边界

- 每阶段一个 commit → `git revert` 即回滚
- 阶段 3 的物理隔离是唯一"动刀大"的阶段：**3b 前打 tag**（如 `pre-shadow-isolation`），该阶段任何一步验证失败就整体回退，不修补继续

---

## 8. 启动顺序建议

阶段 0（0.5 天）→ 阶段 1a + 1b（CI job 和回归脚本，为阶段 2/3 的验证提供基础设施）→ 阶段 2（Profile 集中化，黄金快照机制验证后就是顺手的事）→ 阶段 3（留足时间，一次一个函数族）→ 阶段 4。

---

## 9. 执行状态追踪

| 阶段 | 任务 | 状态 | 完成日期 | 验证证据 |
|------|------|------|---------|---------|
| 0 | 基线冻结（HAP hash + 黄金快照 + 测试数据） | ⬜ | | |
| 1a | CI submodule job | ⬜ | | |
| 1b | known-regressions.sh + smoke 新文件规则 | ⬜ | | |
| 2 | Profile 注册表 + 黄金快照 diff + profile_config_test | ⬜ | | |
| 3a | 诊断分离（vkr_winehua_perf.h + vn_ring 数组移出） | ⬜ | | |
| 3b | 物理隔离 vkr_device_memory（tag 先行） | ⬜ | | |
| 3c | 物理隔离 vkr_queue / vn_ring | ⬜ | | |
| 4a | guest/host 配置握手 | ⬜ | | |
| 4b | ENV_VAR_CATALOG + dxvk 双轨决策 | ⬜ | | |
| 4c | 全量回归 + KNOWN_GOOD 终版归档 | ⬜ | | |

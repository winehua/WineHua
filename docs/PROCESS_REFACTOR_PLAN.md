# 进程启动与环境变量重构规划

> 更新日期: 2026-08-26
> 前置阅读: `docs/PROCESS_SPAWNING.md`(现状全景,含全部 file:line 证据)

## 背景与判断

现有"进程启动路径 + 环境变量拼接"能工作,但维护成本高。复杂度分两类:

- **本质复杂度**(重构消不掉,只能封装): NCP 子进程不继承 environ、fd 必须走 fd 传递、virgl host 独立体系、手机 fork 兜底、wineserver 精简基线兜底 loader 自启。
- **意外复杂度**(可以消灭): 双基线定义、字符串拼接口令靠人肉遵守、策略散落两地两语言。

本规划只动后者。

## 病灶

1. **基线环境两份定义,靠时序 hack 维持一致**
   - `BuildWineEnv`(wine_env.cpp:35,主进程,string vector)与 `setup_wine_env`(wine_child.cpp:269,子进程,setenv)各自硬编码 WINEPREFIX/WINEDLLDIR/PATH/Box64 调优。
   - `WineserverMain` 对 `__env` 中 `BOX64_DYNAREC_*` 二次重放(wine_child.cpp:778-785)就是两份定义时序打架的产物。
2. **env 是裸 `vector<string>` + 管道符拼接,规矩靠人肉**
   - "剔除含 `|`/`\n`、剔除 fd 变量"规则散在 wine_env.cpp:376-448、ohos_broker.c:158 等多处,各写一遍。
   - `UpsertEnvLine`/`SerializeEnvToEntryParams`/`AppendMissingEntryParamsEnvOverrides` 围绕同一字符串数组,覆盖/补缺/强制语义全靠函数名约定。
3. **策略散落两地两语言**
   - `AppendD3dBackendEnv`(wine_env.cpp:152)、`AppendStableDesktopDxvkEnv`(wine_launch.cpp:367)、兼容档位(wine_launch.cpp:318-364)、game 模式 perf 开关组(WineEnvService.ets:1317-1447,**ArkTS 侧**)。
   - 11 个 `BOX64_DYNAREC_*` 键清单 ArkTS(Box64Dynarec.ets)与 native(AppendBox64PerfStrings)各一份。
4. **spawn 点各自手工拼 entryParams**
   - wineserver(wine_launch.cpp:510-516)、wineboot(:608-623)、explorer(:788-803)、broker HandleRequest(broker.cpp:177-240)四处重复处理 homeDir 前缀/WINEPREFIX 权威/audio fd。

## 目标设计

### 抽象 1: `EnvSpec` —— 结构化环境

有序 map(保插入序,`set()` 即 upsert)+ 统一 `Serialize()`。三件事收口到唯一位置:

- `|`/`\n` 过滤
- fd 变量过滤谓词 `IsPerProcessFdVar()`(C++ 与 ohos_broker.c 共用头文件,消除双胞胎实现)
- `|__env=K=V` 序列化

删除 `UpsertEnvLine`/`SerializeEnvToEntryParams`/`AppendMissingEntryParamsEnvOverrides`,调用方改 `env.set(k,v)`/`env.mergeFrom(other)`。

### 抽象 2: 单一基线源

基线 K=V 表放共享头文件(先例: `AppendBox64PerfStrings` 已在 wine_env.h 被子进程复用)。主进程 `BuildWineEnv` 从表构造 EnvSpec;`setup_wine_env` 从表 setenv。两份定义合一,WineserverMain 重放 hack 删除(覆盖顺序由 EnvSpec 语义保证)。

### 抽象 3: EnvProfile 管线

策略函数集中到一个文件(env_profiles.cpp),顺序显式:

```cpp
EnvSpec BuildSessionEnv(SessionProfile p) {
    EnvSpec e = BaselineEnv(p);            // L0+L1+L2
    ApplyGraphicsEnv(e, p);                // L5,graphics_broker 提供
    if (p.audioFd >= 0) ApplyAudioEnv(e);  // L3
    ApplyDesktopEnv(e, p);                 // L4 + 桌面收口
    ApplyD3dBackendEnv(e, p);              // dxvk/vkd3d
    ApplyCompatEnv(e, p);                  // 白名单档位
    e.mergeFrom(p.perApp);                 // per-app 优先级最高
    return e;
}
```

`AppendStableDesktopDxvkEnv`、兼容档位从 wine_launch.cpp 迁入。spawn 点不再各自追加,声明 profile 拿成品。

### 抽象 4: `SpawnRequest` + `Spawner` —— 意图与机制分离

```cpp
struct SpawnRequest {
    SpawnKind kind;      // Wineserver/Wineboot/Explorer/Exe/GuestElf/HostElf
    std::vector<std::string> argv;
    EnvSpec env;         // 仅增量
};
```

四层职责:

```
调用方                SpawnRequest(数据)        Spawner(推导+路由)          wine_child(执行)
─────────────        ──────────────────       ──────────────────────     ─────────────────
"启动什么,           kind/argv/env 增量        + homeDir 前缀              基线 env 重建
 带什么 env 增量"                             + WINEPREFIX 会话权威       __env 覆盖
                                              + audio fd 挂载(kind 推导)  fd 变量重写
                                              + fd 变量过滤               dlopen box64
                                              + entryParams 组装
                                              + 路由: 主进程→NCP/broker
                                                wine子进程→socket→broker
                                                手机→fork 兜底
```

**不在请求里的**(刻意): WINEPREFIX/homeDir(会话单例,broker 权威)、fd(由 kind 推导)、入口符号/基线精简程度(kind 推导)、路由(取决于所在进程与形态)。调用方无法越权指定——不变量从"靠自觉"变成"类型边界内不可能犯错"。

virgl host **不进这套体系**(独立进程、独立安全边界、IPC parcel 配置通道),保持现状。

### 明确不动

- virgl 的 ClearGuestGraphicsEnv + 白名单体系(virgl_child.cpp:265/315)
- 手机 fork 分发层(ncp_dispatch.cpp / phone_process.cpp)
- fd 传递机制本身(NCP fdList / SCM_RIGHTS)
- Wine 上游逻辑(env.c 注册表环境块等)

## 执行计划

每步独立提交、独立回归;1-4 步**纯行为保持**,行为变化单独提交并说明。

| 步 | 内容 | 风险 | 门禁 |
|---|---|---|---|
| 1 | EnvSpec + 统一序列化/过滤谓词,机械替换三个序列化点 | 低 | 构建 + 设备起桌面/notepad |
| 2 | 基线表单源化,删 setup_wine_env 重复与 WineserverMain 重放 hack | 中 | Pad+PC 双设备回归(含游戏启动) |
| 3 | Profile 管线: 策略集中,spawn 点改用 BuildSessionEnv | 中 | 同上 + dxvk/vkd3d 后端各跑一次 |
| 4 | SpawnRequest/Spawner,四个 spawn 点收口 | 中 | 同上 + wine→wine 子进程(launcher 类) |
| 5 | (可选,独立评估) wineserver/wineboot 走 broker;ArkTS 11 键清单单源化 | 中高 | 冷启动/重启/恢复出厂全流程 |

构建: `make NATIVE_ARCH=arm64-v8a`(唯一合法手段);回归设备: Pad(192.168.1.6)+ PC 2in1(192.168.1.8)。

## 进度记录

- [x] 第 1 步: EnvSpec (b04e341)
- [x] 第 2 步: 基线单源 (18b6f5a)
- [x] 第 3 步: Profile 管线 (f3370b0)
- [x] 第 4 步: SpawnRequest/Spawner (fed07ec)
- [x] 第 5 步: wineserver/wineboot 走 broker (native 侧)
  - 连带修复: broker 就绪竞态 — StartBrokerServer 原以 socket 文件存在为就绪,
    但 bind 先于 listen,connect 会拿 ECONNREFUSED 致紧随的 wineserver spawn 失败
    ("启动失败");改为真实 connect 探测 (HandleRequest 对探测 EOF 降 INFO)。
  - 连带修复: wine loader 自启 wineserver (ohos_broker_spawn_wineserver) 此前
    走 `binDir|wineserver|-f|-p` 经 Main 会被当 wine loader argv 错解析,现由
    Main 截获 argv[0]=="wineserver" 转入本体,该兜底路径首次真正可用。
  - 已知副作用: wineboot 经 broker 登记会短暂出现在任务列表 (可接受)。
- [x] ArkTS 11 键清单单源化: 采用 "policy 归 ArkTS / native 只留机制" 方案
  (而非原设想的预设表搬 native — 保留 UI 档位/未来逐键微调的灵活性):
  - Box64Dynarec.ets 为可调键清单与各档取值唯一来源, 头部注释写明所有权;
  - native 只保留出厂基线 (Box64PerfTable, "偏离 box64 编译默认"的安全底)
    + 前缀门 (FilterCompatLines), 两侧注释明确不复制档位表;
  - EntryAbility d3d_env 白名单删 6 键枚举, 改 BOX64_DYNAREC_ 前缀正则
    (与 native 前缀门同一约定) — Box64 键清单不再有两处列举;
  - WEAKBARRIER 散落拷贝 (Index/WineEnvService/SmokeRunner) 保留不动: 那是
    runWineProgram 直启链 (native 桌面 clamp 覆盖不到) 的同款约束, 属另一
    路径的 policy 而非重复真相, 各处已带 Venus ring 定序注释。

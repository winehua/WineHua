# HGS 共享运行时：阶段 0 实施方案与后续扩展设计

> 最后核实：2026-11-04
> 适用仓库：`/workspace/WineHua-root/WineHua`（外层分支 `feature/hgs-har-scaffold`），
> hgs submodule（`main`，当前 `7255c57`）
> 状态：**阶段 0 已完成；阶段 1 基本完成；阶段 2 W1/W2 已完成（真机待回归）**。
> 已落地：P0.0–P0.4（公共契约 + 门面）、P1.1 NAPI canonical/回调收口、
> P1.2 typed sink、P1.3 setProcessEnvironment、N1 原生输入、W1 窗口管理器
> core、W2a 事件接线/NAPI、W2b 默认 ArkTS host（Service/Ability/Page）。
> 签名 HAP 因本机未挂载签名材料未出包，原生编译已验证。
> 后续：N2 手势、P1.4 所有权/seam/smoke 收口、W3 BetterSH 接入。

## 0. 一句话

保持现有行为完全不变，先为 HGS 建立一层**不依赖 NAPI / EGL / OHNativeWindow / Wine 的 C++ 公共契约**，
并用 `HgsInstance` 门面把调用转发到现有单例。阶段 0 以新增为主，只做三处行为中性的
小改动（事件枚举迁移、`IsRunning()` 访问器、`CreateRenderer()` 返回 `bool`），
不改 ArkTS、不改 NAPI 签名、不改 `.d.ts`、不改合成行为。

另外两项后续需求单独设计，阶段 0 只预留配置与边界：

- **原生输入**（目标：ArkTS 侧零输入事件注册，见 §14）。
- **HGS 内置窗口管理器**（目标：BetterSH 删光 Wayland 代码后只依赖 HGS，见 §15）。

## 1. 现状核对

### 1.1 已经具备（上个方案中的部分目标已提前完成）

| 能力 | 现状 | 证据 |
|---|---|---|
| HGS 作为 HAR | 已有 scaffold，`entry` 通过 `file:../hgs` 依赖 | `hgs/oh-package.json5`、`entry/oh-package.json5:11` |
| 合成器内部分层 | `FramePlanner`/`FrameBlitter`/`PresentedFrame` 已拆分 | `hgs/src/main/cpp/compositor/frame/frame_pipeline.h:12-40` |
| 事件总线和 enum 化 | `ToplevelEventBus` 已收口 21 种事件名与 JSON 模板 | `hgs/src/main/cpp/compositor/toplevel/toplevel_event_bus.h:42-121` |
| 输入线程分层 | `InputQueue`/`InputStateTracker`/`InputInjector`/`InputResolver` 已拆分 | `hgs/src/main/cpp/input/input_manager.h:15-32` |
| surfaceId 64 位 | ArkTS 侧已经是 `BigInt(sid)` | `entry/src/main/ets/pages/WineWindow.ets:34` |
| 共享 EGLDisplay | 已有 | `hgs/src/main/cpp/graphics/egl_renderer.h:24` |
| 每 toplevel 独立 renderer | `PluginManager::CreateRenderer` | `hgs/src/main/cpp/bridge/plugin_manager.h:35` |

### 1.2 仍然缺失（阶段 0 要补的）

| 缺失项 | 说明 |
|---|---|
| 公共 API 目录 | `hgs/src/main/cpp/api/` 不存在 |
| `HgsInstance` | 生命周期/配置/事件/Surface/输入没有统一门面，宿主直接调各单例或 NAPI |
| 类型化事件契约 | `ToplevelEventBus` 内部已 enum 化，但宿主侧仍是 `(id, eventName, json)` 字符串 |
| Surface generation | `createRenderer/resizeRenderer/destroyRenderer` 没有 generation，旧 destroy 事件可能误杀新 renderer |
| 宿主回调接口 | 状态/toplevel 回调仍以 NAPI TSFN 为唯一出口 |
| 依赖边界 | `napi_init.cpp` 直接装配所有单例；`seam/*` 与 `smoke/egl_renderer.cpp` 仍编进 `libhgs.so` |

### 1.3 与上个方案的偏差（实施前必须修正）

1. **HGS 内部重构已完成大半**：原计划的阶段 1 第 1–5 条（FramePlanner/Blitter、事件总线、输入队列、CompositorLayer、锁外合成）
   已经落地。阶段 1 剩余工作主要是「所有权收进实例 + NAPI 下沉」，而不是重写。
2. **`HgsConfig` 默认值不能直接下发**。WineHua 现在的顺序是：
   `WineWindowManager.ets:226 setOutputSize(...)` 先于 `WineEnvService.ets:917 startServer(...)`，
   模式由 `applyModePolicy()` 单独设置。若 `HgsInstance::Start` 按原方案的
   `1280x720` / `PerToplevel` 默认值下发，会覆盖宿主的真实屏幕尺寸和桌面模式。
   阶段 0 的契约必须用「0 / Unspecified = 不修改」。
3. **`make test` 已经失效**：`Makefile:441+` 仍指向 `entry/src/main/cpp/compositor/...`，
   而 compositor 已迁入 `hgs/src/main/cpp/`。新增纯函数测试前必须先修包含路径与源文件路径。
4. **surfaceId 的 BigInt 已完成**，原方案中「Number → BigInt」不再是待办。
5. **`HgsSurface` 的 native 回调分支仍是占位**（默认 `false`），
   见 `hgs/src/main/ets/components/HgsSurface.ets:56-59`、`:698-710`。
   阶段 0/1 继续走 ArkTS `XComponentController` 生命周期路径，不切换。

## 2. 本轮范围

### 做

- 新增 `hgs/src/main/cpp/api/`：纯 C++17 头 + 两个实现文件（endpoint/instance）。
- 事件枚举单一来源：从 `toplevel_event_bus.h` 迁到 `api/hgs_events.h`，
  原位置保留 `using` 别名，现有 90 处 `ToplevelEventType::` 调用零改动。
- `HgsInstance` 门面：生命周期、配置、事件、Surface、输入、窗口操作、过渡兼容队列。
- 两个行为中性小改动：`WaylandServer::IsRunning()` 访问器、
  `PluginManager::CreateRenderer()` 返回 `bool`（旧调用方忽略返回值）。
- 纯函数宿主测试：socket 路径拆分、事件名映射。
- 修复 `Makefile` 的 host test 路径（可独立提交）。

### 不做

- 不改 `napi_init.cpp` 的导出表，不改 `types/libhgs/Index.d.ts`，不改任何 `.ets`。
- 不把旧 NAPI 转发到 `HgsInstance`（阶段 1 做，见 §7）。
- 不拆 `seam/`、不拆 `smoke/`、不动 zero-copy / VirGL / Venus。
- 不做 HarmonyBox / BetterSH 接入。
- 不实现原生输入回调与窗口管理器（设计见 §14/§15，代码在阶段 1/2）。
- 不启用 `DesktopRoot`、`NativeVSync`、`NativeImage`、pointer lock。
- 不改变 `HgsSurface` 的 native callback 开关。

## 3. 文件清单

### 新增

```text
hgs/src/main/cpp/api/hgs.h                  # 伞头：types/events/endpoint/host/instance
hgs/src/main/cpp/api/hgs_types.h            # ID/配置/事件 payload/输入/宿主请求
hgs/src/main/cpp/api/hgs_events.h           # 事件枚举 + 名称双向映射（单一来源）
hgs/src/main/cpp/api/hgs_endpoint.h         # socket 路径拆分 + ClientEnvironment（纯函数）
hgs/src/main/cpp/api/hgs_host.h             # IHgsHost 回调接口
hgs/src/main/cpp/api/hgs_instance.h         # HgsInstance 门面
hgs/src/main/cpp/api/hgs_endpoint.cpp       # 路径/环境变量纯函数实现（host test 可链接）
hgs/src/main/cpp/api/hgs_instance.cpp       # 门面实现（唯一依赖现有单例的文件）
host_tests/hgs_endpoint_test.cpp            # 路径/环境变量纯函数测试
host_tests/hgs_event_map_test.cpp           # 事件名双向映射测试
docs/architecture/hgs-shared-runtime-phase0.md  # 本文
```

### 修改

```text
hgs/src/main/cpp/compositor/toplevel/toplevel_event_bus.h
    - 删除本地 enum/ToplevelEventName，改为 #include "hgs_events.h"
    - 加 `using hgs::ToplevelEventType;` / `using hgs::ToplevelEventName;`（过渡别名）
hgs/src/main/cpp/compositor/wayland_server.h
    - 新增 `bool IsRunning() const { return running_; }`（只读访问器）
hgs/src/main/cpp/bridge/plugin_manager.h/.cpp
    - `CreateRenderer` 返回 `bool`（true = native window + EGL 初始化成功）
hgs/src/main/cpp/CMakeLists.txt
    - include_directories 增加 `${NATIVERENDER_ROOT_PATH}/api`
    - add_library(hgs SHARED) 增加 `api/hgs_endpoint.cpp`、`api/hgs_instance.cpp`
Makefile
    - host test 的 `-I` 与源文件路径改为 `hgs/src/main/cpp`（见 §10.1）
    - 增加两个新测试的编译/执行行
```

不新增链接库，不新增 NAPI 导出，不新增 `.d.ts`。

§14（native input）与 §15（窗口管理器）的新增文件属于阶段 1/2，不在阶段 0 文件清单内。

## 4. 公共 API 设计

### 4.1 设计原则

1. **纯头**：`hgs_types.h` / `hgs_events.h` / `hgs_endpoint.h` / `hgs_host.h` /
   `hgs_instance.h` 只允许 `<cstdint> <cstddef> <cstring> <string> <string_view> <vector> <memory>`。
   出现 `napi/native_api.h`、`hilog`、`wayland-*`、`EGL/*`、`native_window/*` 即视为设计错误。
2. **门面不持业务状态**：阶段 0 的 `HgsInstance::Impl` 只持 `IHgsHost*`、
   `running_`、`generation` 计数与 `id -> (token, generation)` 表；
   其余全部转发到 `WaylandServer` / `PluginManager` / `InputManager`。
3. **默认值不改变现状**：`OutputConfig::width/height = 0`、
   `CompositionMode::Unspecified` 表示「不修改宿主已有设置」。
4. **事件枚举单一来源**：public enum 就是内部 enum，避免两套映射漂移。
5. **生命周期分离**：`DetachSurface` 只负责 XComponent surface（= 现 `destroyRenderer`）；
   `DestroyToplevel` 负责 Wayland toplevel 收尾（= 现 `destroyToplevel`，额外释放 pointer lock）。
6. **线程契约写在头文件注释里**，见 §4.6。

### 4.2 `hgs_types.h`

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hgs {

using ToplevelId       = uint32_t;
using SurfaceToken     = uint64_t;   // OHOS surfaceId；ArkTS 侧 bigint
using SurfaceGeneration = uint32_t;  // 0 = 无效；由 AttachSurface 分配

enum class CompositionMode : uint8_t {
    Unspecified = 0,   // 不修改宿主当前模式（阶段 0 默认）
    PerToplevel = 1,   // 每个 toplevel 一个 OHOS 窗口（WineHua/HarmonyBox/BetterSH 现态）
    DesktopRoot = 2,   // 全部合成到一个 root surface（WineHua 平板模式）
};

// 输入后端（阶段 0 只记录，阶段 1 生效）：
//   Auto   — 由 HGS 按平台/构建设置选择；当前等价 ArkTS，目标是 Native。
//   ArkTS  — 现有 .onTouch/.onMouse/.onAxisEvent/.onKeyEvent 路径。
//   Native — OH_NativeXComponent 回调直通 C++，ArkTS 不注册任何输入事件
//            （Blender Index.ets 同款形态，见 §14）。
enum class InputBackend : uint8_t {
    Auto = 0,
    ArkTS = 1,
    Native = 2,
};

struct OutputConfig {
    // 0 = 不修改。WineHua 在 startServer 前已按屏幕/scale 算好尺寸，
    // Start 不能再下发 1280x720 覆盖它。
    int32_t width  = 0;
    int32_t height = 0;
    int32_t refreshMilliHz = 60000;  // 预留；阶段 0 未使用
    double  scale = 0.0;             // 0 = 不修改；对应现 setDisplayScale（当前 no-op）
};

struct FeatureConfig {
    // 声明式能力开关。阶段 0 只记录，不改变注册行为：
    // shm/subsurface/viewporter 当前恒开；其余按需在阶段 1 生效。
    bool shm = true;
    bool subsurface = true;
    bool viewporter = true;
    bool xdgPopup = true;
    bool dataDevice = false;      // BetterSH 接入时置 true
    bool relativePointer = false;
    bool textInput = false;
    bool externalSurface = false; // zero-copy / VirGL / Venus
    bool winehuaToplevel = false; // WineHua 专用协议
};

struct HgsConfig {
    std::string socketPath;  // 必须是绝对路径；Start 校验
    OutputConfig output;
    CompositionMode composition = CompositionMode::Unspecified;
    FeatureConfig features;
    InputBackend inputBackend = InputBackend::Auto;  // 阶段 0 仅记录

    // true = 沿用旧行为（WaylandServer::Start 内部 setenv）。
    // 阶段 0 仅记录；目标默认 false，由宿主把 ClientEnvironment() 传给子进程。
    bool setProcessEnvironment = true;
};

struct SurfaceBinding {
    ToplevelId toplevelId = 0;
    SurfaceToken token = 0;
    SurfaceGeneration generation = 0;  // AttachSurface 返回时填充分配值；0 = 失败
};

// 输入：与 InputManager 现有参数逐项对应（input_manager.h:70-81）
enum class PointerAction : int32_t { Press = 1, Release = 2, Move = 3 };

struct PointerEvent {
    ToplevelId hintToplevel = 0;   // PerToplevel 模式由宿主指定
    PointerAction action = PointerAction::Move;
    double x = 0, y = 0;           // 物理像素
    uint32_t button = 0;           // evdev code
    double rawDeltaX = 0, rawDeltaY = 0;
    bool fromMouse = false;
};

struct KeyEvent {
    ToplevelId toplevelId = 0;
    int32_t evdevCode = 0;
    bool pressed = false;
};

struct ScrollEvent {
    ToplevelId hintToplevel = 0;
    int32_t axis = 0;              // 0=垂直 1=水平
    double value = 0;
    int32_t scrollStep = 0;
    double x = 0, y = 0;
};

// 宿主窗口操作请求：阶段 0 仅定义，阶段 1 由 xdg_shell 产生。
enum class HostRequestType : uint8_t {
    Move, Resize, Maximize, Unmaximize, Minimize, Fullscreen, Close,
};

struct HostRequest {
    HostRequestType type = HostRequestType::Move;
    ToplevelId toplevelId = 0;
    uint32_t edges = 0;
};

// 阶段 2 起由事件总线填充；阶段 0 仅作为契约占位。
struct ToplevelInfo {
    ToplevelId id = 0;
    uint32_t clientPid = 0;
    uint64_t surfaceKey = 0;
    std::string title;
    std::string appId;
    ToplevelId parentId = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool visible = true;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
};

} // namespace hgs
```

### 4.3 `hgs_events.h`

把 `toplevel_event_bus.h:42-100` 的枚举与 `ToplevelEventName` 原样迁入 `namespace hgs`，
并增加显式数值与 `Unknown = 0`：

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace hgs {

enum class ToplevelEventType : uint32_t {
    Unknown = 0,
    Created = 1, Destroyed = 2,
    PopupHide = 3, PopupMove = 4, PopupShow = 5, PopupResize = 6,
    Argb = 7, Resize = 8, Surface = 9, Title = 10, Limits = 11,
    Maximized = 12, Unmaximized = 13, Fullscreen = 14, Unfullscreen = 15,
    Minimized = 16, Restored = 17,
    MoveStart = 18, MoveEnd = 19,
    Modal = 20, DesktopRoot = 21,
};

// 两个映射函数在头内 inline 定义（host test 直接编译，无 .cpp 依赖）。
inline const char* ToplevelEventName(ToplevelEventType evt) { /* switch，文本逐字 */ }
inline ToplevelEventType ToplevelEventTypeFromName(const char* name) { /* 反查 */ }
inline constexpr ToplevelEventType kAllToplevelEvents[] = { /* 除 Unknown 外全部 */ };

} // namespace hgs
```

`toplevel_event_bus.h` 顶部改为：

```cpp
#include "hgs_events.h"        // api/ 已在 include_directories 中
using hgs::ToplevelEventType;   // 过渡别名：现有 90 处调用零改动
using hgs::ToplevelEventName;   // 过渡别名：bus/test 现有调用零改动
```

`ToplevelEventName` 的 `switch` 增加 `case Unknown: return "unknown";`，
其余分支文本逐字保留（ArkTS 消费字符串是红线）。

### 4.4 `hgs_endpoint.h`

纯函数，可被 host test 直接编译：

```cpp
#pragma once
#include <string>
#include <vector>

namespace hgs {

struct SocketEndpoint {
    bool valid = false;
    std::string directory;    // XDG_RUNTIME_DIR
    std::string displayName;  // WAYLAND_DISPLAY（basename）
};

// 规则：绝对路径、非空 basename、不允许以 '/' 结尾。
SocketEndpoint SplitSocketPath(const std::string& socketPath);

// 返回 {"XDG_RUNTIME_DIR=<dir>", "WAYLAND_DISPLAY=<name>"}；
// 路径无效时返回空 vector。
std::vector<std::string> BuildClientEnvironment(const std::string& socketPath);

} // namespace hgs
```

实现放 `hgs_instance.cpp` 或独立 `hgs_endpoint.cpp`（建议独立，便于 host test 链接；
两者都纯标准库）。**决策**：单独 `api/hgs_endpoint.cpp`，CMake 加一行，host test 直接链接。

### 4.5 `hgs_host.h`

```cpp
#pragma once
#include <string_view>
#include "hgs_types.h"
#include "hgs_events.h"

namespace hgs {

struct ToplevelEvent {
    ToplevelId id = 0;
    ToplevelEventType type = ToplevelEventType::Unknown;
    // 与现有 ToplevelEventBus 的 JSON 模板逐字一致；仅在回调期间有效。
    std::string_view json = "{}";
};

class IHgsHost {
public:
    virtual ~IHgsHost() = default;

    // 线程：Wayland 事件循环线程。禁止在回调里调用 Stop()（会 join 自己）。
    virtual void OnToplevelEvent(const ToplevelEvent& event) = 0;

    // 线程：Wayland 事件循环线程。
    virtual void OnStateChanged(std::string_view state) = 0;
};

} // namespace hgs
```

### 4.6 `hgs_instance.h`

```cpp
#pragma once
#include <memory>
#include "hgs_host.h"
#include "hgs_types.h"

namespace hgs {

class HgsInstance {
public:
    // 过渡形态：阶段 0 复用现有单例。阶段 1 改成 Create(config) 真正持有状态。
    static HgsInstance& Instance();
    static uint32_t ApiVersion();   // HGS_API_VERSION
    std::string LastError() const;  // 失败原因；空串 = 无错误

    HgsInstance(const HgsInstance&) = delete;
    HgsInstance& operator=(const HgsInstance&) = delete;

    // 主线程/UI 线程。顺序：校验路径 -> 安装 host -> 下发 output/mode -> Start。
    bool Start(const HgsConfig& config);
    // 幂等；会 join Wayland 线程。禁止在 IHgsHost 回调内调用。
    void Stop();
    bool IsRunning() const;

    std::string WaylandDisplayName() const;          // 无效路径返回空
    std::vector<std::string> ClientEnvironment() const;  // 见 §4.4

    // 主线程，Start 前调用。host 必须活到 Stop 之后。
    void SetHost(IHgsHost* host);

    void SetOutputSize(int32_t w, int32_t h);
    void SetDesktopMode(bool on);
    // 阶段 0 仅记录；阶段 1 起切换 ArkTS/Native 输入后端（§14）。
    void SetInputBackend(InputBackend backend);
    InputBackend GetInputBackend() const;
    ToplevelId DesktopRootId() const;

    // Surface 生命周期（UI 线程）。失败返回全 0（generation == 0）。
    SurfaceBinding AttachSurface(ToplevelId id, SurfaceToken token);
    // generation 必须匹配；0 = 通配（兼容阶段 1 的旧 NAPI 路径）。
    bool ResizeSurface(ToplevelId id, int32_t w, int32_t h, SurfaceGeneration gen);
    bool DetachSurface(ToplevelId id, SurfaceGeneration gen);

    // 输入（NAPI/JS 线程）：入队后由 Wayland 线程注入。
    void SendPointerEvent(const PointerEvent& event);
    void SendKeyEvent(const KeyEvent& event);
    void SendScrollEvent(const ScrollEvent& event);

    // 宿主窗口操作（主线程）
    void NotifyToplevelResize(ToplevelId id, int32_t w, int32_t h, bool resizing);
    void SendToplevelClose(ToplevelId id);
    void RaiseToplevel(ToplevelId id);
    void SetToplevelVisible(ToplevelId id, bool visible);
    void DestroyToplevel(ToplevelId id);   // = 现 NAPI destroyToplevel（含 pointer lock 释放）
    ToplevelId FindToplevelAt(int32_t x, int32_t y) const;

    // 过渡兼容：pending 队列（阶段 2 迁到 WineHua host adapter）
    void SetPendingToplevel(ToplevelId id);
    ToplevelId TakePendingToplevel();      // = 现 getCurrentToplevelId（出队）
    void CancelPendingToplevel(ToplevelId id);

private:
    HgsInstance();
    ~HgsInstance();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hgs
```

### 4.7 `hgs_instance.cpp` 实现要点

| 项 | 实现 |
|---|---|
| `Instance()` | 函数内 `static HgsInstance`（无加载期构造） |
| `Start` | 1) `SplitSocketPath` 校验；2) 无条件安装 state/toplevel 适配 lambda（lambda 内读 `host_`，允许 SetHost 在 Start 前后）；3) `w/h > 0` 才 `SetOutputSize`；4) `composition != Unspecified` 才 `SetDesktopMode`；5) `WaylandServer::Start(path)` |
| 事件适配 | 内部 sink 给 `(id, name, json)`，用 `ToplevelEventTypeFromName` 转 public enum；`Unknown` 事件丢弃并打 WARN（现有 21 种都能命中） |
| `Stop` | 先 `running_ = false`；`WaylandServer::Stop()`；清 generation 表 |
| `SetInputBackend` | 阶段 0 只写 `Impl::inputBackend_`，不改变任何回调注册 |
| `ApiVersion` | 返回 `HGS_API_VERSION`（阶段 0 = 1） |
| `LastError` | 每次失败路径写 `Impl::lastError_`（互斥量保护） |
| `AttachSurface` | `PluginManager::CreateRenderer(id, (int64_t)token)`；成功后 `++gen` 写表并返回 `{id, token, gen}`；失败返回 `{0,0,0}` |
| `ResizeSurface` | 查表；`gen != 0 && gen != 表内 gen` → WARN + return false；否则 `PluginManager::ResizeRenderer` |
| `DetachSurface` | 同上校验；`PluginManager::DestroyToplevel`；擦表 |
| `Send*` | 逐字段转 `InputManager::Send*`（8/3/6 参数） |
| `SetToplevelVisible` | `InputManager::SetToplevelVisible`，`visible==true` 时再 `WaylandServer::SetToplevelRestored` |
| `DestroyToplevel` | `PluginManager::DestroyToplevel` + `PointerExtras::ReleaseLockForToplevel` |
| `FindToplevelAt` | `WaylandServer::FindToplevelAt` |
| pending | `PluginManager::SetPendingToplevel` / `DequeuePendingToplevel` / `CancelPendingToplevel` |

线程与加锁：

- `Impl::mutex` 只保护 `host_`、`running_`、generation 表；**不跨**转发调用持锁。
- `Start/Stop` 不做重入保护之外的同步；`Stop` 内部已 join。
- 回调适配 lambda 捕获 `Impl*`，`Stop` 前必须保证 bus 不再派发（`WaylandServer::Stop` 先 `DestroyAllToplevels` 再回收线程）。

## 5. 逐项映射表

| 新 API | 现有实现 | 线程 | 备注 |
|---|---|---|---|
| `Start` | `WaylandServer::Start`（`wayland_server.cpp:43`） | 主 | 先装回调再 Start |
| `Stop` | `WaylandServer::Stop`（`:106`） | 主 | 幂等 |
| `IsRunning` | 新增访问器读 `running_` | 任意 | 只读 |
| `SetOutputSize` | `wayland_server.h:132` | 主 | 0 不下发 |
| `SetDesktopMode` | `wayland_server.h:151` | 主 | Unspecified 不下发 |
| `SetInputBackend` | 阶段 0 仅记录 `Impl` 字段 | 任意 | 阶段 1 生效（§14） |
| `DesktopRootId` | `GetDesktopRootToplevelId`（`wayland_server.h:154`） | 主 | |
| `AttachSurface` | `PluginManager::CreateRenderer`（`plugin_manager.h:35`） | UI | 改 bool 返回 |
| `ResizeSurface` | `PluginManager::ResizeRenderer`（`:36`） | UI | 内部已 ForceRedraw + `surface` 事件 |
| `DetachSurface` | `PluginManager::DestroyToplevel`（`:39`） | UI | 只 surface，不碰 toplevel |
| `SendPointerEvent` | `InputManager::SendPointerEvent`（`input_manager.h:70`） | JS | 8 参数逐一对应 |
| `SendKeyEvent` | `InputManager::SendKeyEvent`（`:75`） | JS | |
| `SendScrollEvent` | `InputManager::SendScrollEvent`（`:81`） | JS | |
| `NotifyToplevelResize` | `wayland_server.h:127` | 主 | |
| `SendToplevelClose` | `wayland_server.h:103` | 主 | |
| `RaiseToplevel` | `wayland_server.h:145`（`userInitiated=true`） | 主 | 保持现 NAPI 语义 |
| `SetToplevelVisible` | `InputManager::SetToplevelVisible`（`:111`）+ `SetToplevelRestored` | 主 | 组合语义 |
| `DestroyToplevel` | `PluginManager::DestroyToplevel` + `PointerExtras::ReleaseLockForToplevel` | 主 | 现 `DestroyToplevelNAPI` 语义 |
| `FindToplevelAt` | `wayland_server.h:140` | 主 | |
| pending 三件套 | `PluginManager` `Set/Dequeue/Cancel` | UI | 阶段 2 迁出 HGS |

## 6. 事件通道

阶段 0 不改 `ToplevelEventBus::EventSink` 签名（`:106`），
`HgsInstance` 在安装回调时做一次字符串 → enum 转换：

```text
ToplevelEventBus::Post(id, enum, json)
  -> sink (id, name, json)
  -> WaylandServer::ToplevelCb
  -> HgsInstance lambda: ToplevelEventTypeFromName(name)
  -> IHgsHost::OnToplevelEvent({id, type, json})
```

阶段 1 再把 sink 改成类型化签名 `(uint32_t, ToplevelEventType, const char*)`，
删掉这层字符串转换；`napi_init.cpp` 的 `SetToplevelCallback` 适配器负责
`ToplevelEventName(type)` 回字符串，JS 侧零改动。

**不做 JSON 解析**：`ToplevelEvent::json` 原样透传。原因是现有 payload 是固定
`snprintf` 模板（`toplevel_event_bus.h:132+`），自己写 JSON parser 会引入新的
转义/边界问题（标题转义现在是已知的历史行为，见该头注释）。
类型化 payload（`ToplevelInfo` 等）留到阶段 2，由总线在构造点直接填结构体，
而不是从字符串反解。

## 7. NAPI 兼容策略

导出面的完整清单（C++/NAPI/ArkTS HAR、新旧命名、弃用表）见
[hgs-exported-api.md](hgs-exported-api.md)。本节只保留阶段 0/1 的策略要点。

### 阶段 0

- `napi_init.cpp` 的 `desc[]` 一行不改。
- `types/libhgs/Index.d.ts` 一行不改。
- 旧路径 `createRenderer` → `PluginManager` 仍然直连，不经过 `HgsInstance`。
- 新 API 只保证编译进 `libhgs.so` 并可被后续 C++ 宿主使用；无运行时行为变化。

### 阶段 1（下一次改动，不在本轮）

逐个把旧 NAPI 函数体改成「解析参数 → 调 `HgsInstance`」：

| NAPI | 改为 |
|---|---|
| `startServer` | `Instance().Start(cfg)` |
| `stopServer` | `Instance().Stop()` |
| `setOutputSize` | `Instance().SetOutputSize` |
| `setDisplayScale` | 保留 no-op（或写 `OutputConfig::scale`） |
| `setDesktopMode` | `Instance().SetDesktopMode` |
| `getDesktopRootId` | `Instance().DesktopRootId()` |
| `setStateCallback` / `setToplevelCallback` | 建成一个 `IHgsHost` 实现，注册到 `Instance().SetHost`；TSFN 逻辑留在 NAPI 层 |
| `createRenderer` | `Instance().AttachSurface`；返回的 generation 留在 NAPI 层静态表（`.d.ts` 返回值仍是 `void`），旧 `resizeRenderer/destroyRenderer` 传 0 通配 |
| `resizeRenderer` | `Instance().ResizeSurface(..., gen=0)` 通配 |
| `destroyRenderer` | `Instance().DetachSurface(..., gen=0)` 通配 |
| `sendPointerEvent` 等 | `Instance().Send*` |
| `notifyToplevelResize*` | `Instance().NotifyToplevelResize` |
| `destroyToplevel` / `sendToplevelClose` / `raiseToplevel` / `setToplevelVisible` / `findToplevelAt` | 对应门面方法 |
| `setPendingToplevel` / `getCurrentToplevelId` / `cancelPendingToplevel` | 对应兼容方法 |

签名与 `.d.ts` 全部不变；ArkTS 无需改动。

补充约束：阶段 1 起 NAPI 层**不再直接调用** `WaylandServer::SetToplevelCallback` /
`SetStateCallback`。这两个通道的 sink 由 `HgsInstance::Start` 唯一安装，
NAPI 只实现 `IHgsHost` 并通过 `SetHost` 注册；否则后注册者会覆盖前者，
事件会静默丢失。

## 8. 构建改动

`hgs/src/main/cpp/CMakeLists.txt`：

```cmake
include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/api          # 新增
    ${NATIVERENDER_ROOT_PATH}/protocols
    ...
)

add_library(hgs SHARED
    napi_init.cpp
    smoke/egl_renderer.cpp                 # 阶段 1 再移出
    api/hgs_endpoint.cpp                   # 新增
    api/hgs_instance.cpp                   # 新增
    ...
)
```

- 不新增 `target_link_libraries`。
- `api/*.h` 没有 OHOS 依赖，不会被 entry 的 include 顺序影响。
- `api/hgs_instance.cpp` 会间接引入 `hilog` / `napi`（经 `WaylandServer`），
  这是阶段 0 允许的：**只有实现文件允许依赖平台**，头文件不允许。

## 9. 行为不变性论证

| 风险点 | 阶段 0 为什么不改变行为 |
|---|---|
| Start 覆盖输出尺寸/模式 | `width/height=0`、`composition=Unspecified` 不下发；旧 NAPI 不经过门面 |
| 环境变量 | `WaylandServer::Start` 内部 `setenv` 保持原样（`wayland_server.cpp:62,78`） |
| 事件 | 枚举迁移只改定义位置，`ToplevelEventName` 文本逐字不变；宿主测试对账 |
| renderer | `CreateRenderer` 改返回值，旧调用点忽略返回值；失败路径不变 |
| 输入 | 门面只做字段转发，未调用 |
| pending 队列 | 门面只暴露，未接管现有 ArkTS 调用 |
| NAPI/ABI | 导出表、`.d.ts`、函数签名全部不变 |

唯一可感知的变化是 `libhgs.so` 里多了几个未被引用的符号；对 HAP 行为无影响。

## 10. 验证方案

### 10.1 先修 `make test`（P0.0，可独立提交）

现状：`Makefile:441+` 的所有 compositor 测试仍指向 `entry/src/main/cpp/compositor/...`，
这些文件已不存在（实测 `g++` 报 `fatal error: compositor/toplevel/toplevel_event_bus.h: No such file`）。

修法：在 `Makefile` 的 test 段引入两个变量：

```make
HGS_CPP   := $(ROOT)/hgs/src/main/cpp
ENTRY_CPP := $(ROOT)/entry/src/main/cpp
HOST_TEST_INC := -I $(HGS_CPP) -I $(ENTRY_CPP) -I $(ENTRY_CPP)/wine
```

- 所有规则把 `-I $(ROOT)/entry/src/main/cpp -I .../wine` 换成 `$(HOST_TEST_INC)`。
- `geometry_test` / `blit_scaled_test` / `blit_clip_test` / `shm_frame_source_test` /
  `input_state_test` / `controller_merge_test` 的源文件路径改为 `$(HGS_CPP)/...`。
- `env_spec_test` / `env_baseline_test` 的 `wine` 源文件仍走 `$(ENTRY_CPP)`。

新增两条：

```make
	g++ -std=c++17 -Wall -Wextra $(HOST_TEST_INC) \
	    -o $(HOST_TEST_DIR)/hgs_endpoint_test \
	    $(ROOT)/host_tests/hgs_endpoint_test.cpp \
	    $(HGS_CPP)/api/hgs_endpoint.cpp
	$(HOST_TEST_DIR)/hgs_endpoint_test
	g++ -std=c++17 -Wall -Wextra $(HOST_TEST_INC) \
	    -o $(HOST_TEST_DIR)/hgs_event_map_test \
	    $(ROOT)/host_tests/hgs_event_map_test.cpp
	$(HOST_TEST_DIR)/hgs_event_map_test
```

### 10.2 用例

`hgs_endpoint_test.cpp`：

| 输入 | 期望 |
|---|---|
| `/data/storage/el2/base/.wine/wayland-0` | `dir=/data/.../.wine`，`name=wayland-0` |
| `wayland-0`（相对路径） | invalid |
| `/data/`（尾斜杠） | invalid |
| 空串 | invalid |
| `/wayland-0` | `dir=/`，`name=wayland-0` |
| `BuildClientEnvironment` 有效路径 | 两条 `KEY=VALUE`，顺序固定 |

`hgs_event_map_test.cpp`：

- 遍历 `kAllToplevelEvents`，`ToplevelEventName` 返回非空且不等于 `"unknown"`。
- 每个名字反查 `ToplevelEventTypeFromName` 回到原枚举。
- 名字集合无重复。
- 固化红线字符串：`created/destroyed/title/resize/surface/...` 与旧值逐字。

注意：现有 `host_tests/toplevel_event_test.cpp:96-108` 用 `for (i = 0; i < 21; i++)`
数值遍历，并把 `"unknown"` 判为失败。迁入 `Unknown = 0` 后必须改为遍历
`kAllToplevelEvents`，否则 0 会命中 `Unknown` 而误报失败，同时漏掉 `DesktopRoot`。

### 10.3 Docker 增量出 HAP

```bash
docker run --rm --user 1000:1000 -e HOME=/tmp \
  -v /workspace/WineHua-root/WineHua:/data/src/winehua \
  -v /workspace/WineHua-root/WineHua/harmony-sdk:/apps/harmony \
  -v "/media/yiyan/5E1E09181E08EABB/Users/suwan/.ohos/config:/mnt/user-signature:ro" \
  winehua-build:latest bash -lc \
  'cd /data/src/winehua && source scripts/env.sh && make NATIVE_ARCH=arm64-v8a hap'
```

通过标准：

1. hvigor 输出 `hgs:default@...` 编译成功，`libhgs.so` 重新链接（新增两个 `.o`）。
2. 产出 `entry/build/default/outputs/default/entry-default-signed.hap`。
3. 构建日志无新增 warning（尤其 `api/hgs_instance.cpp` 的 `-Wall -Wextra`）。

若只改头文件/纯函数，可先跑容器内 `make test` 缩短反馈：

```bash
docker run --rm --user 1000:1000 -e HOME=/tmp \
  -v /workspace/WineHua-root/WineHua:/data/src/winehua \
  winehua-build:latest bash -lc 'cd /data/src/winehua && make test'
```

### 10.4 不在本轮验证

- 真机窗口/输入/多窗口回归（阶段 1 重定向 NAPI 后必须做）。
- HarmonyBox / BetterSH 构建（本机无 HarmonyBox 源码）。
- zero-copy / VirGL / Venus。

## 11. 实施顺序与回滚

| 步骤 | 内容 | 状态 |
|---|---|---|
| P0.0 | 修 `Makefile` host test 路径，`make test` 恢复绿 | ✅ `aaebd8453` |
| P0.1 | 事件枚举迁入 `api/hgs_events.h`，bus 加 `using`；更新 `toplevel_event_test.cpp` 的数值遍历；新增事件映射测试 | ✅ hgs `8fe3a04` |
| P0.2 | 新增 `hgs_types.h` / `hgs_endpoint.h` / `hgs_host.h` / `hgs.h` + endpoint 测试 | ✅ hgs `1857cb7` |
| P0.3 | 新增 `hgs_instance.h/.cpp`；`IsRunning` / `CreateRenderer` 改 bool；CMake 接入 | ✅ hgs `b2f68a0` |
| P0.4 | Docker 原生编译验证（签名 HAP 待签名材料挂载） | ✅ HAR 构建通过 |
| P1.1 | NAPI canonical 导出 + 回调经 `IHgsHost` 收口 + 旧名弃用转发 + `.d.ts` 补齐 | ✅ hgs `0ff11ad` |
| P1.2 | `ToplevelEventBus::EventSink` 类型化，删字符串反查 | ✅ hgs `2ca4d1b` |
| P1.3 | `setProcessEnvironment=false` 生效（不 setenv，宿主用 `ClientEnvironment`） | ✅ hgs `f433cdd` |
| N1 | 原生输入：mouse/key/axis/focus + touch 单击；`bindXComponent(id)`；原生 renderer | ✅ hgs `0cc9e22` |
| W1 | 窗口管理器 core：`hgs_window_*` + `FloatingWindowPolicy` + host 接口 + 单测 | ✅ hgs `4116552` |
| W2 | 窗口管理器事件接线 + NAPI 通道 + 默认 ArkTS host（Service/Ability/Page） | ✅ hgs `9ac8214`/`8f43031` |
| N2 | 手势状态机移植（长按/双击/双指/触控板），Native 与 ArkTS 行为对齐 | ⏳ |
| P1.4 | `HgsInstance` 持有状态 / seam provider / smoke 移出产品库 / HgsSurface Wine 手势拆分 | ⏳ |

提交归属：

- P0.0 / P0.2 的 `host_tests` 与 `Makefile` 在外层仓库。
- P0.1 / P0.3 的 C++ 在 hgs submodule；外层仓库只更新 submodule 指针。
- 本文档在外层仓库 `docs/architecture/`。

回滚：

- 代码回滚：`git revert` 对应提交（hgs submodule 内单独 revert，再 bump 指针）。
- 无 NAPI/ABI 变化，不需要回滚 `.d.ts` 或 ArkTS。
- 若只回滚门面：删除 `api/` 目录 + CMake 两行即可，其余代码不引用它。

## 12. 后续阶段（供排期，不在本轮）

### 阶段 1：WineHua 内部收口 + 原生输入 N1

1. 旧 NAPI 全部改为门面转发（§7 表）。
2. `ToplevelEventBus::EventSink` 改类型化签名；删除字符串反查。
3. `HgsInstance` 从「门面」改为「持有状态」：`WaylandServer` 的单例成员下沉为 `Impl` 成员。
4. `seam/*` 改为 `IExternalSurfaceProvider` 可选注册；基础 `libhgs.so` 不再编 `seam`。
5. `smoke/egl_renderer.cpp` 移出产品库（单独 `smoke_napi` 或测试目标）。
6. `HgsSurface.ets` 的 Wine 手势/回车补发/触控板逻辑拆到 `WineInputSurface.ets`。
7. `HgsInstance::Start` 真正支持 `setProcessEnvironment=false`，WineHua 改用 `ClientEnvironment()`。
8. 原生输入 N1（§14.7）：mouse/key/axis/focus 直通 + touch 单击；`setInputBackend` NAPI。
9. 验收：单窗口/多窗口/弹窗/resize/关闭/输入/剪贴板；Native/ArkTS 两种输入后端可切。

### 阶段 2：HGS 窗口管理器 W1/W2 + 原生输入 N2

1. `hgs_window_*` 纯 C++ core：`WindowRegistry` / `IWindowPolicy` / `HgsWindowManager` / `IWindowHost`。
2. 默认 `FloatingWindowPolicy`；`HgsInstance::WindowManager()`。
3. HGS HAR 默认 ArkTS host：`HgsWindowHostService` / `HgsAppWindowAbility` / `HgsWindowPage`。
4. 原生输入 N2（§14.7）：手势状态机移植，Native 模式与 ArkTS 对齐。
5. 验收：WineHua 单窗口/多窗口用默认 host 可跑；策略可替换。

### 阶段 3：BetterSH（W3）

- `/workspace/BetterSH` 存在。
- 动作：AbilityStage 启动 HGS；删 `feature/bsh_main/src/main/cpp/shell/shell.cpp:638` 旧 server；
  删除 `WaymonyService` / `ClientWindow` 的窗口管理部分，改用 `HgsWindowHostService` +
  `HgsWindowPage`；`product/{phone,tablet}/src/main/ets/pages/ClientWindow.ets:34`
  的 `Number(surfaceId)` 路径随旧页面一起删除；`dataDevice=true`；保留 `BasicShell/procmgr`。
- 验收：wldemo/GTK/Qt/多窗口/resize/关闭/clipboard；ArkTS 无输入事件。

### 阶段 4：HarmonyBox（W4）

- 本机没有 HarmonyBox 源码（`/media/.../HarmonyBox` 不存在），需先挂载或拉取。
- 动作：依赖 HGS HAR、移除 `entry` 内 `wayland_server/xdg_shell/egl_renderer/plugin_manager`、
  `HBoxAbilityStage.onCreate` 初始化 HGS、`WaylandService` 改 host adapter、
  `WindowManagerService` 改 `IWindowHost` 或直接用默认 host、`AppRunner` 用 `HgsSurface`。

### 阶段 5：高级能力

`DesktopRoot`、`NativeVSync`、`NativeImage`、zero-copy、VirGL/Venus、pointer lock、controller、Wine 协议。

## 13. 需要确认的 5 个决策

1. **事件 payload 是否阶段 0 就类型化？**
   建议：不。阶段 0 只做 enum + 原样 JSON（零解析风险）；阶段 2 由总线构造点直接填结构体。
2. **`HgsInstance` 过渡期用 `Instance()` 还是直接 `Create()`？**
   建议：`Instance()`。阶段 0 没有所有权迁移，`Create()` 会制造「多实例」假象；
   阶段 1 收口状态时再换签名。
3. **`DetachSurface` 是否释放 pointer lock？**
   建议：不。与现 `destroyRenderer` 对齐；pointer lock 归 `DestroyToplevel`（现 `destroyToplevel` 语义）。
   这样 surface 重建不会误释放游戏锁定。
4. **原生输入先给谁用？**
   建议：BetterSH/HarmonyBox 先默认 `Native`（只要鼠标/键盘/滚轮），WineHua 阶段 1
   仍默认 `ArkTS`，设置页可切 `Native` 做 A/B；手势/IME 对齐后再把 WineHua 默认切 Native。
   不建议一步到位删掉 ArkTS 路径。
5. **窗口管理器的默认 host 放哪？**
   建议：core 放 C++（零 OHOS 依赖），默认 ArkTS host 放 HGS HAR
   （`HgsWindowHostService`/`HgsAppWindowAbility`/`HgsWindowPage`）。
   WineHua 保留自己的 `WineWindowManager` 作为 `IWindowHost`/`IWindowPolicy`，
   不强行切到默认 host；BetterSH/HarmonyBox 直接用默认 host。

## 14. 追加需求 A：原生输入（ArkTS 零事件注册）

### 14.1 目标形态

Blender 的 `Index.ets` 已经把输入完全交给 C++：

```ts
XComponent({
  id: 'blender_canvas',
  type: XComponentType.SURFACE,
  libraryname: 'entry',
})
  .width('100%')
  .height('100%')
  .focusable(true).focusOnTouch(true)
```

`Index.ets` 里没有 `onTouch` / `onMouse` / `onAxisEvent` / `onKeyEvent`；
surface 与输入回调都在 `libraryname:'entry'` 触发的 NAPI Init 里注册。
（子窗口的 XComponent 通过 `bindSubXComponent` 补挂。）

HGS 目标相同：

```ts
// HgsSurface 的 native 分支（目标形态）
XComponent({
  id: this.xcId,
  type: XComponentType.SURFACE,
  libraryname: 'hgs',
  controller: this.controller,   // 与 Blender 一致；只承载 surface，不挂事件
})
  .width('100%').height('100%')
  .focusable(true).focusOnTouch(true)
  .onLoad((event?: object) => {
    if (event) {
      hgs.bindXComponent(event, this.resolveId?.() ?? this.toplevelId);
    }
  })
```

`onLoad` 只是生命周期补挂，不是输入事件；`focusable/focusOnTouch` 是布局属性。
这里说的“零事件”指**输入事件**（touch/mouse/axis/key），生命周期与布局属性
仍允许保留 —— Blender 的 Index.ets 本身也保留了 `onDestroy`。
阶段 1 N1 已在 `HgsSurface.ets` 的 native 分支落地
`onLoad((event) => hgs.bindXComponent(event, id))` 的补挂模式
（targetSdk 23，`onLoad` 自 API 18 可用）。

### 14.2 SDK 能力核对（项目自带 SDK 已确认）

| 事件 | 注册 API | 取事件 | 备注 |
|---|---|---|---|
| Surface 生命周期 | `OH_NativeXComponent_RegisterCallback`（`OnSurfaceCreated/Changed/Destroyed`） | — | 现有 `gXcompCb` 已注册 |
| 触摸 | 同上 `OH_NativeXComponent_Callback.DispatchTouchEvent` | `OH_NativeXComponent_GetTouchEvent` | 多指、`sourceTool` |
| 鼠标 | `OH_NativeXComponent_RegisterMouseEventCallback` | `OH_NativeXComponent_GetMouseEvent` | 含 screenX/Y、button/action |
| 键盘 | `OH_NativeXComponent_RegisterKeyEventCallback` | `OH_NativeXComponent_GetKeyEvent` + `GetKeyEventAction/Code` | 需 XComponent 获焦 |
| 滚轮/轴 | `OH_NativeXComponent_RegisterUIInputEventCallback`（`ARKUI_UIINPUTEVENT_TYPE_AXIS`） | `OH_ArkUI_AxisEvent_GetVerticalAxisValue/HorizontalAxisValue/GetScrollStep/GetAxisAction` + `OH_ArkUI_PointerEvent_GetX/Y` | API 12+，仅轴事件 |
| 焦点/失焦 | `OH_NativeXComponent_RegisterFocusEventCallback` / `RegisterBlurEventCallback` | — | 焦点状态（光标/窗口管理器用） |
| 软键盘 | `OH_NativeXComponent_SetNeedSoftKeyboard(true)` | — | IME 场景 |

结论：鼠标、键盘、滚轮、触摸都能在 C++ 侧拿到，**不需要 ArkTS 事件转发**。
缺口只有“手势语义”（长按转右键、双击、双指滚动、触控板虚拟光标），
这些是 HGS 自己的策略层，可移植到 C++。

### 14.3 新增模块

```text
hgs/src/main/cpp/input/native_input.h/.cpp
    NativeInputBinding: OH_NativeXComponent* <-> ToplevelId
    RegisterNativeInput(comp, toplevelId) / UnregisterNativeInput(comp)
    touch/mouse/key/axis/focus/blur 六个回调 -> HgsInstance::Send*
hgs/src/main/cpp/input/native_keymap.h/.cpp
    OHOS KeyCode -> evdev（从 hgs/src/main/ets/common/KeyMap.ets 机械移植）
hgs/src/main/cpp/input/native_mousemap.h
    OHOS MouseEvent button -> evdev（从 common/MouseMap.ets 移植）
hgs/src/main/cpp/input/native_input_policy.h/.cpp
    手势状态机（长按/双击/双指/触控板），纯 C++，可 host test
host_tests/native_keymap_test.cpp
host_tests/native_input_policy_test.cpp
```

绑定表以 `OH_NativeXComponent*` 为键（与现有 `gRenderers` 同模式），
value 为 `{toplevelId, generation}`；surface destroyed 时解绑，
generation 不匹配的延迟回调直接丢弃。

### 14.4 事件映射

| 原生事件 | 映射 | 注意 |
|---|---|---|
| Mouse `action=PRESS/RELEASE/MOVE` | `PointerEvent{action,x,y,button,fromMouse=true}` | `x/y` 单位需实测；`screenX/Y` 用于 DesktopRoot |
| Mouse hover | 只更新光标位置，不产生按键 | hover 不进输入队列 |
| Key `code` + `action=DOWN/UP` | `KeyEvent{evdevCode,pressed}` | 用 `KeyMap` 映射表；修饰键状态可用 `GetKeyEventModifierKeyStates` |
| Axis vertical/horizontal | `ScrollEvent{axis,value,scrollStep,x,y}` | 与 ArkTS `onWineAxisEvent` 的 `SCROLL_STEP_PX` 语义对齐 |
| Touch DOWN/MOVE/UP/CANCEL | N1：单击=左键；N2：长按=右键、双指=滚动 | 手势由 `native_input_policy` 决定，不在回调里硬编码 |
| Focus/Blur | 记录当前获焦窗口（供窗口管理器/光标策略使用） | 键盘事件由系统只发给获焦 XComponent，不需要额外设 target |
| Touch `sourceTool` | finger/pen/touchpad | 触控板虚拟光标策略的输入 |

坐标单位是阶段 1 的第一个实测项：`OH_NativeXComponent_MouseEvent.x/y` 的注释是
“relative to the left edge of the element”，未写明 vp/px；`TouchEvent` 同理。
落地时先打一条日志对 `OH_NativeXComponent_GetXComponentSize` 做比值判定，
再决定是否需要 `vp2px`。不要照抄 ArkTS 的 `vp2px` 假设。

### 14.5 绑定与生命周期

1. 首个 XComponent：`libraryname:'hgs'` 触发 `napi_init.cpp` 的 Init，
   `OH_NATIVE_XCOMPONENT_OBJ` 存在 → 注册 surface + 输入回调。
2. 同进程后续 XComponent：模块已加载，Init 不会再次执行 →
   `onLoad` 调 `bindXComponent(xcomp, toplevelId)` 补挂。
3. `bindXComponent` 扩展为 `(xcomp: object, toplevelId?: number)`；
   旧的一参调用保持可用（toplevelId=0 时只注册 surface 回调，输入丢弃并打 WARN）。
   Native 模式要求页面在 `onLoad` 里带 toplevelId 调用一次。
4. `OnSurfaceDestroyed`：注销回调、清绑定、按 generation 忽略迟到事件。
5. 回调线程 = UI 线程；只做字段转换 + `InputManager::Send*` 入队，
   不直接调 `wl_*_send_*`（沿用现有线程模型，`input_manager.h:15-32`）。
6. Native 模式不依赖 ArkTS controller 的 surfaceId：`OnSurfaceCreated(comp, window)`
   已经拿到 `OHNativeWindow*`，HGS 内部走
   `AttachSurfaceWindow(toplevelId, OHNativeWindow*)`（`PluginManager` 增加
   `CreateRendererFromWindow` 重载），不经过 `AttachSurface(token)`。
   项目 SDK 的 `native_interface_xcomponent.h` 没有
   `OH_NativeXComponent_GetXComponentSurfaceId`，不要假设 native 侧能取到 surfaceId。
   `AttachSurface(token)` 继续服务 ArkTS controller 路径。

### 14.6 用户选择：ArkTS 还是 C++

```cpp
enum class InputBackend : uint8_t { Auto = 0, ArkTS = 1, Native = 2 };
```

- `HgsConfig.inputBackend`：全局默认（阶段 0 记录，阶段 1 生效）。
- `HgsInstance::SetInputBackend(InputBackend)`：运行时切换（阶段 1）。
- NAPI `setInputBackend(mode)`：ArkTS 设置页写偏好（阶段 1，`.d.ts` 追加）。
- `HgsSurface` 增加 `@Prop inputBackend: number`（0/1/2），页面从偏好读取后传入。
- `Auto` 策略：
  - WineHua：阶段 1 仍默认 `ArkTS`（手势/IME 回归面大），设置页可切 `Native` 做 A/B。
  - BetterSH / HarmonyBox：默认 `Native`（只需要鼠标/键盘/滚轮，手势少）。
  - 阶段 3 后 WineHua 默认切 `Native`，`ArkTS` 保留为兼容回退。
- 两种后端必须能在同一个 HAP 内切换，不做构建变体。

`HgsSurface` 的 native 分支条件从
`useNativeSurfaceCallback && useNativeInputCallback` 改为
`inputBackend === 2`；`inputBackend === 1` 走现有 ArkTS 事件路径。
旧的 `useNative*` 属性保留一个版本作为别名，之后删除。

### 14.7 分阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| N1（阶段 1） | mouse/key/axis/focus 原生直通；touch 单击转左键；滚轮；原生 renderer (`CreateRendererFromWindow`) | ✅ hgs `0cc9e22`（真机坐标单位/键位待回归） |
| N2（阶段 2） | 手势状态机移植（长按/双击/双指/触控板）；Wine 专用策略拆 `WineInputPolicy` | ⏳ |
| N3（阶段 3） | WineHua 默认切 Native；ArkTS 路径降为兼容开关 | ⏳ |

### 14.8 风险

1. **坐标单位**（vp vs px）：必须实测，不能假设。
2. **键盘焦点**：XComponent 未获焦时收不到 key；`focusable/focusOnTouch` 必须保留，
   窗口切换时由 host 调 `requestFocus`（OHOS window API）。
3. **IME/软键盘**：原生 key 事件 + `SetNeedSoftKeyboard` 需要真机回归中文输入；
   现有 `text_input` 协议/`setImeCallback` 的 ArkTS 侧输入法弹窗逻辑不在本阶段删除。
4. **触摸手势**：`HgsSurface.ets` 现有状态机较大（长按、双击、双指、虚拟光标），
   移植要逐条对账；N1 先不做手势，避免一次性引入回归。
5. **多窗口**：每个 XComponent 一份绑定；surface 重建用 generation 防串。
6. **系统返回键/组合键**：原生 key 回调可能拿不到系统级按键（返回、音量），
   这类仍由 ArkUI/系统处理，不在“零事件”范围内。

## 15. 追加需求 B：HGS 内置窗口管理器

### 15.1 目标

BetterSH 删除全部 Wayland 相关代码后，只依赖 HGS 就能获得窗口服务：

- 删除 native：`wayland_server`、`xdg_shell`、`egl_renderer`、`plugin_manager`、
  `input_window_napi`、`data_device`。
- 删除 ArkTS：`WaymonyService`、`ClientWindow` 中的窗口管理/输入转发。
- 保留：`BasicShell`、`procmgr`、终端 UI、资源路径、卡片/菜单等业务。
- 目标代码量：EntryAbility 约 10 行初始化 + 一个 Ability/page 声明 + 一个 5 行子类。

窗口服务范围：

```text
创建 / 销毁 / 显示 / 隐藏
标题、appId、图标（可选）
移动 / 缩放 / 最大化 / 最小化 / 全屏 / 还原
点击聚焦 / 置顶 / z-order
模态 / 父子窗口 / popup
关闭请求（xdg close）
输入焦点与键盘目标
```

### 15.2 分层与所有权

```text
hgs-window (C++ core, 不依赖 OHOS)
  WindowRegistry        窗口 spec（pid/appId/title/parent/modal/装饰/策略标记）
                        几何/状态/z-order 不复制，读 ToplevelManager
  IWindowPolicy         可替换策略（放置/聚焦/叠放/状态/关闭）
  HgsWindowManager      订阅 toplevel 事件 -> 更新 spec/策略 -> 发 WindowEvent
  IWindowHost           平台操作接口（创建/销毁/几何/状态/移动/缩放）

hgs-window-ohos (HGS HAR 内的默认 ArkTS host, 可选)
  HgsWindowHostService  消费 WindowEvent -> startAbility / window API
  HgsAppWindowAbility   通用窗口 Ability（multiton）
  HgsWindowPage         通用页面：XComponent + HgsSurface（Native 输入）
```

边界：

- **core 不创建 Ability、不调 OHOS window API、不解析项目路径**。
- 默认 host 是 ArkTS（项目 SDK 的 `window_manager/oh_window.h` 只有
  `ShowWindow`/`SetWindowFocusable` 等“操作已有窗口”的接口，没有 `CreateWindow`；
  “ArkTS 零事件”只针对**输入**，窗口创建仍由 host 完成）。
- WineHua 可以继续用 `WineWindowManager` 实现 `IWindowHost` / `IWindowPolicy`，
  保留任务栏、桌面 root、modal、explorer 特判。
- HarmonyBox / BetterSH 直接用默认 host。

### 15.3 Core API 草案（阶段 2 新增头）

```cpp
// hgs_window_types.h
namespace hgs {

using WindowId = ToplevelId;

enum class WindowState : uint8_t { Normal = 0, Minimized = 1, Maximized = 2, Fullscreen = 3 };

enum class WindowEventType : uint32_t {
    Unknown = 0,
    Created = 1, Destroyed = 2, TitleChanged = 3,
    GeometryChanged = 4, StateChanged = 5, FocusChanged = 6,
    VisibleChanged = 7, CloseRequested = 8,
    MoveRequested = 9, ResizeRequested = 10,
    PopupShown = 11, PopupMoved = 12, PopupResized = 13, PopupHidden = 14,
};

struct WindowGeometry { int32_t x = 0, y = 0, width = 0, height = 0; };

struct WindowSpec {
    WindowId id = 0;
    uint32_t clientPid = 0;
    std::string appId;
    std::string title;
    WindowGeometry geometry;   // 初始/期望几何；运行期权威值读 ToplevelState
    int32_t minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    WindowId parentId = 0;
    bool modal = false;
    bool decorated = true;
};

struct WindowEvent {
    WindowEventType type = WindowEventType::Unknown;
    WindowId id = 0;
    WindowSpec spec;
    WindowGeometry geometry;
    WindowState state = WindowState::Normal;
    bool focused = false;
    uint32_t edges = 0;          // ResizeRequested
    std::string json;            // 兼容旧 payload（过渡期；可为空）
};

} // namespace hgs
```

```cpp
// hgs_window_policy.h
class IWindowPolicy {
public:
    virtual ~IWindowPolicy() = default;
    virtual void OnCreated(WindowSpec& spec) = 0;              // 可改初始几何
    virtual void OnRaise(WindowId id) = 0;
    virtual void OnFocus(WindowId id) = 0;
    virtual void OnStateRequest(WindowId id, WindowState state) = 0;
    virtual bool OnCloseRequest(WindowId id) = 0;               // false = 拒绝
    virtual void OnGeometryRequest(WindowId id, const WindowGeometry& g) = 0;
};
```

```cpp
// hgs_window_host.h
class IWindowHost {
public:
    virtual ~IWindowHost() = default;

    // core -> host（UI 线程；默认 ArkTS host 经 NAPI/TSFN 转成 JS 调用）
    virtual void CreateWindow(const WindowSpec& spec) = 0;
    virtual void DestroyWindow(WindowId id) = 0;
    virtual void SetGeometry(WindowId id, const WindowGeometry& g) = 0;
    virtual void SetVisible(WindowId id, bool visible) = 0;
    virtual void SetTitle(WindowId id, const std::string& title) = 0;
    virtual void SetState(WindowId id, WindowState state) = 0;
    virtual void StartMove(WindowId id) = 0;
    virtual void StartResize(WindowId id, uint32_t edges) = 0;
    virtual void RequestClose(WindowId id) = 0;

    // host -> core 的回调不放在本接口：host 直接调
    // HgsWindowManager::NotifyHostGeometry/Focus/Close/State（见下）。
};
```

```cpp
// hgs_window_manager.h
class HgsWindowManager {
public:
    void SetPolicy(std::unique_ptr<IWindowPolicy> policy);   // 默认 FloatingWindowPolicy
    void SetHost(IWindowHost* host);

    // 来自 HGS 事件总线的 toplevel 事件（wl 线程）
    void HandleToplevelEvent(const ToplevelEvent& event);

    // 来自宿主 / host 的反馈（UI 线程）
    void NotifyHostGeometry(WindowId id, const WindowGeometry& g);
    void NotifyHostFocus(WindowId id, bool focused);
    void NotifyHostClose(WindowId id);
    void NotifyHostState(WindowId id, WindowState state);

    // 策略动作
    void Raise(WindowId id);
    void Focus(WindowId id);
    void RequestMove(WindowId id);
    void RequestResize(WindowId id, uint32_t edges);
    void RequestClose(WindowId id);

    WindowSpec GetWindow(WindowId id) const;
    std::vector<WindowId> ZOrder() const;
    WindowId Focused() const;
};
```

`HgsInstance` 增加 `HgsWindowManager& WindowManager()`；`HgsConfig.window` 增加
默认策略/默认尺寸等配置（阶段 2）。

实现上 `HgsWindowManager` 以 `ToplevelManager` 的 `ToplevelState` 为几何/状态/
z-order 权威，`ToplevelEvent` 只作为“有变化”的触发信号；不重复解析 JSON，
也不复制一份窗口状态。这样 popup/modal/全屏优先级等已有语义不会被窗口层改写。

### 15.4 默认策略 `FloatingWindowPolicy`

- 创建：使用客户端请求尺寸；无位置时按层叠偏移放置（cascade），不居中假设。
  `WindowGeometry` 始终是**客户端内容尺寸/坐标**；OHOS 窗口尺寸、标题栏、
  `windowScale` 由 host 换算，不能把宿主缩放写进 core。
- 聚焦：点击/创建即聚焦；焦点由窗口管理器记录，键盘 enter/leave 仍走现有
  `InputManager::InjectKeyboardEnter` / Wayland focus 路径，不新开一套键盘状态。
- 叠放：Raise 走现有 `WaylandServer::RaiseToplevel(id, true)`（保留全屏优先级
  取号语义）；PerToplevel 模式下 OHOS 系统窗口叠放仍是最终权威，core 只维护逻辑顺序。
- 移动/缩放：`move_start` → `IWindowHost::StartMove`；
  `resize_req(edges)` → `IWindowHost::StartResize` 或跟手会话。
- 最大化/最小化/全屏：host 调 OHOS window API，成功后
  `NotifyHostState` → `WaylandServer::NotifyToplevelResize`/configure 回客户端。
- 关闭：host 的关闭按钮 → `RequestClose` → `SendToplevelClose`。
- 模态：`modalOwnerId` 存在时恒在 owner 上方，owner 销毁级联；
  输入只路由到 modal 子树。
- Popup：父窗口相对定位，恒在父窗口上方；popup 不进入全局 z-order。

策略扩展点：

- `WineFloatingPolicy`：任务栏、桌面 root、explorer 特判、Wine modal。
- `ShellPolicy`：BetterSH 的窗口缩放（UI_SCALE）、终端窗口偏好。
- 未来 `TilingPolicy` / `MobilePolicy` 不改 core。

### 15.5 默认 ArkTS host（HGS HAR 内）

`HgsWindowHostService`：

```ts
// 伪代码
export interface HgsWindowHostOptions {
  abilityName?: string;   // 默认 'HgsAppWindowAbility'
  pageUrl?: string;       // 默认 'pages/HgsWindowPage'（宿主 main_pages.json 注册）
  windowScale?: number;   // BetterSH 的 UI_SCALE=2 可放这里；默认 1
}

export class HgsWindowHostService implements WindowEventSink {
  static init(ctx: common.UIAbilityContext, opts?: HgsWindowHostOptions): void;
  // 内部：
  //   hgs.setWindowCallback(ev => this.onWindowEvent(ev))
  //   on Created  -> startAbility({ abilityName, parameters: { id, title, w, h, appId } })
  //                  失败 -> hgs.notifyWindowClosed(id) 回滚 core 状态
  //   on Destroyed-> terminateSelf / close window
  //   on Title    -> win.setWindowTitle(title)
  //   on State    -> win.maximize()/minimize()/restore()/setFullScreen()
  //   on Move     -> win.startMoving()
  //   on Resize   -> resize session 或 win.resize()
  //   on Geometry -> win.moveTo/resize（host 侧节流）
}
```

`HgsWindowPage`：

```ts
@Component
export struct HgsWindowPage {
  private toplevelId: number = 0;

  aboutToAppear(): void {
    // 与 BetterSH AppRunnerAbility + WaymonyService 同机制：
    // Ability onCreate 用 hgs.setPendingToplevel(id)，页面同步取走。
    this.toplevelId = hgs.getCurrentToplevelId();
  }

  build() {
    HgsSurface({
      xcId: 'hgs_app_xc',
      toplevelId: this.toplevelId,
      inputBackend: 2,        // Native
      keyboardTarget: this.toplevelId,
      // 不挂 onTouch/onMouse/onKeyEvent
      // 不调 createRenderer/attachSurface：native OnSurfaceCreated 直接绑定 window
    })
  }
}
```

宿主只需：

1. `module.json5` 注册 `HgsAppWindowAbility`（5 行子类，`launchType: "multiton"`）。
2. `main_pages.json` 注册 `pages/HgsWindowPage`（5 行包装页）。
3. `EntryAbility.onCreate` 调 `HgsWindowHostService.init(this.context, { windowScale: 2 })`。
4. `EntryAbility.onCreate` 调 `HgsWindowHostService.start({ socketPath, ... })` —
   封装 `hgs.startServer`，默认 `inputBackend=Native` / `PerToplevel` /
   `dataDevice=true` / `setProcessEnvironment=false`；可传 string 只给路径，
   或传 `HgsWindowHostStartOptions` 覆盖 output/backend/features。

`HgsAppWindowAbility` 在 `onWindowStageCreate` 里按 `w/h × windowScale` 调
`mainWindow.resize`（BetterSH 现在由 `ClientWindow` 在 resize 事件里做，
初始尺寸没有显式设置），再把 `toplevelId` 放进 `setPendingToplevel` 队列。

`HgsWindowHostService` 需要一个新 NAPI 通道：`setWindowCallback(cb)`，
payload 为 `WindowEvent` 的 JS 对象（type/id/spec/geometry/state/focused/edges）。
旧的 `setToplevelCallback` 继续保留给 WineHua 的现有窗口管理，两者并行一个版本；
阶段 3 后 BetterSH/HarmonyBox 只订阅 `setWindowCallback`。

`HgsWindowHostService` 的 `startAbility` 必须从 `ctx.abilityInfo` 动态取
`bundleName`/`moduleName`（HAR 不能硬编码宿主包名，参考现有
`hgs/src/main/ets/window_launcher.ets` 的注释）。

### 15.6 BetterSH 迁移后的代码形态

```text
feature/bsh_main/
  cpp/
    shell.cpp              # 删 WaylandServer::Start（:638）
    procmgr/               # 保留
    （删除 wayland/、compositor/、egl_renderer、plugin_manager、data_device 实现）
  ets/
    service/WaymonyService.ets     # 删除（由 HgsWindowHostService 取代）
    pages/ClientWindow.ets         # 删除（由 HgsWindowPage 取代）
    service/TerminalService.ets    # 保留
product/phone|tablet/
  ets/apprunnerability/AppRunnerAbility.ets  # 改为 extends HgsAppWindowAbility（约 5 行）
  ets/pages/HgsWindowPage.ets                # 5 行包装 HGS 组件
```

EntryAbility 初始化：

```ts
HgsWindowHostService.init(this.context, { abilityName: 'AppRunnerAbility' });
HgsWindowHostService.start({
  socketPath: SOCK,
  inputBackend: 2,                 // Native
  composition: 1,                  // PerToplevel
  dataDevice: true,                // GTK seat/clipboard
});
```

客户端启动：

```ts
hgsNapi.launchClient(BOX64, [], SOCK, ..., env /* XDG_RUNTIME_DIR/WAYLAND_DISPLAY */);
```

`ClientEnvironment()` 替代 BetterSH 现在自己拼的 `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`。

### 15.7 事件映射

| 现有 toplevel 事件 | WindowEvent | 处理 |
|---|---|---|
| `created` | `Created` | 解析 w/h/title，host 创建窗口 |
| `destroyed` | `Destroyed` | host 关闭窗口 |
| `title` | `TitleChanged` | host setWindowTitle |
| `resize` / `surface` | `GeometryChanged` | 客户端内容尺寸变化；host 调整窗口或回 configure |
| `minimized` / `restored` | `StateChanged` | host minimize/restore |
| `maximized` / `unmaximized` | `StateChanged` | host maximize/restore + configure |
| `fullscreen` / `unfullscreen` | `StateChanged` | host setFullScreen + configure |
| `move_start` / `move_end` | `MoveRequested` | host startMoving |
| `resize`（带 edges） | `ResizeRequested` | host startResize/跟手 |
| `popup_show/move/resize/hide` | `Popup*` | host 子窗口操作 |
| `modal` | 更新 `WindowSpec.modal/parentId` | 策略层 |
| `desktop_root` | 不产生 WindowEvent | DesktopRoot 模式专用 |

### 15.8 WineHua 的兼容路径

- `WineWindowManager.ets` 不删除：实现 `IWindowHost` 的 NAPI 桥（或继续消费
  `setToplevelCallback`），把任务栏、桌面 root、modal、explorer 特判留在 WineHua。
- `HgsWindowManager` 的默认策略只服务 BetterSH/HarmonyBox；
  WineHua 用 `WineFloatingPolicy` 覆盖创建/聚焦/关闭。
- 迁移顺序：WineHua 先保持现有事件回调（阶段 1 NAPI 下沉后仍可并存），
  等默认 host 在 BetterSH 验证通过，再决定是否切换到统一 host。

### 15.9 data_device 与 GTK 兼容

- HGS 新增 `wl_data_device_manager` 注册（`FeatureConfig.dataDevice`）。
- 实现挂在 HGS 现有 `Seat` 上（`input/seat.cpp` 已注册 wl_seat），不要另建一个 seat；
  `data_device` 的 selection/offer 生命周期与键盘焦点绑定。
- BetterSH 现有的 `data_device.cpp` 行为（seat/clipboard）作为参考实现迁入 HGS；
  迁移后用 GTK 应用验证复制/粘贴、拖放、键盘焦点。
- 该能力是 BetterSH 的硬要求，不能因为 HGS 基础版省略。
- 同时把 Wine 专用 global 关掉：BetterSH 配置
  `features = { dataDevice: true, winehuaToplevel: false, textInput: false }`。
  当前 `WaylandServer::Start` 无条件 `RegisterWinehuaToplevel(display_)`
  （`wayland_server.cpp:84`），阶段 1 起按 `FeatureConfig` 注册。

### 15.10 窗口管理器分阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| W1（阶段 2 前半） | `hgs_window_*` 纯 C++ core + `FloatingWindowPolicy` + host 接口 + host tests | ✅ hgs `4116552`（`hgs_window_manager_test` 46 checks） |
| W2（阶段 2 后半） | HGS HAR 默认 ArkTS host：`HgsWindowHostService` + `HgsAppWindowAbility` + `HgsWindowPage`；事件接线 | ✅ hgs `9ac8214`/`8f43031`（真机待回归） |
| W3（阶段 3） | BetterSH 接入：删 WaymonyService/ClientWindow/旧 compositor；`dataDevice=true` | ⏳ |
| W4（阶段 4） | HarmonyBox 接入 | ⏳ |
| W5（后续） | 策略扩展（tiling/mobile）、多显示器、窗口动画 | ⏳ |

### 15.11 风险

1. **OHOS 窗口创建仍需要 ArkTS**：默认 host 是 ArkTS，HGS core 只做模型/策略。
   若未来 OHOS 提供稳定的 C++ 窗口创建 API，可替换 host，不影响 core。
2. **PerToplevel 模式下系统 WM 才是叠放权威**：core 的 z-order 只服务焦点/逻辑顺序；
   不要假设它能改变 OHOS 窗口层级。
3. **Ability 生命周期与 toplevel 不同步**：窗口快速关闭/重建时用 generation
   和 `destroyed` 幂等处理；host 创建失败要回滚 core 状态。
4. **窗口尺寸/缩放**：BetterSH 的 `UI_SCALE=2`、WineHua 的 effectiveScale 都是宿主策略，
   不能写死进 core；用 `WindowSpec.geometry` 与 host 的 `SetGeometry` 表达。
5. **默认 host 的 API 版本兼容**：`window.maximize/restore/startMoving/setFullScreen`
   在不同 API level 的可用性要逐项确认，缺能力时降级为 configure 回客户端。
6. **输入与窗口的绑定**：每个窗口一个 XComponent/toplevelId；native 输入用
   `bindXComponent(xcomp, toplevelId)` 关联，窗口重建用 generation 防串。

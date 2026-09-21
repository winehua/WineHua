#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

class DesktopCompositor;

// -- ZC (zero-copy) 层状态与几何供给 (重构第 3 步: 从 DesktopCompositor 抽出) --
//
// ZC 层是走 GPU 内容的 subsurface/toplevel surface (游戏/DXVK overlay)。其几何
// 信息 (布局/裁剪) 由 compositor 记录在 SubsurfaceLayer, 由本模块按需供给给
// egl_renderer (渲染视口) 与输入映射; key 权威簿记 (哪个 key 走 GPU) 由本类
// 持有 (activeKeys_, 原 DesktopCompositor::zeroCopySurfaceKeys_)。
//
// protocolOnly 布尔改显式枚举 (ZeroCopySource): 该位仅作 once-log 信息位
// (desktop_compositor.cpp protocol 分支的逐 key 去重日志), 无运行时读方,
// 改枚举是纯类型重标, 行为无变化。

enum class ZeroCopySource { ShmLayer, ProtocolOnly };

struct ZeroCopyLayerInfo {
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;
    uint32_t parentToplevel = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint64_t shmCommitSerial = 0;
    bool desktopCoordinates = false;
    ZeroCopySource source = ZeroCopySource::ShmLayer;  // 原 protocolOnly 布尔
    bool fullscreen = false;  // 所属 toplevel 全屏: GL 层保比例缩放铺满视口 (ZC 游戏)
};

struct ZeroCopyOccluderRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// -- ZC layer 判定诊断 (2026-09-16, Steam 跨进程黑窗调查) --
// 只看不改: 供 egl_renderer 在"presenter 有 surface 但从未 attach"时
// 打印真实原因 (resource/toplevel/可见性/尺寸分别在哪一步被拒)。
struct ZcLayerDiag {
    bool hasResource = false;
    bool hasData = false;
    bool hasToplevel = false;
    bool isSubsurface = false;
    bool rootCompositing = false;
    bool rootVisible = false;
    uint32_t clientPid = 0;
    uint32_t protocolId = 0;
    uint32_t toplevelId = 0;
    uint32_t parentToplevel = 0;
    uint32_t desktopRootToplevelId = 0;
    uint32_t registeredSurfaces = 0;
    int surfaceW = 0;
    int surfaceH = 0;
    int vpDstW = 0;
    int vpDstH = 0;
    int subX = 0;
    int subY = 0;
    // 同一 client pid 下的其余 surface (最多 8 个), 用于判断 present surface 的 owner 窗口
    struct Peer {
        uint32_t surfaceId = 0;
        uint32_t toplevelId = 0;
        uint32_t parentToplevel = 0;
        uint8_t hasToplevel = 0;
        uint8_t isSubsurface = 0;
        int16_t w = 0;
        int16_t h = 0;
    };
    uint32_t peerCount = 0;
    Peer peers[8] = {};
    // 全量注册表快照 (跨 client): 用于回答"present surface 的 owner 窗口到底是谁"
    struct RegEntry {
        uint32_t clientPid = 0;
        uint32_t surfaceId = 0;
        uint32_t toplevelId = 0;
        uint32_t parentToplevel = 0;
        uint8_t hasToplevel = 0;
        uint8_t isSubsurface = 0;
        int16_t w = 0;
        int16_t h = 0;
        int16_t stateW = 0;
        int16_t stateH = 0;
    };
    uint32_t regCount = 0;
    RegEntry registry[16] = {};
};

// -- ZC 发布/回退协议状态 (重构第 3C 步: per-key, 原 EglRenderer 三状态位迁入) --
// 原 EglRenderer::zeroCopyReadyPublished_ / zeroCopyFallbackPending_ /
// zeroCopyFallbackShmSerial_ 是 renderer 实例单套成员 (每 renderer 恰绑一个
// key); 迁移后按 key 存储 — activeKeys_ 集合支持多 key, 状态必须按 key
// 隔离, 防止 B 游戏状态污染 A 游戏。
struct ZcPublishState {
    bool readyPublished = false;    // broker ready marker 已写 (guest 走 ZC)
    bool fallbackPending = false;   // 已撤 ready, 等 shmCommitSerial 越基线
    uint64_t fallbackShmSerial = 0; // fallback 基线 (本次 fallback 起点的 shm serial)
};

// ============================================================================
// P0-1 PresentBinding (2026-09-17): ProducerKey → WindowKey 持久绑定
//
// 依据 Window Identity Probe (docs/STEAM_WINDOW_IDENTITY_20260917.md)：CEF 的
// GPU/present 进程在 Wine 层与窗口 owner 没有任何可用的 handle 关系；producer
// 唯一可靠的输入是 (自身 hostPid, present surface id, frame extent, serial)。
// 因此窗口身份由 Wayland 侧注册表提供，并按方案 §9 分级解析：
//   L0 已有绑定        → 直接使用 (绝不重新做几何搜索)
//   L1 同进程显式路径  → presenterHostPid == ownerHostPid
//   L3 首次几何 bootstrap → 唯一候选 (toplevel + 可见 + 非桌面根 + 尺寸精确相等)
//   其它               → 拒绝 (不选第二候选 / 不用 Z-order / 焦点 / 标题猜)
// 绑定建立后 resize/move 不再重新解析 owner；窗口销毁或代际变化才失效。
// ============================================================================
struct WineHuaWindowKey {
    uint32_t ownerHostPid = 0;
    uint32_t wlSurfaceId = 0;
    uint32_t windowGeneration = 0;
    bool valid() const { return ownerHostPid != 0 && wlSurfaceId != 0; }
};

struct WineHuaProducerKey {
    uint32_t presenterHostPid = 0;
    uint32_t presentSurfaceId = 0;
    uint32_t producerGeneration = 0;
};

struct WineHuaPresentBinding {
    WineHuaProducerKey producer;
    WineHuaWindowKey window;
    uint32_t bindGeneration = 0;
    uint64_t lastSerial = 0;
    const char* reason = "unset";  // "same-process" | "unique-geometry" | ...
    // P0-1 Task A (黑窗归因): producer 侧最新 extent 与"最近被消费"的时刻
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    // 2026-09-20: 语义修正为"最近一次该 producer **真实** present 的时刻"
    // (由 native present 回调 NoteProducerPresent 写入)。旧实现把它放在
    // 渲染循环的查询路径里刷新, 于是失效 producer 永远显示活跃、新 producer
    // 无法接管其窗口 (参见 ROUND3 文档 §7)。
    uint64_t lastProducerUs = 0;
    uint64_t lastDrawUs = 0;       // 最近一次绑定路径真的用于取几何(合成消费)
    // P0-1 Task E/F (2026-09-17): producer 生命周期
    bool pending = false;          // 新 producer 已绑定但还没出第一帧 (旧 producer 仍显示)
    bool retired = false;          // 已被接替 → 旧 producer 的 layer 应释放
};

// TEMP-DIAG(BIND-PRODUCER, 2026-09-18): 每个 producer 的到达/帧链状态。
// 目的: 回答 "win64 native 模式下 CEF 建立了哪些 producer、各自 extent/帧数、
// 被哪条绑定规则拒绝、contentRect 是否精确匹配"。结论成立后按计划收敛。
struct WineHuaBindDiagProducer {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frames = 0;                 // 该 producer 到达次数 (帧链活跃度)
    uint64_t firstUs = 0;
    uint64_t lastUs = 0;
    bool bound = false;
    bool contentRectWouldMatch = false;  // 只诊断: 某候选的 contentRect 是否精确等于 producer extent
    char lastReject[48] = "none";
};

// ZC 层 key 权威簿记 + 几何供给。
// friend of DesktopCompositor: 访问其层容器 (subsurfaceLayers_) / tmgr / policy /
// root 引用, 同 FramePlanner / FrameComposer 模式 — 合成状态仍由
// DesktopCompositor 持有 (本类只迁入 ZC key 权威集合), 锁边界/读写线程域不变。
class ZcBridge {
public:
    explicit ZcBridge(DesktopCompositor& comp) : comp_(comp) {}

    // -- key 簿记 (原 zeroCopySurfaceKeys_ 权威集合迁入) --
    void SetEnabled(uint64_t surfaceKey, bool enabled);  // 原 SetSurfaceZeroCopy
    void RemoveKey(uint64_t surfaceKey);                 // 原 RemoveZeroCopyKeyLocked
    bool IsActive(uint64_t surfaceKey) const { return activeKeys_.count(surfaceKey) > 0; }
    const std::unordered_set<uint64_t>& activeKeys() const { return activeKeys_; }

    // -- ZC 状态机 (重构第 3C 步: 协议 owner, 自 EglRenderer 三方法/三状态位
    //    按 key 化迁入) — 何时发布/回退/确认的时序编排收敛到本类, 全部为
    //    幂等动作方法 (入口守卫与旧实现一致)。时序是协议设计, 不可合并:
    //    发布先 compositor key 后 ready marker (先让合成跳过, 再通知 guest
    //    走 ZC); fallback 分两步 — 先撤 ready (guest 立即切 SHM), 等
    //    shmCommitSerial 越过基线 (新 SHM 帧已到) 再撤 compositor key (恢复
    //    合成), 避免合成到 ZC 前的旧 SHM 帧。methods 全部从渲染线程调用
    //    (原调用点上下文), SetEnabled 内部持 tmgr 锁, 其余方法无锁 —
    //    与原实现一致。broker 的 attached 集合 (IPC 簿记) 由
    //    Attach/DetachZeroCopyTarget 独立维护, 不参与合成判定。
    void Activate(uint64_t surfaceKey, uint32_t rendererToplevelId);  // 原 EglRenderer::PublishZeroCopyActive
    void BeginFallback(uint64_t surfaceKey, uint64_t shmBaseline, bool baselineValid,
                       uint32_t rendererToplevelId);  // 原 UnpublishZeroCopyReady + 失败调用点基线抓取/置位
    bool ConfirmFallback(uint64_t surfaceKey, uint64_t shmSerial);  // 原 ClearZeroCopyCompositorKey + 确认调用点判断/置位
    void CancelFallback(uint64_t surfaceKey);  // 原成功帧恢复路径的 pending 复位
    void Release(uint64_t surfaceKey, uint32_t rendererToplevelId);  // 原 ReleaseZeroCopyBinding 状态复位序列
    void BindSurface(uint64_t surfaceKey, uint64_t initialShmBaseline);  // 原 TryAttachZeroCopySurface 成功路径复位

    bool IsReadyPublished(uint64_t surfaceKey) const;
    bool IsFallbackPending(uint64_t surfaceKey) const;
    uint64_t GetFallbackShmSerial(uint64_t surfaceKey) const;

    // -- 几何供给 (原 DesktopCompositor 方法, 行为平价) --
    bool GetLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                      int fallbackWidth, int fallbackHeight, ZeroCopyLayerInfo& info,
                      const char** outReason = nullptr);
    // 诊断: 填充该 key 在当前合成状态下的原始事实 (不做任何策略判定)
    bool DiagnoseLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                           ZcLayerDiag& out);
    // -- P0-1 PresentBinding (见本头文件上方说明) --
    // 解析/复用 (presenterHostPid, presentSurfaceId) → 窗口绑定；首次唯一解析结果
    // 会被持久化，后续调用直接返回绑定（不再做几何搜索）。
    bool ResolvePresentBinding(uint64_t surfaceKey, uint32_t frameWidth, uint32_t frameHeight,
                               WineHuaPresentBinding* outBinding, bool* outNewlyBound);
    // 2026-09-20 关键修复: producer 活性只能由真实 present 驱动。
    // NoteProducerPresent 由 native present 回调 (OHOS 回调线程) 调用, 只碰本类
    // 自己的小锁 + 表, 不取 tmgr 锁 (回调可能在 UpdateSurfaceImage 内同步触发,
    // 而渲染线程此刻可能正持 tmgr 锁) — 锁序单向: tmgr → presentLivenessMutex_。
    void NoteProducerPresent(uint64_t surfaceKey, uint64_t nowUs);
    // 合成侧真正消费一帧 (渲染线程在 UpdateSurfaceImage 成功后调用)
    void NoteLayerConsumed(uint64_t surfaceKey, uint64_t nowUs);
    // 窗口销毁/代际变化时失效所有指向该窗口的绑定
    void InvalidateBindingsForWindow(uint32_t ownerHostPid, uint32_t wlSurfaceId);
    size_t PresentBindingCount() const { return presentBindings_.size(); }
    // P0-1 Task A: per-window 黑窗归因快照 (调用方须已持有 tmgr 锁)
    void DumpWindowBindingDiag();
    int GetOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                     ZeroCopyOccluderRect* out, int maxOut);
    bool HasLayerForToplevel(uint32_t id) const;
    bool GetContentSize(uint32_t toplevelId, int& outW, int& outH) const;

private:
    DesktopCompositor& comp_;
    std::unordered_set<uint64_t> activeKeys_;  // ZC key 权威
    std::unordered_map<uint64_t, ZcPublishState> publishStates_;  // key → ZC 发布状态
    // 真实 present 活性表 (producer key → 最近一次 present 的 steady 时钟 µs)。
    // 只由 NoteProducerPresent 写、由 ResolvePresentBinding / 归因诊断读。
    mutable std::mutex presentLivenessMutex_;
    std::unordered_map<uint64_t, uint64_t> lastPresentUsByKey_;
    uint64_t LastPresentUs(uint64_t surfaceKey) const;
    // 清理"窗口还锁着一个已经没有绑定/资源已消失的 producer"的残留映射,
    // 否则该窗口永远无法被新 producer 接管 (ROUND3 §7.2)。
    void PruneStaleWindowBindings();
    // P0-1: producer key ((presenterHostPid<<32)|presentSurfaceId) → 持久绑定
    std::unordered_map<uint64_t, WineHuaPresentBinding> presentBindings_;
    // 已被其它 producer 占用的窗口 (防止两个 producer 绑到同一窗口)
    std::unordered_map<uint64_t, uint64_t> windowBindings_;
    std::unordered_set<uint64_t> bindingRejectedLogged_;
    std::unordered_map<uint64_t, WineHuaBindDiagProducer> bindDiagProducers_;
    struct BlankShmFrame {
        uint64_t serial = 0;
        bool blank = false;
        bool valid = false;
    };
    std::unordered_map<uint32_t, BlankShmFrame> blankShmFrames_;
    // TEMP-DIAG(BIND): 去重表 (有界 vector — OHOS libc++ 无 std::hash<std::string>)
    std::vector<std::string> bindDiagWindowLogged_;
    std::vector<std::string> bindDiagRejectLogged_;
    // TEMP-DIAG(BIND-PRODUCER): 候选/拒绝原因诊断 (只观察, 不绑定)
    void BindDiagPass(uint64_t surfaceKey, uint32_t frameWidth, uint32_t frameHeight,
                      uint32_t presenterHostPid, uint32_t presentSurfaceId, uint32_t rootId);
};

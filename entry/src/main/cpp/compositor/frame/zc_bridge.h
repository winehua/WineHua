#pragma once
#include <cstdint>
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
                      int fallbackWidth, int fallbackHeight, ZeroCopyLayerInfo& info);
    int GetOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                     ZeroCopyOccluderRect* out, int maxOut);
    bool HasLayerForToplevel(uint32_t id) const;
    bool GetContentSize(uint32_t toplevelId, int& outW, int& outH) const;

private:
    DesktopCompositor& comp_;
    std::unordered_set<uint64_t> activeKeys_;  // ZC key 权威
    std::unordered_map<uint64_t, ZcPublishState> publishStates_;  // key → ZC 发布状态
};

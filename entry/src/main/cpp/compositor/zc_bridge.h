#pragma once
#include <cstdint>
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
};

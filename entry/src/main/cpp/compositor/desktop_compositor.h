#pragma once
#include <wayland-server-core.h>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "display_policy.h"
#include "geometry.h"
#include "toplevel_manager.h"

class ToplevelManager;

// -- 公共类型 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用) --

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
    bool protocolOnly = false;
    bool fullscreen = false;  // 所属 toplevel 全屏: GL 层保比例缩放铺满视口 (ZC 游戏)
};

struct ZeroCopyOccluderRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// -- 帧合成 + 零拷贝 layer 管理 --
// 依赖 ToplevelManager (只读), 通过构造时注入的引用访问。
// 所有读写 subsurface/zero-copy 状态的方法由自身持有数据, 加锁约定同 WaylandServer。
//
// 不变式:
// - desktop root 帧是合成基底: TakeToplevelFrame(rootId) 输出整屏合成
//   结果, 其它 toplevel 帧只在其上叠加, 永不替代。
// - zero-copy GL 层与 CPU 合成按帧互斥 (非并存): 走 GL overlay 的帧,
//   被上层窗口遮挡的区域用桌面纹理重绘恢复层序 (egl_renderer occluder
//   redraw); GPU→CPU fallback 时 key 移出 zeroCopySurfaceKeys_, 该层
//   自动回归普通 CPU 合成与置顶命中, 无需特判。
// - desktop root 不参与可见性判定 (契约在 ToplevelManager::IsToplevelVisibleLocked)。
// - 层序单一数据源 (阶段 1, 行为等价): 一帧桌面的内容来源统一为
//   CompositorLayer 列表 (BuildLayerListLocked), 合成与输入遍历同一个
//   按 zIndex 升序的列表; 各层的合成/命中特判逻辑原样保留 (等价形式),
//   阶段 2 起 ZC 层入列参与层序。

class DesktopCompositor {
public:
    // subsurface 合成层 (独立于 per-toplevel 帧缓冲, 避免污染)
    struct SubsurfaceLayer {
        wl_resource* surface = nullptr;
        uint64_t surfaceKey = 0;
        std::vector<uint8_t> pixels;
        int x = 0, y = 0, w = 0, h = 0;
        int localX = 0, localY = 0;
        uint64_t shmCommitSerial = 0;
        uint32_t parentToplevel = 0;
        uint32_t shmFormat = 1;
        bool opaque = false;
        int32_t dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;  // damage 包围盒
        int32_t vpDstW = -1, vpDstH = -1;                // viewport destination
        bool isExternal = false;  // 外部菜单 (任务栏等), 输入坐标需用 Wine 基底
    };

    // -- 层序单一数据源 (阶段 1: 行为等价重构) --
    // 一帧桌面的所有内容来源统一为 Layer; 合成与输入遍历同一按 zIndex 升序
    // 的 Layer 列表 (BuildLayerListLocked)。zIndex 分配: root=0 < toplevel
    // (按 toplevelZOrder_ 顺序) < subsurface (原顺序) — 与旧双循环顺序等价。
    // 阶段 1 仅收敛遍历源, 各层合成/命中的特判逻辑保留等价形式 (不动行为);
    // ZC 层阶段 1 仍由合成/输入跳过, 阶段 2 起入列参与层序。
    // 阶段 3: zcActive 为 ZC 层状态单一字段 (合成/输入/遮挡重绘只认它)。
    // sub/st 指针指向调用方持有的容器, 必须在 ToplevelManager 锁内使用。
    struct CompositorLayer {
        enum class Type { Root, Toplevel, Subsurface };
        Type type = Type::Root;
        size_t zIndex = 0;
        bool visible = false;    // 可见性判定结果 (Root 恒 true, 不参与命中)
        // ZC 层状态单一字段: 该层走 GPU 内容 (合成/输入跳过, 内容由
        // egl_renderer GPU 层自绘); false = fallback 到 CPU 内容 (合成/
        // 命中照常)。由 zeroCopySurfaceKeys_ 派生 — 该集合是 compositor
        // 侧唯一权威, broker 的 attached 簿记 / ready marker (guest 选路)
        // 只是它的执行投影, 不参与合成判定。
        bool zcActive = false;
        uint32_t toplevelId = 0; // 归属窗口 (Root 为 0; Subsurface 为 parentToplevel)
        int x = 0, y = 0, w = 0, h = 0;  // 坐标 (桌面合成: 桌面坐标; 窗口内: 窗口局部坐标)
        bool fullscreen = false; // Toplevel: 全屏标记
        const SubsurfaceLayer* sub = nullptr;  // Type==Subsurface 时引用原层

        // 该层是否参与 CPU 合成/命中: ZC 层 (GPU 自绘, 合成/输入/覆盖判定
        // 跳过) 或不可见层 (不显示不命中)。消费方判跳过一律用此谓词, 不要
        // 直接摸 zcActive/visible — 规则变更只改这里 (等价性: desktop 模式
        // toplevel 层 zcActive 恒 false; 全屏窗口的 subsurface visible 恒
        // true — 父窗口已被 fs-pick 确认可见)。
        bool ShouldSkipCpu() const { return !visible || zcActive; }
    };

    // 构造: 注入 ToplevelManager + 桌面合成配置 (由 WaylandServer 持有,
    // policy 为引用 — SetDesktopMode 后随动)
    DesktopCompositor(ToplevelManager& tmgr,
                      const DisplayPolicy& policy,
                      const uint32_t& desktopRootToplevelId,
                      const int32_t& outputW,
                      const int32_t& outputH);

    // -- 帧输出 --
    // 取指定 toplevel 的最新帧 (桌面模式合成到 root framebuffer)
    bool TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h);

    // 本帧重绘矩形 (root 坐标, 局部合成范围)。full=true 走整帧合成路径
    // (几何/层序/root 帧/全屏变化时, 行为与旧实现一致); 局部时仅 R 内像素
    // 从快照沿 z 序重建, R 外复用上帧输出内容 (渲染线程帧缓冲跨帧保留)。
    struct DamageRect {
        int x = 0, y = 0, w = 0, h = 0;
        bool full = true;
        bool empty() const { return !full && (w <= 0 || h <= 0); }
    };

    // -- 层序单一数据源 --
    // 构建按 zIndex 升序的 Layer 列表 (调用方须已持有 tmgr mutex)。
    // 合成 (TakeToplevelFrame) 与输入 (InputResolver) 遍历同一列表;
    // rootW/rootH 用于 Root 层几何 (输入侧仅作占位, 不参与命中)。
    std::vector<CompositorLayer> BuildLayerListLocked(int rootW, int rootH);

    // 窗口内 Layer 列表 (阶段 3, PC 模式): 单窗口合成数据源, 与
    // BuildLayerListLocked 对称但用窗口局部坐标:
    //   zIndex: Root(窗口帧) < Subsurface(窗口内局部坐标) < ZC 层(最顶)
    // 窗口间层序不在此管理 (系统合成器)。PC 模式 subsurface 全部转 popup
    // 伪 toplevel (UpdatePopupOnCommit), 窗口内 subsurface 当前恒空 —
    // 层序结构为窗口内内容扩展预留; ZC 层 (zcActive) 在层序最顶, 合成跳过
    // (GPU 自绘覆盖, 与 desktop 模式同语义)。调用方须已持有 tmgr mutex。
    std::vector<CompositorLayer> BuildWindowLayerListLocked(uint32_t toplevelId,
                                                            int winW, int winH);

    // 全屏目标选取 (阶段 4, S3 收敛): 渲染 (TakeToplevelFrame) 与输入
    // (FindInputTargetAt) 共用的唯一实现 — 可见全屏窗口中取 fsPriority
    // 最大者, 返回其 toplevelId (0 = 无全屏窗口)。多窗口可同时 fullscreen
    // (显示模式切换时 Wine 会把足够大的旧窗口连带标记, 请求到达顺序不定 —
    // 2026-07 实测 notepad 被连带标记并压在游戏上), 规则原因/局限见
    // ToplevelState::fsPriority 注释。调用方须已持有 tmgr mutex;
    // 返回 id 对应的 state 由调用方锁内查询 (pick 时已确认非空)。
    uint32_t PickFullscreenLayerLocked(const std::vector<CompositorLayer>& layers) const;

    // 非主全屏窗口 (显示模式切换时被 winewayland 连带标记的旧窗口) 是否应
    // 跳过合成/命中 — 渲染 blitToplevel/blitSubsurface 与输入
    // FindInputTargetAt 共用的唯一实现 (收敛前各有一份独立规则)。规则:
    // fsOk 存在主全屏窗口时, toplevel 层看自身 fullscreen 标记, subsurface
    // 层看父 toplevel 的 IsFullscreen(); 非全屏弹窗/对话框 (及其 subsurface)
    // 不跳过。调用方须已持有 tmgr mutex。
    static bool ShouldSkipFullscreenCascade(const CompositorLayer& layer,
                                            uint32_t fullscreenId, bool fsOk,
                                            ToplevelManager& tmgr);

    // 全屏内容 fit 几何 (渲染/输入共用): 内部做全屏内容尺寸选择
    // (SelectFullscreenContentSize: ZC 游戏用 zero-copy 层实际内容几何,
    // SHM 用 buffer 尺寸) + ComputeFitRect — 该规则的唯一实现, 替换两侧
    // 各自组合。调用方须已持有 tmgr mutex; 找不到 toplevel state 返回 false。
    bool ComputeFullscreenFitLocked(uint32_t toplevelId, int rootW, int rootH,
                                    FitRect& out) const;

    // -- Zero-copy layer 管理 --
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              int fallbackWidth, int fallbackHeight,
                              ZeroCopyLayerInfo& info);
    void SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled);
    int GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                             ZeroCopyOccluderRect* out, int maxOut);

    // -- Subsurface layer 位置解析 (InputResolver 调用) --
    void ResolveSubsurfaceLayerPositionLocked(const SubsurfaceLayer& layer,
                                              int& x, int& y) const;

    // -- 桌面 root dirty 标记 --
    void MarkDesktopRootDirtyLocked();

    // -- Subsurface layer 生命周期 (替代直接操作 subsurfaceLayers_) --

    // 更新 subsurface layer 的本地偏移 (subsurface_set_position 调用)
    void UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y);

    // 移除指定 surface 对应的 layer。返回是否实际移除 (调用方据此决定是否 mark dirty)。
    bool RemoveSubsurfaceLayer(wl_resource* surface);

    // 插入或替换 layer (按 surface 匹配)。`layer` 应已填充除 pixels 外的所有字段。
    // `newPixels` 是 sd->pixels 中刚提交的帧数据, 被移入 layer。
    // 返回旧 layer 的 pixels (新插入时为空), 供调用方归还给 sd->pixels 做双缓冲轮转。
    std::vector<uint8_t> UpsertSubsurfaceLayer(SubsurfaceLayer&& layer,
                                               std::vector<uint8_t>&& newPixels);

    // 在 sibling 之上/下移动 child layer。child 和 sibling 必须已存在。
    // 返回是否实际改变了合成顺序，调用方据此避免无效的 root 重绘。
    bool ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling);
    bool ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling);

    // 移除 zero-copy key (调用方须已持有 mutex)
    void RemoveZeroCopyKeyLocked(uint64_t surfaceKey);

    // Increment root frame serial (called from surface_commit when root commits)
    void IncrementDesktopRootFrameSerial() { ++desktopRootFrameSerial_; }

    // toplevel 是否有 zero-copy GL 层 (ZC 游戏判定: 全屏渲染/输入映射分流用,
    // 调用方须已持有 tmgr mutex)
    bool HasZeroCopyLayerForToplevelLocked(uint32_t id) const;
    // 取 toplevel 的 zero-copy subsurface 层实际内容尺寸 (vpDst 裁剪后,
    // 与 GetZeroCopyLayerInfo / egl_renderer 渲染视口同规则) — 全屏内容
    // 尺寸的单一权威源, 输入 fit 与渲染视口同源才保证逆映射严格互逆。
    // 返回是否有 ZC 层; 无 ZC 层时 outW/outH 保持 0 (SelectFullscreenContentSize
    // 退化为 buffer 尺寸)。调用方须已持有 tmgr mutex。
    bool GetZeroCopyContentSizeLocked(uint32_t toplevelId, int& outW, int& outH) const;

private:
    // toplevel 的 zero-copy subsurface 层查找 (上面两个查询的单一实现,
    // 同一遍历同一谓词; 返回首个匹配层, 调用方须已持有 tmgr mutex)
    const SubsurfaceLayer* FindZeroCopyLayerForToplevelLocked(uint32_t id) const;

    // -- TakeToplevelFrame 阶段拆分 (重构第 2A 步: 纯结构拆分, 行为平价) --
    // 桌面分支按原内联段拆为命名阶段方法, 分"锁内规划 / 锁外绘制"两段:
    // Plan*Locked 在 tmgr 锁内运行并产出 DesktopTakePlan; Blit*/Composite*
    // 锁外纯像素, 只读 plan 快照。锁边界与原函数逐段对应 (原 lk.unlock()
    // 前的代码全在规划段, 之后的全在绘制段), desktopRootFrameSerial_/
    // snapPool_/lastSubSerial_/lastTopSerial_/desktopCompositionSignature_
    // 的读写线程域不变。
    using TakeClock = std::chrono::steady_clock;

    /*
     * 层间快照结构 (BlitSource): 把 blit 要读的全部源像素/元数据拷成
     * 私有副本, 随后立即解锁, blit 在锁外进行。动机 (实测): 旧实现持锁
     * 完成整帧 CPU blit (1400x920 全屏合成 ~25ms), wl 事件循环线程的
     * commit 与输入派发同抢 tmgr_ 锁 — commit 实测平均被堵 27ms
     * (p95 ~90ms), 输入注入 NAPI→INJ 中位 8ms; 快照仅 ~1-3ms memcpy,
     * 锁占用 ↓10 倍。正确性: 快照后 wl 线程的新 commit 只影响下一帧
     * (dirty 重新置位), 与本帧 blit 无共享指针; layer.sub /
     * ToplevelState 指针解锁后失效, 故所需字段全部拷入 BlitSource。
     */
    struct BlitSource {
        const std::vector<uint8_t>* pixels = nullptr;  // 指向 snapPool_ 条目 (ZC 层为空)
        int w = 0, h = 0;        // 源像素尺寸 (toplevel: Width/Height; sub: sl.w/h)
        int x = 0, y = 0;        // toplevel 屏幕位置 (cst->X/Y)
        uint32_t shmFormat = 1;  // 0=ARGB8888 1=XRGB8888
        bool opaque = false;     // sub: 不透明标记
        int vpDstW = 0, vpDstH = 0;           // sub: viewport 目标尺寸
        int dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;  // sub: damage 矩形
        bool skip = false;       // 预计算: 本帧不参与合成/快照 (见下方赋值注释)
    };

    // 一帧桌面合成的规划产物: 规划段 (锁内) 填, 绘制段 (锁外) 消费。
    // layers 的 sub 指针解锁后失效 — 绘制段只读各层值字段 (type/toplevelId/
    // x/y/w/h), 不得解引用 sub (所需字段已全部拷入 srcs)。
    struct DesktopTakePlan {
        std::vector<CompositorLayer> layers;  // 层列表 (锁内构建)
        std::vector<BlitSource> srcs;         // 与 layers 等长 (锁内快照)
        int rootW = 0, rootH = 0;
        uint32_t fullscreenId = 0;
        bool hasFullscreen = false;
        // ZC 游戏 (画面在 zero-copy GL 层): 全屏独占输出, 见绘制段填黑分支
        bool isZcGame = false;
        int fullscreenX = 0, fullscreenY = 0;
        FitRect transform;
        bool fullscreenContentCovered = false;
        // -- 规划段内部中间态 (绘制段不读) --
        uint64_t compositionSignature = 0;
        bool rebuildBase = false;
        // -- 本帧重绘矩形 (局部合成范围) --
        DamageRect dmg;
        // -- 日志计数 + 分段计时点 (锁内捕获, 锁外 [GL-TAKE]/[MW-TAKE] 用) --
        size_t nZOrder = 0;
        size_t nSubLayers = 0;
        TakeClock::time_point rootCopied;
        TakeClock::time_point snapshotDone;
    };

    // 规划结果: kNoFrame=无新帧 (返回 false); kFastPath=无子窗口快进 (out/w/h
    // 已填, 返回 true); kDirectPass=SHM 全屏直传 (out/w/h 已填+日志, 返回
    // true); kCompose=需锁外合成 (plan 已备)。
    enum class TakePlanOutcome { kNoFrame, kFastPath, kDirectPass, kCompose };

    // -- 锁内规划段 (调用方须已持有 tmgr mutex) --
    TakePlanOutcome PlanDesktopFrameLocked(uint32_t id, bool frameTrace,
                                           TakeClock::time_point takeStarted,
                                           TakeClock::time_point lockAcquired,
                                           std::vector<uint8_t>& out, int& w, int& h,
                                           DesktopTakePlan& plan);
    // 阶段 1: dirty 门控 + 无子窗口快进
    TakePlanOutcome GateDesktopDirtyLocked(uint32_t id, DesktopTakePlan& plan,
                                           std::vector<uint8_t>& out, int& w, int& h,
                                           ToplevelManager::ToplevelState*& rst);
    // 阶段 2: 全屏 pick/fit
    void PlanFullscreenLocked(DesktopTakePlan& plan);
    // 阶段 3: fullscreenContentCovered 覆盖检测
    bool DetectFullscreenContentCoveredLocked(const DesktopTakePlan& plan) const;
    // 阶段 4: SHM 全屏直传判定 (通过即填好 out/w/h 并打直传日志, 返回 true)
    bool TryShmFullscreenDirectLocked(uint32_t id, ToplevelManager::ToplevelState* rst,
                                      bool frameTrace,
                                      TakeClock::time_point takeStarted,
                                      TakeClock::time_point lockAcquired,
                                      DesktopTakePlan& plan,
                                      std::vector<uint8_t>& out, int& w, int& h);
    // 阶段 5: 合成签名 FNV 哈希 (rebuildBase 判定在 PlanDesktopFrameLocked)
    uint64_t ComputeCompositionSignatureLocked(uint32_t id, const DesktopTakePlan& plan) const;
    // 阶段 6: 局部合成重绘矩形 R 计算 (含 BlitSource skip 预计算);
    // 返回 false = R 为空 (无内容变化, 本帧不产出, 已 ClearDirty)
    bool ComputeDamageRectLocked(ToplevelManager::ToplevelState* rst, DesktopTakePlan& plan);
    // 阶段 7: 基底重建/局部基底拷贝
    void CopyBaseToOutputLocked(const ToplevelManager::ToplevelState* rst,
                                DesktopTakePlan& plan, std::vector<uint8_t>& out);
    // 阶段 8: 锁内快照 (BlitSource/snapPool/ARGB opaque 融合扫描) + ClearDirty
    void SnapshotBlitSourcesLocked(ToplevelManager::ToplevelState* rst, DesktopTakePlan& plan);

    // -- 锁外绘制段 (纯像素, 不碰 tmgr 锁) --
    static void BlitToplevel(const DesktopTakePlan& plan, const CompositorLayer& layer,
                             const BlitSource& bs, std::vector<uint8_t>& composited);
    static void BlitSubsurface(const DesktopTakePlan& plan, const CompositorLayer& layer,
                               const BlitSource& bs, std::vector<uint8_t>& composited);
    // 合成主循环 + 每帧 [GL-TAKE]/[MW-TAKE] 日志 (frameTrace 门控)
    void CompositeDesktopFrame(uint32_t id, bool frameTrace,
                               TakeClock::time_point takeStarted,
                               TakeClock::time_point lockAcquired,
                               const DesktopTakePlan& plan,
                               std::vector<uint8_t>& out, int& w, int& h);
    // PC 模式窗口内 subsurface blit (纯像素; PC 路径全程持锁, 与原实现一致)
    static void BlitWindowSubsurface(const CompositorLayer& layer, int winW, int winH,
                                     std::vector<uint8_t>& out);
    // PC 模式单窗口分支 (锁内)
    bool TakeWindowFrameLocked(uint32_t id, std::vector<uint8_t>& out, int& w, int& h,
                               bool frameTrace);

    ToplevelManager& tmgr_;
    const DisplayPolicy& policy_;
    const uint32_t& desktopRootToplevelId_;
    const int32_t& outputW_;
    const int32_t& outputH_;

    std::vector<SubsurfaceLayer> subsurfaceLayers_;
    std::unordered_set<uint64_t> zeroCopySurfaceKeys_;
    uint64_t desktopCompositionSignature_ = 0;
    uint64_t desktopOutputRootFrameSerial_ = 0;
    bool desktopOutputInitialized_ = false;
    uint64_t desktopRootFrameSerial_ = 0;
    // TakeToplevelFrame 快照缓冲池 (仅渲染线程访问): 跨帧复用容量,
    // 避免每帧新建多 MB vector 的分配+缺页开销 — 见 cpp 快照阶段注释
    std::vector<std::vector<uint8_t>> snapPool_;
    // 帧内容 serial 基准 (局部合成, 仅渲染线程访问): 记录上一次合成时各层
    // 看到的像素序列号 — 下一帧以此判定层内容是否更新 (sub=shmCommitSerial,
    // toplevel=FrameSerial)。层键: sub 用 surfaceKey, toplevel 用 id。
    std::unordered_map<uint64_t, uint64_t> lastSubSerial_;
    std::unordered_map<uint64_t, uint64_t> lastTopSerial_;
};

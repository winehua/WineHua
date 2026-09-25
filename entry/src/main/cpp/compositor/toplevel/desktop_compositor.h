#pragma once
#include <wayland-server-core.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compositor_layer.h"
#include "display_policy.h"
#include "geometry.h"
#include "presented_frame.h"
#include "zc_bridge.h"  // ZC 层几何/状态类型与 ZcBridge (原 ZeroCopyLayerInfo/ZeroCopyOccluderRect)

class ToplevelManager;

// -- 公共类型 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用) --
// ZeroCopyLayerInfo / ZeroCopyOccluderRect 已迁至 zc_bridge.h, 仍为全局作用域,
// 本头文件 include zc_bridge.h 即可见; wayland_server.h 的 using 别名不变。

// -- 帧合成 + 零拷贝 layer 管理 --
// 依赖 ToplevelManager (只读), 通过构造时注入的引用访问。
// 所有读写 subsurface/zero-copy 状态的方法由自身持有数据, 加锁约定同 WaylandServer。
//
// 不变式:
// - desktop root 帧是合成基底: TakeToplevelFrame(rootId) 输出整屏合成
//   结果, 其它 toplevel 帧只在其上叠加, 永不替代。
// - zero-copy GL 层与 CPU 合成按帧互斥 (非并存): 走 GL overlay 的帧,
//   被上层窗口遮挡的区域用桌面纹理重绘恢复层序 (egl_renderer occluder
//   redraw); GPU→CPU fallback 时 key 移出 ZcBridge::activeKeys_ (zc_), 该层
//   自动回归普通 CPU 合成与置顶命中, 无需特判。
// - desktop root 不参与可见性判定 (契约在 ToplevelManager::IsToplevelVisibleLocked)。
// - 层序单一数据源 (阶段 1, 行为等价): 一帧桌面的内容来源统一为
//   CompositorLayer 列表 (BuildLayerListLocked), 合成与输入遍历同一个
//   按 zIndex 升序的列表; 各层的合成/命中特判逻辑原样保留 (等价形式),
//   阶段 2 起 ZC 层入列参与层序。

class DesktopCompositor {
public:
    // 层类型定义在 compositor_layer.h (模块级数据契约: 合成与输入共用);
    // 保留类作用域别名兼容既有写法 (先例同 ZeroCopyLayerInfo 迁 zc_bridge.h)。
    using SubsurfaceLayer = ::SubsurfaceLayer;
    using CompositorLayer = ::CompositorLayer;


    // 构造: 注入 ToplevelManager + 桌面合成配置 (存储在 WaylandServer 的
    // DesktopSessionState POD — 重构第 6B 步, 注入引用指向 session_ 字段;
    // policy 随 SetDesktopMode 随动, rootId/output 随会话状态变化即见)
    DesktopCompositor(ToplevelManager& tmgr,
                      const DisplayPolicy& policy,
                      const uint32_t& desktopRootToplevelId,
                      const int32_t& outputW,
                      const int32_t& outputH);

    // -- 帧输出 --
    // 取指定 toplevel 的最新帧 (桌面模式合成到 root framebuffer)。
    // out 为像素载体 (调用方持有, 跨帧复用 — 局部合成 R 外保留上帧内容);
    // frame 为帧交付契约 (presented_frame.h): 坐标空间/内容尺寸/alpha 语义
    // 由产出侧填好, 消费方从字段取几何。
    // 取帧路径按 DisplayPolicy::FrameRouteFor 路由 (任务 2): Desktop root 帧
    // 走 DesktopRootFrameComposer, PC 单窗口帧走 WindowFrameComposer — 本函数
    // 只剩纯编排, 不再按 id==root 在自身内分 PC/Desktop 合成逻辑。
    bool TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, PresentedFrame& frame);

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

    // 窗口内 Layer 列表 (阶段 3, 多窗口模式 — PC 窗口模式与 Pad 多窗口模式
    // 共用): 单窗口合成数据源, 与 BuildLayerListLocked 对称但用窗口局部坐标:
    //   zIndex: Root(窗口帧) < 内嵌客户区 Subsurface(窗口局部坐标) < ZC 层(最顶)
    // 窗口间层序不在此管理 (系统合成器)。只收 route=InlineClient 的子层
    // (多窗口客户区), Popup 类走伪 toplevel + 独立子窗口 (见
    // DisplayPolicy::RouteForSubsurface); ZC 层 (zcActive) 在层序最顶,
    // 合成跳过 (GPU 自绘覆盖, 与 desktop 模式同语义)。
    // 调用方须已持有 tmgr mutex。
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

    // -- Zero-copy layer 管理 (任务 3-A: 已抽离到 ZcBridge, 本类经 zc_ 委托) --
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              int fallbackWidth, int fallbackHeight,
                              ZeroCopyLayerInfo& info,
                              const char** outReason = nullptr) {
        return zc_.GetLayerInfo(surfaceKey, rendererToplevelId, fallbackWidth, fallbackHeight,
                                info, outReason);
    }
    int GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                             ZeroCopyOccluderRect* out, int maxOut) {
        return zc_.GetOccluders(surfaceKey, rendererToplevelId, out, maxOut);
    }
    // (SetSurfaceZeroCopy 入口已删: zero-copy 开关经 ZcBridge 状态机幂等动作
    // Activate/Release — Activate 内部调 SetEnabled, 入口外部零调用方,
    // 重构第 6A 步死转发清理)

    // ZC 协议 owner 访问 (重构第 3C 步: 渲染器直调 ZC 状态机动作 — 6A 转
    // 发删除后 EglRenderer 经注入的本类引用持 zc() 直连)
    ZcBridge& zc() { return zc_; }
    const ZcBridge& zc() const { return zc_; }

    // -- Subsurface layer 位置解析 (InputResolver 调用) --
    void ResolveSubsurfaceLayerPositionLocked(const SubsurfaceLayer& layer,
                                              int& x, int& y) const;

    // -- 配置只读访问 (装配出口, 重构第 6A 步) --
    // 本类经构造注入了 policy/rootId 的共享引用 (与 InputResolver 同源),
    // 渲染器 (经 plugin_manager 注入本类引用) 经此读同值配置 — 替代
    // WaylandServer::Policy()/GetDesktopRootToplevelId() 门面转发。
    const DisplayPolicy& Policy() const { return policy_; }
    uint32_t DesktopRootToplevelId() const { return desktopRootToplevelId_; }

    // -- 桌面 root dirty 标记 --
    void MarkDesktopRootDirtyLocked();

    // -- Subsurface layer 生命周期 (替代直接操作 subsurfaceLayers_) --

    // 更新 subsurface layer 的本地偏移 (subsurface_set_position 调用)。
    // 命中时经 out 返回该 layer 的归属 (父 toplevel + 承载路由), 供调用方
    // 按承载方式标脏 (DesktopLayer→root 帧; InlineClient→父窗口帧; Popup 不经此
    // 容器)。未命中返回 false (调用方不标脏)。
    bool UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y,
                                            uint32_t& outParentToplevel,
                                            DisplayPolicy::SubsurfaceRoute& outRoute);

    // 移除指定 surface 对应的 layer。返回是否实际移除 (调用方据此决定是否
    // mark dirty); 命中时经 out 返回其归属 (父 toplevel + 承载路由), 供调用方
    // 按承载方式标脏 (见 WaylandServer::MarkLayerHostDirtyLocked)。
    bool RemoveSubsurfaceLayer(wl_resource* surface, uint32_t& outParentToplevel,
                               DisplayPolicy::SubsurfaceRoute& outRoute);

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
    void RemoveZeroCopyKeyLocked(uint64_t surfaceKey) { zc_.RemoveKey(surfaceKey); }

    // Increment root frame serial (called from surface_commit when root commits)
    // — 存储为 atomic (重构第 6B 步, 见成员处注释: 读写两侧均持 tmgr 锁,
    // atomic 为防御性收口; 调用点/线程域不变)
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
    // 桌面合成管线在 frame_pipeline.{h,cpp}: FramePlanner (锁内规划) 产出
    // FramePlan, FrameBlitter (锁外纯像素) 消费, TakeToplevelFrame 本体只剩
    // 编排。Planner 经 friend 访问本类层容器/快照池/合成状态 — 状态仍由本类
    // 持有, 读写线程域不变 (渲染线程 + tmgr 锁内); 锁边界与原单函数一致。
    friend class FramePlanner;
    // 取帧路径策略实现 (任务 2, 重构第 2B 步): DesktopRootFrameComposer /
    // WindowFrameComposer 经 friend 访问 tmgr_ (锁/查询) 复刻原
    // TakeToplevelFrame desktop 分支与 TakeWindowFrameLocked — 两条路径的
    // 合成状态 (snapPool_/subsurfaceLayers_/zeroCopySurfaceKeys_) 仍由本类持有。
    friend class FrameComposer;
    friend class DesktopRootFrameComposer;
    friend class WindowFrameComposer;
    // ZC 层几何供给与 key 簿记 (任务 3-A, 重构第 3 步): ZcBridge 经 friend
    // 访问本类的层容器 (subsurfaceLayers_) / tmgr / policy / root 引用与
    // dirty 标记 — ZC key 权威集合 (activeKeys_) 已迁入 ZcBridge, 本类经
    // zc_ 委托; 锁边界/读写线程域不变 (tmgr 锁内, 与 FramePlanner 一致)。
    friend class ZcBridge;

    ToplevelManager& tmgr_;
    const DisplayPolicy& policy_;
    const uint32_t& desktopRootToplevelId_;
    const int32_t& outputW_;
    const int32_t& outputH_;

    // ZC 层状态与几何供给 (任务 3-A 抽离): 构造时绑定 *this (friend 访问
    // 本类层容器/tmgr/policy/root/dirty)。
    ZcBridge zc_;

    std::vector<SubsurfaceLayer> subsurfaceLayers_;
    uint64_t desktopCompositionSignature_ = 0;
    uint64_t desktopOutputRootFrameSerial_ = 0;
    bool desktopOutputInitialized_ = false;
    /* desktopRootFrameSerial_ — desktop root 全局帧序号 (重构第 6B 步原子化)。
     * 语义: wl 线程在 root commit 时 ++ (IncrementDesktopRootFrameSerial,
     * wl_core.cpp UpdateToplevelFrameOnCommit, tmgr 锁内), 渲染线程在
     * FramePlanner 锁内段读 (rebuildBase 判定/CopyBaseToOutputLocked 回写
     * desktopOutputRootFrameSerial_, frame_pipeline.cpp) — 读写两侧均持
     * tmgr mutex, 无 data race; 原子化为防御性收口 (锁协议改变时防 TSan 类
     * 隐性撕裂；锁纪律不变 — 不因 atomic 引入新无锁访问, 持锁访问保持锁内)。
     * 核实结论 (PLAN §四阶段6): 与 ToplevelState::frameSerial_ (per-toplevel
     * dirty 序号, 各层内容变化判定) 是不同概念, 不合一 — 前者是 root 帧
     * 级"根帧又新了"的全局序号, 后者是 each-toplevel 内容版本号。 */
    std::atomic<uint64_t> desktopRootFrameSerial_{0};
    // TakeToplevelFrame 快照缓冲池 (仅渲染线程访问): 跨帧复用容量,
    // 避免每帧新建多 MB vector 的分配+缺页开销 — 见 cpp 快照阶段注释
    std::vector<std::vector<uint8_t>> snapPool_;
    // 帧内容 serial 基准 (局部合成, 仅渲染线程访问): 记录上一次合成时各层
    // 看到的像素序列号 — 下一帧以此判定层内容是否更新 (sub=shmCommitSerial,
    // toplevel=FrameSerial)。层键: sub 用 surfaceKey, toplevel 用 id。
    std::unordered_map<uint64_t, uint64_t> lastSubSerial_;
    std::unordered_map<uint64_t, uint64_t> lastTopSerial_;
};

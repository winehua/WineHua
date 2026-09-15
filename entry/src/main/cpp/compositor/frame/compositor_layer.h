#pragma once
#include <wayland-server-core.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "display_policy.h"

// 合成层数据契约: 一帧桌面的内容来源统一为 CompositorLayer 列表 — 渲染
// (FramePlanner/FrameBlitter) 与输入 (InputResolver) 遍历同一按 zIndex 升序
// 的列表, 层序/几何/可见性判定共用同一份数据。消费方不必经
// DesktopCompositor 的类作用域使用它们 (该类保留同名别名兼容既有写法)。

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
    // ARGB 精确 opaque 的缓存 (IsFullyOpaqueArgb 结果): 判定成本为一次
    // 全层扫描, 按内容序列号缓存 — 仅当像素实际重写 (serial 变化) 时重算。
    // 消费方: desktop 快照扫描写回 / 窗口内 blit 读用 (两条路径同一语义)。
    uint64_t opaqueCheckedSerial = 0;
    int32_t dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;  // damage 包围盒
    int32_t vpDstW = -1, vpDstH = -1;                // viewport destination
    bool isExternal = false;  // 外部菜单 (任务栏等), 输入坐标需用 Wine 基底

    // 承载路由 (DisplayPolicy::SubsurfaceRoute 的落库副本, 见该枚举注释):
    // DesktopLayer = root 帧合成; InlineClient = 父窗口帧合成 (多窗口模式
    // 客户区)。两者都存本容器 (desktop 用桌面坐标, inline 用窗口局部
    // 坐标), 消费方按本字段/父窗口过滤, 不得用 isExternal 反推承载方式
    // (isExternal 只表达"坐标是否 Wine 基底", 语义正交)。
    DisplayPolicy::SubsurfaceRoute route =
        DisplayPolicy::SubsurfaceRoute::DesktopLayer;
};

// -- 层序单一数据源 (阶段 1: 行为等价重构) --
// 一帧桌面的所有内容来源统一为 Layer; 合成与输入遍历同一按 zIndex 升序
// 的 Layer 列表 (DesktopCompositor::BuildLayerListLocked)。zIndex 分配:
// root=0 < toplevel (按 toplevelZOrder_ 顺序) < subsurface (原顺序)。
// 阶段 3: zcActive 为 ZC 层状态单一字段 (合成/输入/遮挡重绘只认它)。
// sub/st 指针指向调用方持有的容器, 必须在 ToplevelManager 锁内使用。
struct CompositorLayer {
    enum class Type { Root, Toplevel, Subsurface };
    Type type = Type::Root;
    size_t zIndex = 0;
    bool visible = false;    // 可见性判定结果 (Root 恒 true, 不参与命中)
    // ZC 层状态单一字段: 该层走 GPU 内容 (合成/输入跳过, 内容由
    // egl_renderer GPU 层自绘); false = fallback 到 CPU 内容 (合成/
    // 命中照常)。由 ZcBridge::IsActive (zc_) 派生 — 该集合是 compositor
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

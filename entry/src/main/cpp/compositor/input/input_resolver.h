#pragma once
#include <wayland-server-core.h>
#include <cstdint>

class ToplevelManager;
class DesktopCompositor;

// 输入命中终态 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用)
//
// 裁决闭环 (重构第 4A 步): FindInputTargetAt 返回终态 — 调用方无需再懂
// "桌面坐标→surface 局部"的逆映射与内容区钳制, 直接取 localX/localY 注入
// (wl_fixed 转换在注入时做); 调用方只按 swallow 加工 (见下)。origin/scale
// 保留供诊断日志 (TARGET/SCROLL-TARGET 断点), 不再参与换算; 内容区尺寸
// (钳制上界 contentW/H) 是 ComputeLocalPoint 的内部管道参数, 无外部
// 消费者, 4C2 顺手项已从 InputTarget 删除 (局部变量传给纯函数)。
struct InputTarget {
    uint32_t toplevelId = 0;         // 事件归属 toplevel (raise/键盘焦点)
    wl_resource* surface = nullptr;  // pointer enter 目标
    // 终态: 可注入的 surface 局部坐标 (双精度)。由 resolver 在锁内完成
    // 逆映射 (ComputeLocalPoint): (桌面逻辑坐标 - origin) / scale, 已按
    // contentW/H 钳到内容区边缘
    double localX = 0.0, localY = 0.0;
    // true = 该点落在全屏黑边内: 调用方只吞 PRESS (防幻影点击/焦点切换);
    // MOVE/RELEASE 照常注入 local (已在 resolver 内钳到内容区边缘,
    // 吞掉会导致按键状态卡死)
    bool swallow = false;
    // 诊断/复核: 本命中使用的逆映射基 (local = (d - origin) / scale)。
    // 全屏窗口保比例缩放 (== FitRect off/scale), 普通窗口与 root 回退为恒等
    // (origin=0, scale=1)。精度与 geometry.h FitRect 对齐: origin 是整数
    // 屏幕原点 (int → double 无损), scale 是未取整 double (不再 float 截断)。
    // 仅输入日志 (TARGET/SCROLL-TARGET 断点 2/4B) 消费, 不参与换算。
    double originX = 0.0, originY = 0.0;
    double scale = 1.0;
};

// 输入命中裁决 (依赖 ToplevelManager + DesktopCompositor)
//
// 不变式: 命中是单一 Z 序逆序遍历 (从最高 zIndex 向下), 与渲染侧
// TakeToplevelFrame 单一合成循环同源 — 均遍历 BuildLayerListLocked 同一
// Layer 列表, 每层用其渲染时实际占屏区域做命中。全屏窗口作为普通层参与:
// 命中用 fit 变换后的屏幕几何 (内容区命中 / 黑边命中标 swallow), 更高
// zIndex 的窗口渲染在游戏上方, 逆序先到 → 命中优先 (游戏全屏时弹出的新
// 窗口点击必须路由给它)。连带 fullscreen 旧窗口 (渲染时被跳过) 命中也
// 跳过; zero-copy GL 层不参与置顶命中 (渲染时被遮挡重绘压回, 命中同样
// 下放给 z-order 循环)。
// 全屏黑边命中标 swallow, 调用方只吞 PRESS — MOVE/RELEASE 照常透传,
// 否则按下拖到黑边松手会丢 release, 按键状态永久卡死 (见实现注释)。
class InputResolver {
public:
    InputResolver(ToplevelManager& tmgr, DesktopCompositor& compositor,
                  const uint32_t& desktopRootToplevelId,
                  const int32_t& outputW, const int32_t& outputH);

    // Desktop 模式: 桌面逻辑坐标 (lx,ly) 处的精确输入目标, 一次性产出
    // 终态 (surface + 可注入局部坐标 localX/localY + swallow 动作指示)。
    // 命中判定用取整坐标 (lround), 逆映射用未取整 double — 与旧调用方
    // (lround 命中 / 原始 logicalX 换算) 逐点一致, 4A 收内时不合并两处精度。
    // 命中 subsurface 菜单层时返回层自己的 wl_surface + 层桌面原点 —
    // 菜单可伸出父窗口边界, 事件必须 enter 菜单 surface 并用菜单相对坐标,
    // 否则经父窗口 surface 的越界坐标会被 winewayland 的 motion clamp
    // (wayland_pointer.c "bring them within bounds") 夹回窗口内, 菜单收不到。
    // 未命中层时回退 toplevel / desktop root。返回 false = surface 不可用。
    bool FindInputTargetAt(double lx, double ly, InputTarget& out);

    // Desktop 模式: 在合成帧中查找包含 (x,y) 的 toplevel (用于输入路由)
    uint32_t FindToplevelAt(int x, int y);

    // surface 指针是否仍存活 (输入注入前的防御校验, 遍历 surfaceResources_)
    bool IsSurfaceAlive(wl_resource* surface);

    // surface 局部坐标 → 桌面坐标 (warp 锚点换算, OnPointerWarp 用)。
    // 全屏 toplevel 用与命中/渲染相同的 FitRect 正变换; 普通窗口 = 位置+局部。
    bool SurfaceLocalToDesktop(wl_resource* surface, double lx, double ly,
                               double& dx, double& dy);

private:
    // root 尺寸解析 (FindInputTargetAt / SurfaceLocalToDesktop 两处全屏 fit
    // 计算共用): root toplevel state 有尺寸用它的, 否则回退到 output 尺寸
    void ResolveRootSize(int& rootW, int& rootH) const;

    ToplevelManager& tmgr_;
    DesktopCompositor& compositor_;
    const uint32_t& desktopRootToplevelId_;
    const int32_t& outputW_;
    const int32_t& outputH_;
};

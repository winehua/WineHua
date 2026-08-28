#pragma once
#include <wayland-server-core.h>
#include <cstdint>

class ToplevelManager;
class DesktopCompositor;

// 输入命中目标 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用)
struct InputTarget {
    uint32_t toplevelId = 0;         // 事件归属 toplevel (raise/键盘焦点)
    wl_resource* surface = nullptr;  // pointer enter 目标
    int originX = 0, originY = 0;    // surface 的桌面原点 (输入坐标换算基)
    // 桌面坐标 → surface 局部坐标的缩放除数。
    // 全屏窗口保比例缩放显示, 局部坐标 = (桌面坐标 - origin) / scale; 普通窗口为 1
    float scale = 1.0f;
    // true = 该点落在全屏黑边内: 调用方只吞 PRESS (防幻影点击/焦点切换);
    // MOVE/RELEASE 照常按 origin/scale 透传给全屏窗口 (越界坐标由调用方
    // 用 contentW/contentH 钳到内容区边缘, 吞掉会导致按键状态卡死)
    bool swallow = false;
    // 全屏窗口的内容尺寸 (fit 变换 src 尺寸, 即表面局部坐标有效域);
    // 非全屏目标为 0 = 调用方不做内容区钳制
    int contentW = 0, contentH = 0;
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

    // Desktop 模式: (x,y) 处的精确输入目标。
    // 命中 subsurface 菜单层时返回层自己的 wl_surface + 层桌面原点 —
    // 菜单可伸出父窗口边界, 事件必须 enter 菜单 surface 并用菜单相对坐标,
    // 否则经父窗口 surface 的越界坐标会被 winewayland 的 motion clamp
    // (wayland_pointer.c "bring them within bounds") 夹回窗口内, 菜单收不到。
    // 未命中层时回退 toplevel / desktop root。返回 false = surface 不可用。
    bool FindInputTargetAt(int x, int y, InputTarget& out);

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

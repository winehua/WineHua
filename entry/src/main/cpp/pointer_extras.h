#pragma once

#include <wayland-server-core.h>
#include <mutex>
#include <vector>

/*
 * DirectInput 类老游戏 (PAL2 等) 依赖的指针扩展协议 compositor 端实现:
 *
 * - wp_pointer_warp_v1: SetCursorPos 映射。绝对模式游戏 (RTS 等) 稀疏
 *   调用, wineserver 光标已在 wine 侧移动到位, host 无需注入 motion。
 * - zwp_pointer_constraints_v1: lock/confine 对象承载。只注册全局 +
 *   应答 locked/confined 即可满足 wine 的约束状态机; 锁销毁时若游戏
 *   给过 cursor_position_hint, 按协议把逻辑指针移到 hint。
 * - zwp_relative_pointer_manager_v1: wine 相对模式的承载 (wayland_pointer.c
 *   needs_relative = !is_visible && constraint_hwnd == focused_hwnd —
 *   游戏隐藏系统光标且有约束 → 启用 zwp_relative_pointer_v1, 丢弃绝对
 *   motion, 光标位置 = 基线 + 增量累积, 经 NtUserSendHardwareInput 驱动
 *   wineserver 光标; 绝对读 (GetCursorPos) 与相对读 (dinput 差值) 都工作)。
 *   host 不做模式判断: 始终发绝对 motion, 同时把输入坐标差分出增量,
 *   有 relative 对象就发 relative_motion — 启用与否由 wine 按真实游戏
 *   行为 (光标可见性 + 约束) 决定。SetCursorPos 在相对模式下被 wine
 *   拒绝 (wayland_pointer.c:1024 返回 FALSE), 不发 warp 请求。
 *
 * confine 的坐标钳制不在 compositor 侧做: ClipCursor 在 wineserver 内
 * 同样钳住光标 (与驱动无关), 两侧钳制结果一致, 无需重复实现。
 *
 * 参照 weston pointer-constraints.c。
 */
class PointerExtras {
public:
    static PointerExtras* GetInstance();

    // 注册 constraints + warp + relative_pointer_manager global (见头注释)
    void Register(wl_display* display);

    enum class ConstraintType { None, Lock, Confine };

    // 某 surface 当前生效的约束 (无 = None)。
    // surface 已销毁的约束条目视为不存在 (惰性失效)
    ConstraintType ConstraintFor(wl_resource* surface);

    // 相对指针增量广播: wine 有 relative_pointer 对象时把输入增量发过去。
    // 对象存在 ⇔ wine 判定当前为相对模式 (隐藏光标 + 约束); 无对象 = 绝对
    // 模式, 此函数空转。Wayland 线程调用 (事件发送必须在该线程)。
    void SendRelativeMotion(double dx, double dy);
    bool HasRelativePointer() const;

    // -- 协议接口实现 (public: wl 接口表在类外初始化, 与 wayland_server.h 同例) --
    // zwp_pointer_constraints_v1
    static void constr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void constr_lock_pointer(wl_client*, wl_resource*, uint32_t id,
                                    wl_resource* surface, wl_resource* pointer,
                                    wl_resource* region, uint32_t lifetime);
    static void constr_confine_pointer(wl_client*, wl_resource*, uint32_t id,
                                       wl_resource* surface, wl_resource* pointer,
                                       wl_resource* region, uint32_t lifetime);
    static void locked_destroy(wl_client*, wl_resource* r);
    static void locked_set_cursor_position_hint(wl_client*, wl_resource* r,
                                                wl_fixed_t sx, wl_fixed_t sy);
    static void confined_destroy(wl_client*, wl_resource* r);
    static void confined_set_region(wl_client*, wl_resource*, wl_resource*) {}
    static void locked_set_region(wl_client*, wl_resource*, wl_resource*) {}
    // wp_pointer_warp_v1
    static void warp_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void warp_warp_pointer(wl_client*, wl_resource*, wl_resource* surface,
                                  wl_resource* pointer, wl_fixed_t x, wl_fixed_t y,
                                  uint32_t serial);
    // zwp_relative_pointer_manager_v1
    static void relmgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void relmgr_get_relative_pointer(wl_client*, wl_resource*, uint32_t id,
                                            wl_resource* pointer);

private:
    struct Constraint {
        ConstraintType type = ConstraintType::None;
        wl_resource* surface = nullptr;   // 约束目标 surface
        wl_resource* res = nullptr;       // locked/confined 对象
        bool hasHint = false;             // locked 的 cursor_position_hint
        double hintX = 0, hintY = 0;
    };

    // mutable: const 查询接口 (HasRelativePointer) 也要锁
    mutable std::mutex mutex_;
    std::vector<Constraint> constraints_;
    // 已创建的 zwp_relative_pointer_v1 对象 (wine 相对模式时存在, 至多一个:
    // wine 用 process_wayland.pointer.wl_pointer 固定绑定; 多对象时全部广播)
    std::vector<wl_resource*> relativePointers_;

    // 约束资源析构共通处理: 摘掉条目, 如有 hint 则把逻辑指针移到 hint
    static void OnConstraintResourceDestroyed(wl_resource* r);
    static void OnRelativePointerDestroyed(wl_resource* r);
    static void constraints_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void warp_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void relmgr_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
};

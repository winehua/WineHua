#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

struct wl_display;      // 只存指针 (事件循环给), 不操作 — 头不 include wayland
struct wl_event_source; // 同上 (wl_event_loop_add_fd 返回的不透明句柄)
struct wl_resource;     // 事件载荷里只存指针做身份比较

// ============================================================================
// InputQueue — 输入事件队列机制 (重构第 4C2 步从 InputManager 抽离)
//
// 职责: NAPI/JS 线程 → Wayland 线程的边车队列 —
//   - 队列存储 + mutex (queueMutex_)
//   - pipe 唤醒 (pipeRead_/pipeWrite_)
//   - wl_event_source 注册 (pipeSource_, wl_display_get_event_loop 的事件循环)
//   - Poll: 取批 + 去重 (原 InputManager::FlushQueue 前半 — 去重是"批处理
//     机制"而非注入语义: 只比较事件序列的 type/tl/surface/坐标, 不依赖任何
//     wayland 资源或 client 状态, 与取批同属队列边界, 故不出队到 Injector)
//   - FlushClients: wl_display_flush_clients (原 FlushQueue 尾部)
//
// 线程模型 (与旧 InputManager 逐字对应):
//   - NAPI/JS 线程 (Send*Event): Enqueue* → 锁内 push, 锁外 pipe write
//   - Wayland 线程: OnPipeReadable (wl_event_source 回调) → 编排层 flush
//     回调 → Poll (锁内 swap) + 去重 → dispatch; 加锁边界与旧实现逐字 —
//     队列操作绝不移出锁外 (红线)。
//
// 去重规则 (逐字平移, 语义与旧一致):
//   - PTR_MOTION / REL_MOTION: 同类型连续事件只保留最后一个 (绝对位置/增量)
//   - PTR_BUTTON/KBD_KEY: 同键同状态连续事件合并
//   - PTR_ENTER: 同 tl+surface 连续事件合并
//   - PTR_AXIS: 不去重 — 每个值是累积滚动量, 丢中间值 = 丢滚动量
//   - PTR_LEAVE/KBD_ENTER/KBD_LEAVE/KBD_MODIFIERS: 不去重
// ============================================================================

class InputQueue {
public:
    // 事件载荷 — x/y/axis_value 以 int32_t 承载 wl_fixed_t 值 (wl_fixed_t
    // 是 int32_t 的 typedef, 类型等价; 头不 include wayland, 宿主可直连编译)
    struct Event {
        enum Type { PTR_ENTER, PTR_LEAVE, PTR_MOTION, PTR_BUTTON, PTR_AXIS,
                    REL_MOTION,
                    KBD_ENTER, KBD_LEAVE, KBD_KEY, KBD_MODIFIERS } type;
        uint32_t tl = 0;
        wl_resource* surface = nullptr;
        int32_t x = 0, y = 0;
        uint32_t btn_or_key = 0;
        uint32_t state = 0;
        // axis fields
        int axis = 0;           // 0=vertical, 1=horizontal
        int32_t axis_value = 0;
        // modifiers fields
        uint32_t mod_depressed = 0, mod_latched = 0, mod_locked = 0, mod_group = 0;
    };

    // 生命周期 (WaylandServer 启动/停止时由 InputManager 委托调用) —
    // display = WaylandServer 的事件循环 display (peek: wl_display_create
    // → wl_display_get_event_loop), pipe 注册在该事件循环上
    void Initialize(wl_display* display, std::function<void()> onFlush);
    void Shutdown();

    // -- NAPI/JS 线程入队 (锁内 push, 锁外 pipe 唤醒; 与旧 Enqueue 逐字) --
    void Enqueue(Event::Type type, uint32_t tl, wl_resource* surface,
                 int32_t x, int32_t y, uint32_t btn_or_key, uint32_t state);
    // 修饰键快照入队 (编排层从 InputStateTracker 取当前值 — 事件携带快照,
    // 消费侧不读实时状态, 与旧 EnqueueModifiers 同序)
    void EnqueueModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    // axis 入队 (原 SendScrollEvent 尾部手写 push — tl 是诊断字段, 消费侧
    // InjectPointerAxis 不读: 广播到全部 pointer 资源, wine 按 per-process
    // focused_hwnd 消化 axis, 见 4B 台账"注入端与 pointer 资源结论")
    void EnqueueAxis(int axis, int32_t axis_value, uint32_t tl);

    // -- Wayland 线程 --
    // 取批 + 去重 (原 FlushQueue 前半; 批换出后队列可立即接纳新事件)
    std::vector<Event> Poll();
    // wl_display_flush_clients (原 FlushQueue 尾部; display 未就绪时空转)
    void FlushClients();

    // pipe 回调 (wl_event_loop_add_fd 的 static 接口) — 读空 fd 后调编排层
    // flush 回调 (回调在事件循环启动前一次性注入, 与 4C1 warpSink 同模式)
    static int OnPipeReadable(int fd, uint32_t mask, void* data);

private:
    void WakePipe();

    std::mutex queueMutex_;
    std::vector<Event> queue_;
    int pipeRead_ = -1, pipeWrite_ = -1;
    struct wl_event_source* pipeSource_ = nullptr;
    std::function<void()> flushCallback_;  // 仅 Wayland 线程读
    wl_display* display_ = nullptr;
};

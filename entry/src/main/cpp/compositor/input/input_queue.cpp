#include "compositor/input/input_queue.h"

#include <wayland-server-core.h>  // wl_display_get_event_loop / wl_event_loop_add_fd ...
#include <utility>  // std::move
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// ============================================================================
//  生命周期 — 原 InputManager::Initialize/Shutdown 的队列段逐字搬移
// ============================================================================

void InputQueue::Initialize(wl_display* display, std::function<void()> onFlush) {
    if (pipeRead_ >= 0) {
        OH_LOG_WARN(LOG_APP, "[Input] already initialized");
        return;
    }
    display_ = display;
    flushCallback_ = std::move(onFlush);

    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Input] pipe2 failed errno=%{public}d", errno);
        return;
    }
    pipeRead_  = fds[0];
    pipeWrite_ = fds[1];

    struct wl_event_loop* loop = wl_display_get_event_loop(display);
    pipeSource_ = wl_event_loop_add_fd(loop, pipeRead_, WL_EVENT_READABLE, OnPipeReadable, this);
    if (!pipeSource_) {
        OH_LOG_ERROR(LOG_APP, "[Input] wl_event_loop_add_fd failed");
        close(pipeRead_); close(pipeWrite_);
        pipeRead_ = pipeWrite_ = -1;
        return;
    }
    OH_LOG_INFO(LOG_APP, "[Input] initialized OK (pipe r=%{public}d w=%{public}d)", pipeRead_, pipeWrite_);
}

void InputQueue::Shutdown() {
    if (pipeSource_) {
        wl_event_source_remove(pipeSource_);
        pipeSource_ = nullptr;
    }
    if (pipeRead_ >= 0)  { close(pipeRead_);  pipeRead_  = -1; }
    if (pipeWrite_ >= 0) { close(pipeWrite_); pipeWrite_ = -1; }
    display_ = nullptr;
}

// ============================================================================
//  入队 (JS 线程) — 原 InputManager::Enqueue/EnqueueModifiers + SendScrollEvent
//  尾部 axis 手写段逐字搬移 (锁内 push, 锁外唤醒 — 锁边界一字未动)
// ============================================================================

void InputQueue::Enqueue(Event::Type type, uint32_t tl, wl_resource* surface,
                         int32_t x, int32_t y, uint32_t btn_or_key, uint32_t state) {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back({type, tl, surface, x, y, btn_or_key, state});
    }
    WakePipe();
}

void InputQueue::EnqueueModifiers(uint32_t depressed, uint32_t latched,
                                  uint32_t locked, uint32_t group) {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back({Event::KBD_MODIFIERS, 0, nullptr, 0, 0, 0, 0,
                          0, 0, depressed, latched, locked, group});
    }
    WakePipe();
}

void InputQueue::EnqueueAxis(int axis, int32_t axis_value, uint32_t tl) {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        // 诊断字段: InjectPointerAxis 与 motion/button 一致广播到全部 pointer
        // 资源, 消费侧不读 tl (wine 按 per-process focused_hwnd 消化 axis,
        // 谁收到 enter 谁响应 — 见 4B 台账"注入端与 pointer 资源结论")
        queue_.push_back({Event::PTR_AXIS, tl, nullptr, 0, 0, 0, 0,
                          axis, axis_value, 0, 0, 0, 0});
    }
    WakePipe();
}

void InputQueue::WakePipe() {
    // 唤醒 Wayland 线程 (原 Enqueue 尾部逐字)
    if (pipeWrite_ >= 0) {
        char c = 1;
        ssize_t n = write(pipeWrite_, &c, 1);
        if (n < 0 && errno != EAGAIN) {
            OH_LOG_WARN(LOG_APP, "[Input] pipe write FAIL errno=%{public}d", errno);
        }
    }
}

// ============================================================================
//  Poll: 取批 + 去重 (原 InputManager::FlushQueue 前半逐字搬移)
//  去重归本层 (而非 Injector): 去重只比对事件序列自身的 type/tl/surface/
//  坐标 — 不读任何 wayland 资源/客户端状态, 与"取批"同属队列批量语义;
//  Injector 保持"每事件一注入"的纯粹协议层 (见 input_queue.h 注释)。
// ============================================================================

std::vector<InputQueue::Event> InputQueue::Poll() {
    // Wayland 线程: 取出所有事件并发送
    std::vector<Event> batch;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        batch.swap(queue_);
    }
    if (batch.empty()) return batch;

    // 去重
    std::vector<Event> merged;
    for (auto& ev : batch) {
        if (!merged.empty()) {
            auto& last = merged.back();
            if (last.type == ev.type) {
                bool skip = false;
                switch (ev.type) {
                    case Event::PTR_BUTTON:
                        skip = (last.btn_or_key == ev.btn_or_key && last.state == ev.state);
                        break;
                    case Event::PTR_MOTION:
                        last = ev; continue;  // 只保留最后一个坐标 (绝对位置)
                    // PTR_AXIS 不去重: 每个值是累积滚动距离, 丢中间值 = 丢滚动量
                    // 快速滚轮/触控板会产生连续 axis 事件, 必须全部送达 Wine
                    case Event::PTR_ENTER:
                        skip = (last.tl == ev.tl && last.surface == ev.surface);
                        break;
                    case Event::KBD_KEY:
                        skip = (last.btn_or_key == ev.btn_or_key && last.state == ev.state);
                        break;
                    default: break;
                }
                if (skip) continue;
            }
        }
        merged.push_back(ev);
    }
    if (merged.size() != batch.size()) {
        OH_LOG_INFO(LOG_APP, "[Input] dedup %{public}zu→%{public}zu", batch.size(), merged.size());
    }
    return merged;
}

void InputQueue::FlushClients() {
    if (display_) {
        wl_display_flush_clients(display_);
    }
}

// ============================================================================
//  pipe 回调 (Wayland 线程) — 读空 fd 后调编排层 flush (原
//  static_cast<InputManager*>(data)->FlushQueue() 的注入式替代)
// ============================================================================

int InputQueue::OnPipeReadable(int fd, uint32_t mask, void* data) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
    auto* q = static_cast<InputQueue*>(data);
    if (q->flushCallback_) q->flushCallback_();
    return 0;
}

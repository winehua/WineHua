#include "input/text_input.h"

#include "protocols/text-input-unstable-v3-server-protocol.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_TextInput"
#include <hilog/log.h>

extern "C" void winehua_display_focus_select(uint32_t, bool) __attribute__((weak));

TextInput* TextInput::GetInstance() {
    static TextInput s;
    return &s;
}

void TextInput::Register(wl_display* display) {
    std::lock_guard<std::mutex> stateLock(mutex_);
    std::lock_guard<std::mutex> queueLock(opMutex_);
    if (global_) return;
    if (!display) return;
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        OH_LOG_ERROR(LOG_APP, "[TextInput] pipe creation failed");
        return;
    }
    wl_event_source* source = wl_event_loop_add_fd(wl_display_get_event_loop(display),
        fds[0], WL_EVENT_READABLE, OnPipeReadable, this);
    wl_global* global = source ? wl_global_create(display,
        &zwp_text_input_manager_v3_interface, 1, this, manager_bind) : nullptr;
    if (!global) {
        if (source) wl_event_source_remove(source);
        close(fds[0]);
        close(fds[1]);
        OH_LOG_ERROR(LOG_APP, "[TextInput] registration failed");
        return;
    }
    display_ = display;
    global_ = global;
    pipeSource_ = source;
    pipeReadFd_ = fds[0];
    pipeWriteFd_ = fds[1];
    armed_ = requestedArmed_ = false;
    OH_LOG_INFO(LOG_APP, "[TextInput] manager registered armed=%{public}d",
                armed_ ? 1 : 0);
}

void TextInput::Shutdown() {
    std::vector<Entry> retired;
    wl_global* global = nullptr;
    bool wasActivated = false;
    {
        std::lock_guard<std::mutex> stateLock(mutex_);
        std::lock_guard<std::mutex> queueLock(opMutex_);
        // Dispatch has stopped. Holding both locks also prevents a NAPI writer
        // from targeting a retired resource or a closed/reused pipe descriptor.
        if (pipeSource_) wl_event_source_remove(pipeSource_);
        pipeSource_ = nullptr;
        if (pipeReadFd_ >= 0) close(pipeReadFd_);
        if (pipeWriteFd_ >= 0) close(pipeWriteFd_);
        pipeReadFd_ = pipeWriteFd_ = -1;
        opQueue_.clear();
        retired.swap(entries_);
        surfaceRects_.clear();
        for (const Entry& entry : retired) wasActivated |= entry.activated;
        global = global_;
        global_ = nullptr;
        display_ = nullptr;
        focusedToplevel_ = 0;
        focusedSurface_ = nullptr;
        armed_ = requestedArmed_ = false;
    }
    // Resource destructors re-enter mutex_; do not destroy under either lock.
    for (const Entry& entry : retired) wl_resource_destroy(entry.res);
    if (global) wl_global_destroy(global);
    if (wasActivated) NotifyActivated(false);
    OH_LOG_INFO(LOG_APP, "[TextInput] manager shutdown resources=%{public}zu", retired.size());
}

static const struct zwp_text_input_manager_v3_interface kManagerImpl = {
    .destroy = TextInput::manager_destroy,
    .get_text_input = TextInput::manager_get_text_input,
};

static const struct zwp_text_input_v3_interface kTextInputImpl = {
    .destroy = TextInput::ti_destroy,
    .enable = TextInput::ti_enable,
    .disable = TextInput::ti_disable,
    .set_surrounding_text = TextInput::ti_set_surrounding_text,
    .set_text_change_cause = TextInput::ti_set_text_change_cause,
    .set_content_type = TextInput::ti_set_content_type,
    .set_cursor_rectangle = TextInput::ti_set_cursor_rectangle,
    .commit = TextInput::ti_commit,
};

void TextInput::manager_bind(wl_client* client, void* data,
                                    uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_text_input_manager_v3_interface,
                                          std::min(version, 1u), id);
    wl_resource_set_implementation(res, &kManagerImpl, data, nullptr);
}

void TextInput::manager_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

void TextInput::manager_get_text_input(wl_client*, wl_resource* manager,
                                              uint32_t id, wl_resource*) {
    auto* self = GetInstance();
    wl_client* client = wl_resource_get_client(manager);
    wl_resource* res = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
    wl_resource_set_implementation(res, &kTextInputImpl, nullptr, resource_destroyed);

    wl_resource* enterSurface = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->entries_.push_back({});
        self->entries_.back().res = res;
        self->entries_.back().epoch = ++self->nextEpoch_;
        // 输入对象晚于键盘焦点创建时立即补发 enter, 不丢协议激活。
        if (self->armed_ && self->focusedSurface_ &&
            wl_resource_get_client(res) == wl_resource_get_client(self->focusedSurface_)) {
            self->entries_.back().enteredSurface = self->focusedSurface_;
            enterSurface = self->focusedSurface_;
        }
    }
    OH_LOG_INFO(LOG_APP, "[TextInput] object created client=%{public}p res=%{public}p entries=%{public}d pendingEnter=%{public}d",
                client, res, (int)self->entries_.size(), enterSurface ? 1 : 0);
    if (enterSurface) zwp_text_input_v3_send_enter(res, enterSurface);
}

void TextInput::resource_destroyed(wl_resource* r) {
    auto* self = GetInstance();
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        for (const auto& entry : self->entries_) if (entry.res == r) active |= entry.activated;
        self->entries_.erase(std::remove_if(self->entries_.begin(), self->entries_.end(),
            [&](const Entry& entry) { return entry.res == r; }), self->entries_.end());
        std::lock_guard<std::mutex> queueLock(self->opMutex_);
        self->opQueue_.erase(std::remove_if(self->opQueue_.begin(), self->opQueue_.end(),
            [&](const Op& op) { return op.res == r; }), self->opQueue_.end());
    }
    if (active) self->NotifyActivated(false);
}

void TextInput::ti_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

void TextInput::ti_enable(wl_client*, wl_resource* r) {
    auto* self = GetInstance();
    wl_resource* entered = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        for (Entry& entry : self->entries_) {
            if (entry.res != r) continue;
            entry.enabled = true;
            entered = entry.enteredSurface;
            break;
        }
    }
    OH_LOG_INFO(LOG_APP, "[TextInput] enable res=%{public}p surface=%{public}p client=%{public}p",
                r, entered, wl_resource_get_client(r));
}

void TextInput::ti_disable(wl_client*, wl_resource* r) {
    auto* self = GetInstance();
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        for (Entry& entry : self->entries_) {
            if (entry.res != r) continue;
            active = entry.activated;
            entry.enabled = entry.activated = false;
            entry.cursorX = entry.cursorY = entry.cursorW = entry.cursorH = 0;
            entry.epoch = ++self->nextEpoch_;
            break;
        }
    }
    if (active) self->NotifyActivated(false);
}

void TextInput::ti_set_surrounding_text(wl_client*, wl_resource*,
                                               const char*, int32_t, int32_t) {}
void TextInput::ti_set_text_change_cause(wl_client*, wl_resource*, uint32_t) {}
void TextInput::ti_set_content_type(wl_client*, wl_resource*, uint32_t, uint32_t) {}

void TextInput::ti_set_cursor_rectangle(wl_client*, wl_resource* r,
                                               int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* self = GetInstance();
    bool activated = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        for (Entry& entry : self->entries_) {
            if (entry.res != r) continue;
            entry.cursorX = x;
            entry.cursorY = y;
            entry.cursorW = w;
            entry.cursorH = h;
            if (entry.enteredSurface && w > 0 && h > 0)
                self->surfaceRects_[entry.enteredSurface] = {x, y, w, h};
            // 文本框聚焦后 Wine 调 SetIMECompositionRect → 非零矩形。
            // enter 时 Wine 发 0,0,0,0 (占位); 非零即真文本框, 立即激活。
            if (self->armed_ && entry.enteredSurface && entry.enabled && w > 0 && h > 0 && !entry.activated) {
                entry.activated = true;
                activated = true;
            }
            break;
        }
    }
    if (activated) {
        OH_LOG_INFO(LOG_APP, "[TextInput] ACTIVATE rect=%{public}d,%{public}d %{public}dx%{public}d",
                    x, y, w, h);
        self->NotifyActivated(true);
    }
}

void TextInput::ti_commit(wl_client*, wl_resource* r) {
    auto* self = GetInstance();
    bool activated = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        for (Entry& entry : self->entries_) {
            if (entry.res == r) {
                entry.commitCount++;
                if (entry.enteredSurface && (entry.cursorW <= 0 || entry.cursorH <= 0)) {
                    const auto memo = self->surfaceRects_.find(entry.enteredSurface);
                    if (memo != self->surfaceRects_.end()) {
                        entry.cursorX = memo->second.x; entry.cursorY = memo->second.y;
                        entry.cursorW = memo->second.w; entry.cursorH = memo->second.h;
                    }
                }
                // 激活判定 (对齐上游): enabled + 非零光标矩形。
                if (self->armed_ && entry.enteredSurface && entry.enabled && entry.cursorW > 0 && entry.cursorH > 0 &&
                    !entry.activated) {
                    entry.activated = true;
                    activated = true;
                }
                break;
            }
        }
    }
    if (activated) {
        OH_LOG_INFO(LOG_APP, "[TextInput] ACTIVATE (commit)");
        self->NotifyActivated(true);
    }
}

void TextInput::OnKeyboardEnter(uint32_t toplevelId, wl_resource* surface) {
    if (!surface) return;

    struct SendAction {
        bool enter;
        wl_resource* res;
        wl_resource* surface;
    };
    std::vector<SendAction> actions;
    bool wasActivated = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (focusedSurface_ == surface) {
            OH_LOG_INFO(LOG_APP, "[TextInput] focus enter dup tl=%{public}u surface=%{public}p",
                        toplevelId, surface);
            return;
        }
        if (winehua_display_focus_select) winehua_display_focus_select(toplevelId, false);
        focusedToplevel_ = toplevelId;
        focusedSurface_ = surface;
        wl_client* client = wl_resource_get_client(surface);

        // 先把已经 enter 到其他 surface 的对象 leave 掉 (同 client 也要重进)。
        for (Entry& entry : entries_) {
            if (entry.enteredSurface && entry.enteredSurface != surface) {
                actions.push_back({false, entry.res, entry.enteredSurface});
                entry.enteredSurface = nullptr;
                wasActivated |= entry.activated;
                entry.activated = false;
                entry.cursorX = entry.cursorY = entry.cursorW = entry.cursorH = 0;
                entry.enabled = false;
                entry.epoch = ++nextEpoch_;
            }
        }
        if (armed_) {
            for (Entry& entry : entries_) {
                if (!entry.res || wl_resource_get_client(entry.res) != client) continue;
                if (entry.enteredSurface == surface) continue;
                entry.enteredSurface = surface;
                actions.push_back({true, entry.res, surface});
            }
        }
    }

    if (wasActivated) NotifyActivated(false);
    for (const SendAction& action : actions) {
        if (action.enter) {
            OH_LOG_INFO(LOG_APP, "[TextInput] send enter res=%{public}p surface=%{public}p surfClient=%{public}p",
                        action.res, action.surface, wl_resource_get_client(action.surface));
            zwp_text_input_v3_send_enter(action.res, action.surface);
        } else {
            OH_LOG_INFO(LOG_APP, "[TextInput] send leave res=%{public}p surface=%{public}p",
                        action.res, action.surface);
            zwp_text_input_v3_send_leave(action.res, action.surface);
        }
    }
    OH_LOG_INFO(LOG_APP, "[TextInput] focus enter tl=%{public}u surface=%{public}p armed=%{public}d actions=%{public}d",
                toplevelId, surface, armed_ ? 1 : 0, (int)actions.size());
}

void TextInput::OnKeyboardLeave() {
    std::vector<std::pair<wl_resource*, wl_resource*>> actions;
    bool wasActivated = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        focusedToplevel_ = 0;
        focusedSurface_ = nullptr;
        for (const Entry& entry : entries_) wasActivated |= entry.activated;
        LeaveAllEnteredLocked(actions);
        for (Entry& entry : entries_) {
            if (entry.activated) {
                entry.activated = false;
                wasActivated = true;
            }
        }
    }
    for (const auto& action : actions) {
        OH_LOG_INFO(LOG_APP, "[TextInput] send leave res=%{public}p surface=%{public}p",
                    action.first, action.second);
        zwp_text_input_v3_send_leave(action.first, action.second);
    }
    if (wasActivated) NotifyActivated(false);
    if (!actions.empty()) {
        OH_LOG_INFO(LOG_APP, "[TextInput] focus leave actions=%{public}d", (int)actions.size());
    }
}

void TextInput::LeaveAllEnteredLocked(
    std::vector<std::pair<wl_resource*, wl_resource*>>& actions) {
    for (Entry& entry : entries_) {
        if (!entry.enteredSurface) continue;
        actions.emplace_back(entry.res, entry.enteredSurface);
        entry.enteredSurface = nullptr;
        entry.enabled = entry.activated = false;
        entry.cursorX = entry.cursorY = entry.cursorW = entry.cursorH = 0;
        entry.epoch = ++nextEpoch_;
    }
}

void TextInput::OnSurfaceDestroyed(wl_resource* surface) {
    bool wasActivated = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        surfaceRects_.erase(surface);
        if (focusedSurface_ == surface) {
            OH_LOG_INFO(LOG_APP, "[TextInput] focus surface destroyed surface=%{public}p tl=%{public}u",
                        surface, focusedToplevel_);
            focusedSurface_ = nullptr;
            focusedToplevel_ = 0;
        }
        for (Entry& entry : entries_) {
            if (entry.enteredSurface == surface) {
                entry.enteredSurface = nullptr;
                entry.enabled = false;
                entry.epoch = ++nextEpoch_;
                if (entry.activated) {
                    entry.activated = false;
                    wasActivated = true;
                }
            }
        }
    }
    if (wasActivated) NotifyActivated(false);
}

void TextInput::SetActivateCallback(ActivateCb cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    activateCb_ = std::move(cb);
}

void TextInput::NotifyActivated(bool active) {
    ActivateCb cb;
    int x = 0, y = 0, w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = activateCb_;
        // 回调携带当前 focused entry 的光标矩形 (ArkTS 定位软键盘/输入框)。
        for (Entry& entry : entries_) {
            if (entry.enteredSurface && entry.enabled) {
                x = entry.cursorX;
                y = entry.cursorY;
                w = entry.cursorW;
                h = entry.cursorH;
                break;
            }
        }
    }
    if (cb) {
        cb(active, x, y, w, h);
    }
}

uint32_t TextInput::FocusedToplevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return focusedToplevel_;
}

bool TextInput::IsActivated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_ || !focusedSurface_) return false;
    for (const Entry& entry : entries_) {
        if (entry.activated && entry.enabled && entry.enteredSurface == focusedSurface_) return true;
    }
    return false;
}

bool TextInput::IsEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_) return false;
    for (const Entry& entry : entries_) {
        if (entry.enabled && entry.enteredSurface) return true;
    }
    return false;
}

bool TextInput::EnqueueOpLocked(Op op, bool appendDone) {
    std::lock_guard<std::mutex> lock(opMutex_);
    if (pipeWriteFd_ < 0) return false;
    // Visibility control must not be dropped behind a full text queue. A new
    // arm boundary cancels text accepted for the previous keyboard session.
    if (op.type == OpType::SetArmed) opQueue_.clear();
    if (opQueue_.size() + (appendDone ? 2 : 1) > 256) return false;
    wl_resource* res = op.res;
    const uint64_t epoch = op.epoch;
    opQueue_.push_back(std::move(op));
    if (appendDone) {
        Op done;
        done.type = OpType::Done;
        done.res = res;
        done.epoch = epoch;
        opQueue_.push_back(std::move(done));
    }
    char byte = 1;
    ssize_t n;
    do { n = write(pipeWriteFd_, &byte, 1); } while (n < 0 && errno == EINTR);
    if (n == 1 || (n < 0 && errno == EAGAIN)) return true;
    opQueue_.resize(opQueue_.size() - (appendDone ? 2 : 1));
    return false;
}

int TextInput::OnPipeReadable(int fd, uint32_t, void* data) {
    char buffer[64];
    while (read(fd, buffer, sizeof(buffer)) > 0) {}
    static_cast<TextInput*>(data)->FlushOps();
    return 0;
}

TextInput::Entry* TextInput::EnabledEntryLocked() {
    if (!armed_) return nullptr;
    for (Entry& entry : entries_) {
        if (entry.enabled && entry.res && entry.enteredSurface) return &entry;
    }
    return nullptr;
}

bool TextInput::SendPreedit(const char* utf8, int32_t cursorBegin, int32_t cursorEnd) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = EnabledEntryLocked();
    if (!entry) return false;
    Op op;
    op.type = OpType::Preedit;
    op.res = entry->res;
    op.epoch = entry->epoch;
    op.text = utf8 ? utf8 : "";
    op.begin = cursorBegin;
    op.end = cursorEnd;
    return EnqueueOpLocked(std::move(op), true);
}

bool TextInput::SendCommit(const char* utf8) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = EnabledEntryLocked();
    if (!entry) return false;
    Op op;
    op.type = OpType::Commit;
    op.res = entry->res;
    op.epoch = entry->epoch;
    op.text = utf8 ? utf8 : "";
    return EnqueueOpLocked(std::move(op), true);
}

bool TextInput::SendDeleteSurrounding(uint32_t before, uint32_t after) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = EnabledEntryLocked();
    if (!entry) return false;
    Op op;
    op.type = OpType::DeleteSurrounding;
    op.res = entry->res;
    op.epoch = entry->epoch;
    op.begin = (int32_t)before;
    op.end = (int32_t)after;
    return EnqueueOpLocked(std::move(op), true);
}

void TextInput::SetArmed(bool armed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed == requestedArmed_) return;
    Op op;
    op.type = OpType::SetArmed;
    op.armed = armed;
    if (EnqueueOpLocked(std::move(op))) requestedArmed_ = armed;
}

void TextInput::RearmActivation() {
    std::lock_guard<std::mutex> lock(mutex_);
    Op op; op.type = OpType::Rearm;
    EnqueueOpLocked(std::move(op));
}

void TextInput::FlushOps() {
    std::vector<Op> batch;
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        batch.swap(opQueue_);
    }
    if (batch.empty()) return;

    for (const Op& op : batch) {
        switch (op.type) {
        case OpType::Rearm: {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& entry : entries_) entry.activated = false;
            break;
        }
        case OpType::SetArmed: {
            struct SendAction {
                bool enter;
                wl_resource* res;
                wl_resource* surface;
            };
            std::vector<SendAction> actions;
            bool wasActivated = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                armed_ = op.armed;
                if (!armed_) {
                    // 收键盘: 结束所有已 enter 的会话, 但保留焦点镜像,
                    // 重新打开时可立即补 enter。
                    std::vector<std::pair<wl_resource*, wl_resource*>> leaves;
                    for (const Entry& entry : entries_) wasActivated |= entry.activated;
                    LeaveAllEnteredLocked(leaves);
                    for (const auto& leave : leaves) {
                        actions.push_back({false, leave.first, leave.second});
                    }
                } else if (focusedSurface_) {
                    wl_client* client = wl_resource_get_client(focusedSurface_);
                    for (Entry& entry : entries_) {
                        if (!entry.res || wl_resource_get_client(entry.res) != client) continue;
                        if (entry.enteredSurface == focusedSurface_) continue;
                        entry.enteredSurface = focusedSurface_;
                        actions.push_back({true, entry.res, focusedSurface_});
                    }
                }
            }
            if (wasActivated) NotifyActivated(false);
            for (const SendAction& action : actions) {
                if (action.enter) {
                    OH_LOG_INFO(LOG_APP, "[TextInput] arm on, send enter res=%{public}p surface=%{public}p",
                                action.res, action.surface);
                    zwp_text_input_v3_send_enter(action.res, action.surface);
                } else {
                    OH_LOG_INFO(LOG_APP, "[TextInput] arm off, send leave res=%{public}p surface=%{public}p",
                                action.res, action.surface);
                    zwp_text_input_v3_send_leave(action.res, action.surface);
                }
            }
            OH_LOG_INFO(LOG_APP, "[TextInput] armed=%{public}d actions=%{public}d",
                        armed_ ? 1 : 0, (int)actions.size());
            break;
        }
        case OpType::Preedit: {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Entry& entry : entries_) {
                    if (entry.res == op.res && entry.epoch == op.epoch) {
                        ok = entry.enabled && entry.enteredSurface;
                        break;
                    }
                }
            }
            if (ok) {
                zwp_text_input_v3_send_preedit_string(op.res, op.text.c_str(), op.begin, op.end);
            }
            break;
        }
        case OpType::Commit: {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Entry& entry : entries_) {
                    if (entry.res == op.res && entry.epoch == op.epoch) {
                        ok = entry.enabled && entry.enteredSurface;
                        break;
                    }
                }
            }
            if (ok) {
                zwp_text_input_v3_send_commit_string(op.res, op.text.c_str());
            }
            break;
        }
        case OpType::Done: {
            uint32_t serial = 0;
            bool alive = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Entry& entry : entries_) {
                    if (entry.res == op.res && entry.epoch == op.epoch) {
                        serial = entry.commitCount;
                        alive = entry.enabled && entry.enteredSurface;
                        break;
                    }
                }
            }
            if (alive) zwp_text_input_v3_send_done(op.res, serial);
            break;
        }
        case OpType::DeleteSurrounding: {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Entry& entry : entries_) {
                    if (entry.res == op.res && entry.epoch == op.epoch) {
                        ok = entry.enabled && entry.enteredSurface;
                        break;
                    }
                }
            }
            if (ok) {
                zwp_text_input_v3_send_delete_surrounding_text(op.res,
                    (uint32_t)op.begin, (uint32_t)op.end);
            }
            break;
        }
        }
    }
    if (display_) wl_display_flush_clients(display_);
}

void TextInput::ResetKeyboardFocus() {
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        focusedSurface_ = nullptr;
        focusedToplevel_ = 0;
        for (auto& entry : entries_) {
            active |= entry.activated;
            entry.enteredSurface = nullptr;
            entry.enabled = entry.activated = false;
            entry.cursorX = entry.cursorY = entry.cursorW = entry.cursorH = 0;
            entry.epoch = ++nextEpoch_;
        }
    }
    if (active) NotifyActivated(false);
}

void TextInput::NotifyExternalActivated(bool active, int x, int y, int w, int h) {
    ActivateCb cb;
    { std::lock_guard<std::mutex> lock(mutex_); cb = activateCb_; }
    if (cb) cb(active, x, y, w, h);
}

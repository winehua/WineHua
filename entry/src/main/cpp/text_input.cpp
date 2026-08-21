#include "text_input.h"
#include "text-input-unstable-v3-server-protocol.h"
#include <algorithm>
#include <cstring>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_TextInput"
#include <hilog/log.h>

// -- zwp_text_input_v3 接口实现表 (request 顺序见协议 xml) --
static const struct zwp_text_input_v3_interface kTextInputImpl = {
    .destroy               = TextInput::text_input_destroy,
    .enable                = TextInput::text_input_enable,
    .disable               = TextInput::text_input_disable,
    .set_surrounding_text  = TextInput::text_input_set_surrounding_text,
    .set_text_change_cause = TextInput::text_input_set_text_change_cause,
    .set_content_type      = TextInput::text_input_set_content_type,
    .set_cursor_rectangle  = TextInput::text_input_set_cursor_rectangle,
    .commit                = TextInput::text_input_commit,
};

static const struct zwp_text_input_manager_v3_interface kManagerImpl = {
    .destroy        = TextInput::manager_destroy,
    .get_text_input = TextInput::manager_get_text_input,
};

TextInput* TextInput::GetInstance() {
    static TextInput s;
    return &s;
}

void TextInput::Register(wl_display* display) {
    if (global_) {
        OH_LOG_WARN(LOG_APP, "[TextInput] already registered");
        return;
    }
    display_ = display;
    global_ = wl_global_create(display, &zwp_text_input_manager_v3_interface, 1, this, manager_bind);
    OH_LOG_INFO(LOG_APP, "[TextInput] zwp_text_input_manager_v3 global registered OK");
}

void TextInput::Unregister() {
    if (global_) {
        wl_global_destroy(global_);
        global_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    // 全量清空: res 指向已销毁资源, 残留会导致下次会话 OnKeyboardEnter
    // 往死资源发 enter (日志假成功, Wine 收不到) + Register 被 global_ 残留跳过
    state_.res = nullptr;
    state_.surface = nullptr;
    state_.enabled = false;
    state_.activated = false;
    state_.cursorX = state_.cursorY = 0;
    state_.cursorW = state_.cursorH = 0;
    textInputs_.clear();
    display_ = nullptr;
    OH_LOG_INFO(LOG_APP, "[TextInput] unregistered OK");
}

// ========================================================================
//  global bind + get_text_input
// ========================================================================

void TextInput::manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<TextInput*>(data);
    uint32_t v = std::min(version, 1u);
    wl_resource* res = wl_resource_create(client, &zwp_text_input_manager_v3_interface, v, id);
    wl_resource_set_implementation(res, &kManagerImpl, self, nullptr);
    OH_LOG_INFO(LOG_APP, "[TextInput] client bound manager v=%{public}u OK", v);
}

void TextInput::manager_destroy(wl_client*, wl_resource* res) {
    wl_resource_destroy(res);
}

void TextInput::manager_get_text_input(wl_client* client, wl_resource* res, uint32_t id, wl_resource* seat) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    uint32_t version = wl_resource_get_version(res);

    wl_resource* ti = wl_resource_create(client, &zwp_text_input_v3_interface, version, id);
    wl_resource_set_implementation(ti, &kTextInputImpl, self, nullptr);

    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        // 每个 client 独立对象: 多个 Wine 进程各自绑定, 路由到对应对象
        self->textInputs_[client] = ti;
        self->state_.res = ti;  // 默认指向最近绑定者, OnKeyboardEnter 会按 client 纠正
        self->state_.surface = nullptr;
        self->state_.enabled = false;
        self->state_.activated = false;
        self->state_.cursorX = self->state_.cursorY = 0;
        self->state_.cursorW = self->state_.cursorH = 0;
    }
    OH_LOG_INFO(LOG_APP, "[TextInput] get_text_input OK (client=%{public}p seat=%{public}p)", client, seat);
}

// ========================================================================
//  zwp_text_input_v3 requests (Wine → compositor)
// ========================================================================

void TextInput::text_input_destroy(wl_client*, wl_resource* res) {
    wl_resource_destroy(res);
}

void TextInput::text_input_enable(wl_client*, wl_resource* res) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    std::lock_guard<std::mutex> lk(self->mutex_);
    self->state_.enabled = true;
    OH_LOG_INFO(LOG_APP, "[TextInput] enable (surface=%{public}p)", self->state_.surface);
}

void TextInput::text_input_disable(wl_client*, wl_resource* res) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    bool wasActivated;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        self->state_.enabled = false;
        wasActivated = self->state_.activated;
        self->state_.activated = false;
        self->state_.cursorX = self->state_.cursorY = 0;
        self->state_.cursorW = self->state_.cursorH = 0;
    }
    OH_LOG_INFO(LOG_APP, "[TextInput] disable (wasActivated=%{public}d)", wasActivated);
    if (wasActivated) self->NotifyActivated(false);
}

void TextInput::text_input_set_surrounding_text(wl_client*, wl_resource* res,
                                                const char*, int32_t, int32_t) {
    // 未使用 (Wine 发不了前置文本语义, 忽略)
}

void TextInput::text_input_set_text_change_cause(wl_client*, wl_resource* res, uint32_t) {
    // 忽略
}

void TextInput::text_input_set_content_type(wl_client*, wl_resource* res,
                                            uint32_t hint, uint32_t purpose) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    (void)hint;
    OH_LOG_DEBUG(LOG_APP, "[TextInput] content_type purpose=%{public}u", purpose);
}

void TextInput::text_input_set_cursor_rectangle(wl_client*, wl_resource* res,
                                                int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    std::lock_guard<std::mutex> lk(self->mutex_);
    self->state_.cursorX = x;
    self->state_.cursorY = y;
    self->state_.cursorW = w;
    self->state_.cursorH = h;
    OH_LOG_INFO(LOG_APP, "[TextInput] cursor_rect=%{public}d,%{public}d %{public}dx%{public}d",
                x, y, w, h);
}

void TextInput::text_input_commit(wl_client*, wl_resource* res) {
    auto* self = static_cast<TextInput*>(wl_resource_get_user_data(res));
    bool shouldActivate = false;
    {
        std::lock_guard<std::mutex> lk(self->mutex_);
        // 激活判定: enabled + 非零光标矩形 (enter 时 Wine 发 0,0,0,0;
        // 文本框聚焦后应用调 SetIMECompositionRect → 非零矩形 → 真文本框)
        if (self->state_.enabled && self->state_.cursorW > 0 && self->state_.cursorH > 0) {
            if (!self->state_.activated) {
                self->state_.activated = true;
                shouldActivate = true;
            }
        }
    }
    if (shouldActivate) {
        OH_LOG_INFO(LOG_APP, "[TextInput] ACTIVATE rect=%{public}d,%{public}d %{public}dx%{public}d",
                    self->state_.cursorX, self->state_.cursorY, self->state_.cursorW, self->state_.cursorH);
        self->NotifyActivated(true);
    }
}

// ========================================================================
//  InputManager 挂接: keyboard enter/leave → text-input enter/leave
// ========================================================================

void TextInput::OnKeyboardEnter(wl_resource* surface) {
    std::lock_guard<std::mutex> lk(mutex_);
    state_.surface = surface;
    state_.enabled = false;
    state_.activated = false;
    state_.cursorX = state_.cursorY = 0;
    state_.cursorW = state_.cursorH = 0;
    if (!surface) return;
    // 按 surface 所属 client 路由到该进程的 text_input 对象
    // (多 Wine 进程各自 get_text_input, 单实例 state_.res 可能指向别的进程)
    wl_client* c = wl_resource_get_client(surface);
    auto it = textInputs_.find(c);
    if (it == textInputs_.end()) {
        state_.res = nullptr;
        OH_LOG_WARN(LOG_APP, "[TextInput] enter DROP: client=%{public}p no text_input", c);
        return;
    }
    state_.res = it->second;
    zwp_text_input_v3_send_enter(state_.res, surface);
    OH_LOG_INFO(LOG_APP, "[TextInput] enter surface=%{public}p (client=%{public}p)", surface, c);
}

void TextInput::OnKeyboardLeave(wl_resource* surface) {
    bool wasActivated;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (state_.res && state_.surface == surface) {
            zwp_text_input_v3_send_leave(state_.res, surface);
            OH_LOG_INFO(LOG_APP, "[TextInput] leave surface=%{public}p", surface);
        }
        state_.surface = nullptr;
        state_.enabled = false;
        wasActivated = state_.activated;
        state_.activated = false;
    }
    if (wasActivated) NotifyActivated(false);
}

// ========================================================================
//  ArkTS 桥接注入: 输入法文本 → Wine (调用方须在 enter 激活后)
// ========================================================================

void TextInput::SendCommitString(const char* utf8) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!state_.res || !state_.surface || !state_.enabled) {
        OH_LOG_WARN(LOG_APP, "[TextInput] SendCommitString DROP (res=%{public}p enabled=%{public}d)",
                    state_.res, state_.enabled);
        return;
    }
    zwp_text_input_v3_send_commit_string(state_.res, utf8);
    zwp_text_input_v3_send_done(state_.res, ++serial_);
    OH_LOG_INFO(LOG_APP, "[TextInput] commit_string \"%{public}s\" serial=%{public}u", utf8, serial_);
}

void TextInput::SendPreeditString(const char* utf8, int32_t cursorBegin, int32_t cursorEnd) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!state_.res || !state_.surface || !state_.enabled) return;
    zwp_text_input_v3_send_preedit_string(state_.res, utf8, cursorBegin, cursorEnd);
    zwp_text_input_v3_send_done(state_.res, ++serial_);
    OH_LOG_INFO(LOG_APP, "[TextInput] preedit \"%{public}s\" [%{public}d,%{public}d] serial=%{public}u",
                utf8, cursorBegin, cursorEnd, serial_);
}

void TextInput::SendDeleteSurrounding(uint32_t before, uint32_t after) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!state_.res || !state_.surface || !state_.enabled) return;
    zwp_text_input_v3_send_delete_surrounding_text(state_.res, before, after);
    zwp_text_input_v3_send_done(state_.res, ++serial_);
    OH_LOG_INFO(LOG_APP, "[TextInput] delete_surrounding before=%{public}u after=%{public}u", before, after);
}

// ========================================================================
//  内部
// ========================================================================

void TextInput::NotifyActivated(bool active) {
    // 拷贝避免跨线程读, 锁外回调 (调用方负责 tsfn 线程安全)
    ActivateCb cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cb = activateCb_;
    }
    if (cb) {
        int x = state_.cursorX, y = state_.cursorY, w = state_.cursorW, h = state_.cursorH;
        cb(active, x, y, w, h);
    }
}

void TextInput::ResetState() {
    std::lock_guard<std::mutex> lk(mutex_);
    state_.enabled = false;
    state_.activated = false;
    state_.cursorX = state_.cursorY = 0;
    state_.cursorW = state_.cursorH = 0;
}

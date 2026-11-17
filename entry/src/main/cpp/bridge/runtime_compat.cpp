#include "runtime_compat.h"
#include "display/display_input.h"
#include "display/display_session.h"
#include "display/x11_toplevel.h"
#include "input/data_device.h"
#include "input/text_input.h"
#include <cstdlib>
#include <string>
#include <uv.h>

extern "C" bool WineHua_DisplayRouteIsX11();
extern "C" char* wl_ohos_input_clipboard_snapshot(uint64_t* version);

namespace {
struct ClipboardCallback {
    napi_env env = nullptr;
    napi_ref callback = nullptr;
    uv_timer_t timer{};
    uint64_t version = 0;
    bool x11 = false;
    bool seen = false;
};

void PollClipboard(uv_timer_t* timer) {
    auto* state = static_cast<ClipboardCallback*>(timer->data);
    const bool x11 = WineHua_DisplayRouteIsX11();
    uint64_t version = 0;
    char* text = x11 ? wl_ohos_input_clipboard_snapshot(&version) : WineClipboardSnapshot(&version);
    const bool changed = !state->seen || state->x11 != x11 || state->version != version;
    state->seen = true; state->x11 = x11; state->version = version;
    if (changed && version && state->callback) {
        napi_handle_scope scope = nullptr;
        if (napi_open_handle_scope(state->env, &scope) == napi_ok) {
            napi_value callback, receiver, value;
            napi_get_reference_value(state->env, state->callback, &callback);
            napi_get_undefined(state->env, &receiver);
            napi_create_string_utf8(state->env, text ? text : "", NAPI_AUTO_LENGTH, &value);
            napi_call_function(state->env, receiver, callback, 1, &value, nullptr);
            napi_close_handle_scope(state->env, scope);
        }
    }
    free(text);
}

void CleanupClipboard(void* data) {
    auto* state = static_cast<ClipboardCallback*>(data);
    uv_timer_stop(&state->timer);
    if (state->callback) napi_delete_reference(state->env, state->callback);
    state->callback = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(&state->timer), [](uv_handle_t* handle) {
        delete static_cast<ClipboardCallback*>(handle->data);
    });
}

napi_value SetClipboardUpdateCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value arg; void* data = nullptr;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, &data);
    auto* state = static_cast<ClipboardCallback*>(data);
    uv_timer_stop(&state->timer);
    if (state->callback) napi_delete_reference(env, state->callback);
    state->callback = nullptr; state->seen = false;
    napi_valuetype type = napi_undefined;
    if (argc && napi_typeof(env, arg, &type) == napi_ok && type == napi_function) {
        if (napi_create_reference(env, arg, 1, &state->callback) == napi_ok)
            uv_timer_start(&state->timer, PollClipboard, 0, 300);
    }
    return nullptr;
}

napi_value SetClipboardText(napi_env env, napi_callback_info info) {
    size_t argc = 1, len = 0; napi_value arg;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    if (!argc || napi_get_value_string_utf8(env, arg, nullptr, 0, &len) != napi_ok || len > 1024 * 1024)
        return nullptr;
    std::string text(len + 1, '\0');
    if (napi_get_value_string_utf8(env, arg, text.data(), text.size(), &len) != napi_ok) return nullptr;
    if (WineHua_DisplayRouteIsX11()) wl_ohos_input_post_clipboard(text.c_str());
    else WineClipboardPost(text.c_str());
    return nullptr;
}

napi_value ImeRearm(napi_env, napi_callback_info) {
    TextInput::GetInstance()->RearmActivation();
    return nullptr;
}

napi_value NotifyToplevelMove(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id; int32_t x, y;
    if (argc == 3 && napi_get_value_uint32(env, args[0], &id) == napi_ok &&
        napi_get_value_int32(env, args[1], &x) == napi_ok && napi_get_value_int32(env, args[2], &y) == napi_ok &&
        x11_toplevel_active()) x11_toplevel_geometry_move(id, x, y);
    return nullptr;
}
} // namespace

void RegisterRuntimeCompat(napi_env env, napi_value exports) {
    uv_loop_t* loop = nullptr;
    if (napi_get_uv_event_loop(env, &loop) != napi_ok || !loop) return;
    auto* state = new ClipboardCallback(); state->env = env;
    if (uv_timer_init(loop, &state->timer)) { delete state; return; }
    state->timer.data = state;
    uv_unref(reinterpret_cast<uv_handle_t*>(&state->timer));
    napi_add_env_cleanup_hook(env, CleanupClipboard, state);
    napi_property_descriptor properties[] = {
        {"setClipboardText", nullptr, SetClipboardText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setClipboardUpdateCallback", nullptr, SetClipboardUpdateCallback, nullptr, nullptr, nullptr, napi_default, state},
        {"imeRearm", nullptr, ImeRearm, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelMove", nullptr, NotifyToplevelMove, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(properties)/sizeof(properties[0]), properties);
}

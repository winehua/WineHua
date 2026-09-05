#include "input/controller/controller_napi.h"

#include "input/controller/controller_hub.h"
#include "input/controller/controller_runtime.h"
#include "input/controller/gamepad_bridge.h"

#include <cstdio>
#include <string>

using winehua::controller::ControllerHub;
using winehua::controller::ControllerSourceId;
using winehua::controller::GamepadBridge;
using winehua::controller::LogicalAxis;
using winehua::controller::LogicalButton;
using winehua::controller::LogicalGamepadState;

namespace {

bool ReadBool(napi_env env, napi_value v, bool* out)
{
    return napi_get_value_bool(env, v, out) == napi_ok;
}

bool ReadInt32(napi_env env, napi_value v, int32_t* out)
{
    return napi_get_value_int32(env, v, out) == napi_ok;
}

bool ReadDouble(napi_env env, napi_value v, double* out)
{
    return napi_get_value_double(env, v, out) == napi_ok;
}

bool ReadUtf8(napi_env env, napi_value v, std::string* out)
{
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return false;
    out->resize(len);
    return napi_get_value_string_utf8(env, v, out->data(), len + 1, &len) == napi_ok;
}

}  // namespace

napi_value ControllerSetEnabled(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    if (argc >= 1) ReadBool(env, args[0], &enabled);
    ControllerHub::Instance().SetEnabled(enabled);
    if (enabled) GamepadBridge::Instance().AttachToHub();
    return nullptr;
}

napi_value ControllerSetButton(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t source = 0, slot = 0, button = 0;
    bool pressed = false;
    if (argc < 4) return nullptr;
    ReadInt32(env, args[0], &source);
    ReadInt32(env, args[1], &slot);
    ReadInt32(env, args[2], &button);
    ReadBool(env, args[3], &pressed);
    ControllerHub::Instance().SetButton(static_cast<ControllerSourceId>(source),
                                        static_cast<uint32_t>(slot),
                                        static_cast<LogicalButton>(button), pressed);
    return nullptr;
}

napi_value ControllerSetAxis(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t source = 0, slot = 0, axis = 0;
    double value = 0;
    if (argc < 4) return nullptr;
    ReadInt32(env, args[0], &source);
    ReadInt32(env, args[1], &slot);
    ReadInt32(env, args[2], &axis);
    ReadDouble(env, args[3], &value);
    ControllerHub::Instance().SetAxis(static_cast<ControllerSourceId>(source),
                                      static_cast<uint32_t>(slot),
                                      static_cast<LogicalAxis>(axis),
                                      static_cast<float>(value));
    return nullptr;
}

napi_value ControllerSetHat(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t source = 0, slot = 0, x = 0, y = 0;
    if (argc < 4) return nullptr;
    ReadInt32(env, args[0], &source);
    ReadInt32(env, args[1], &slot);
    ReadInt32(env, args[2], &x);
    ReadInt32(env, args[3], &y);
    ControllerHub::Instance().SetHat(static_cast<ControllerSourceId>(source),
                                     static_cast<uint32_t>(slot),
                                     static_cast<int8_t>(x), static_cast<int8_t>(y));
    return nullptr;
}

napi_value ControllerResetSource(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t source = 0;
    if (argc >= 1) ReadInt32(env, args[0], &source);
    ControllerHub::Instance().ResetSource(static_cast<ControllerSourceId>(source));
    return nullptr;
}

napi_value ControllerGetState(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = 0;
    if (argc >= 1) ReadInt32(env, args[0], &slot);
    const LogicalGamepadState st = ControllerHub::Instance().GetState(static_cast<uint32_t>(slot));
    napi_value obj;
    napi_create_object(env, &obj);
    auto setInt = [&](const char* key, int64_t value) {
        napi_value v, k;
        napi_create_int64(env, value, &v);
        napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &k);
        napi_set_property(env, obj, k, v);
    };
    setInt("buttons", st.buttons);
    setInt("lx", st.lx);
    setInt("ly", st.ly);
    setInt("rx", st.rx);
    setInt("ry", st.ry);
    setInt("lt", st.lt);
    setInt("rt", st.rt);
    setInt("hatX", st.hatX);
    setInt("hatY", st.hatY);
    setInt("sequence", static_cast<int64_t>(st.sequence));
    return obj;
}

napi_value ControllerGetStateText(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = 0;
    if (argc >= 1) ReadInt32(env, args[0], &slot);
    const LogicalGamepadState st = ControllerHub::Instance().GetState(static_cast<uint32_t>(slot));
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "btn=0x%x LX=%d LY=%d RX=%d RY=%d LT=%u RT=%u hat=%d,%d seq=%llu",
                  st.buttons, st.lx, st.ly, st.rx, st.ry,
                  st.lt, st.rt, st.hatX, st.hatY,
                  static_cast<unsigned long long>(st.sequence));
    napi_value result;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value ControllerStartBridge(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string path;
    if (argc >= 1) ReadUtf8(env, args[0], &path);
    GamepadBridge::Instance().AttachToHub();
    const bool ok = GamepadBridge::Instance().Start(path);
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

napi_value ControllerStopBridge(napi_env, napi_callback_info)
{
    GamepadBridge::Instance().Stop();
    return nullptr;
}

napi_value ControllerGetSocketPath(napi_env env, napi_callback_info)
{
    const std::string path = GamepadBridge::Instance().SocketPath();
    napi_value result;
    napi_create_string_utf8(env, path.c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value ControllerSetOutputMode(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string mode;
    if (argc >= 1) ReadUtf8(env, args[0], &mode);
    winehua::controller::SetOutputMode(mode);
    if (winehua::controller::IsDinputMode()) {
        ControllerHub::Instance().SetEnabled(true);
    } else {
        ControllerHub::Instance().ResetSource(ControllerSourceId::Touch);
        ControllerHub::Instance().ResetSource(ControllerSourceId::Physical);
        GamepadBridge::Instance().Stop();
        ControllerHub::Instance().SetEnabled(false);
    }
    return nullptr;
}

napi_value ControllerGetOutputMode(napi_env env, napi_callback_info)
{
    const std::string mode = winehua::controller::GetOutputMode();
    napi_value result;
    napi_create_string_utf8(env, mode.c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

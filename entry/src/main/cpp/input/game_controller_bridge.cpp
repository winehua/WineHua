#include "input/game_controller_bridge.h"

#include "input/controller/gamepad_bridge.h"
#include "input/controller/physical_gamepad.h"

#include <dlfcn.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WineGamepad"
#include <hilog/log.h>

namespace {

using ErrorCode = int;
using ButtonAction = int;
struct ButtonEvent;
struct AxisEvent;
struct DeviceEvent;
struct DeviceInfos;
using ButtonCallback = void (*)(const ButtonEvent*);
using AxisCallback = void (*)(const AxisEvent*);
using DeviceCallback = void (*)(const DeviceEvent*);
using RegisterButton = ErrorCode (*)(ButtonCallback);
using RegisterAxis = ErrorCode (*)(AxisCallback);
using Unregister = void (*)();

void* gLibrary = nullptr;
bool gInitialized = false;
std::atomic<int> gDeviceCount{0};
std::mutex gMutex;
std::vector<Unregister> gUnregisterFunctions;
napi_threadsafe_function gButtonTsfn = nullptr;
napi_threadsafe_function gAxisTsfn = nullptr;
napi_threadsafe_function gDeviceTsfn = nullptr;
napi_threadsafe_function gRumbleTsfn = nullptr;

ErrorCode (*gGetButtonAction)(const ButtonEvent*, ButtonAction*) = nullptr;
ErrorCode (*gGetButtonCode)(const ButtonEvent*, int*) = nullptr;
ErrorCode (*gGetX)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetY)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetZ)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetRz)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetHatX)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetHatY)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetBrake)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetGas)(const AxisEvent*, double*) = nullptr;
ErrorCode (*gGetDeviceChangedType)(const DeviceEvent*, int*) = nullptr;
ErrorCode (*gGetAllDeviceInfos)(DeviceInfos**) = nullptr;
ErrorCode (*gGetDeviceCount)(const DeviceInfos*, int32_t*) = nullptr;
ErrorCode (*gDestroyDeviceInfos)(DeviceInfos**) = nullptr;

struct ButtonData { int code; bool pressed; };
struct AxisData { int type; double x; double y; };
struct DeviceData { bool connected; };
struct RumbleData { uint32_t low; uint32_t high; uint32_t durationMs; };

template<typename T>
T Resolve(const char* name) {
    return reinterpret_cast<T>(dlsym(gLibrary, name));
}

void ButtonJs(napi_env env, napi_value callback, void*, void* raw) {
    auto* data = static_cast<ButtonData*>(raw);
    if (env && callback && data) {
        napi_value values[2], result;
        napi_create_int32(env, data->code, &values[0]);
        napi_get_boolean(env, data->pressed, &values[1]);
        napi_call_function(env, nullptr, callback, 2, values, &result);
    }
    delete data;
}

void AxisJs(napi_env env, napi_value callback, void*, void* raw) {
    auto* data = static_cast<AxisData*>(raw);
    if (env && callback && data) {
        napi_value values[3], result;
        napi_create_int32(env, data->type, &values[0]);
        napi_create_double(env, data->x, &values[1]);
        napi_create_double(env, data->y, &values[2]);
        napi_call_function(env, nullptr, callback, 3, values, &result);
    }
    delete data;
}

void DeviceJs(napi_env env, napi_value callback, void*, void* raw) {
    auto* data = static_cast<DeviceData*>(raw);
    if (env && callback && data) {
        napi_value value, result;
        napi_get_boolean(env, data->connected, &value);
        napi_call_function(env, nullptr, callback, 1, &value, &result);
    }
    delete data;
}

void RumbleJs(napi_env env, napi_value callback, void*, void* raw) {
    auto* data = static_cast<RumbleData*>(raw);
    if (env && callback && data) {
        napi_value values[3], result;
        napi_create_uint32(env, data->low, &values[0]);
        napi_create_uint32(env, data->high, &values[1]);
        napi_create_uint32(env, data->durationMs, &values[2]);
        napi_call_function(env, nullptr, callback, 3, values, &result);
    }
    delete data;
}

void ForwardRumble(uint16_t low, uint16_t high, uint32_t durationMs) {
    if (!gRumbleTsfn) return;
    auto* data = new RumbleData{low, high, durationMs};
    if (napi_call_threadsafe_function(gRumbleTsfn, data, napi_tsfn_nonblocking) != napi_ok) delete data;
}

void RefreshDeviceCount() {
    if (!gGetAllDeviceInfos || !gGetDeviceCount || !gDestroyDeviceInfos) return;
    DeviceInfos* infos = nullptr;
    int32_t count = 0;
    if (gGetAllDeviceInfos(&infos) == 0 && infos) {
        if (gGetDeviceCount(infos, &count) == 0) {
            gDeviceCount.store(count, std::memory_order_relaxed);
        }
        gDestroyDeviceInfos(&infos);
    }
}

void DeviceChanged(const DeviceEvent* event) {
    if (!event || !gGetDeviceChangedType) return;
    int type = 0;
    gGetDeviceChangedType(event, &type);
    RefreshDeviceCount();
    const bool connected = (type == 1);
    winehua::controller::PhysicalFeedDevice(connected);
    if (!gDeviceTsfn) return;
    auto* data = new DeviceData{connected};
    if (napi_call_threadsafe_function(gDeviceTsfn, data, napi_tsfn_nonblocking) != napi_ok) delete data;
}

void EmitButton(const ButtonEvent* event) {
    if (!event || !gGetButtonAction || !gGetButtonCode) return;
    ButtonAction action = 1;
    int code = 0;
    gGetButtonAction(event, &action);
    gGetButtonCode(event, &code);
    const bool pressed = (action == 0);
    winehua::controller::PhysicalFeedButton(code, pressed);
    if (!gButtonTsfn) return;
    auto* data = new ButtonData{code, pressed};
    if (napi_call_threadsafe_function(gButtonTsfn, data, napi_tsfn_nonblocking) != napi_ok) delete data;
}

void ButtonA(const ButtonEvent* event) { EmitButton(event); }
void ButtonB(const ButtonEvent* event) { EmitButton(event); }
void ButtonX(const ButtonEvent* event) { EmitButton(event); }
void ButtonY(const ButtonEvent* event) { EmitButton(event); }
void ButtonLs(const ButtonEvent* event) { EmitButton(event); }
void ButtonRs(const ButtonEvent* event) { EmitButton(event); }
void ButtonLt(const ButtonEvent* event) { EmitButton(event); }
void ButtonRt(const ButtonEvent* event) { EmitButton(event); }
void ButtonMenu(const ButtonEvent* event) { EmitButton(event); }
void ButtonHome(const ButtonEvent* event) { EmitButton(event); }
void ButtonL3(const ButtonEvent* event) { EmitButton(event); }
void ButtonR3(const ButtonEvent* event) { EmitButton(event); }
void ButtonUp(const ButtonEvent* event) { EmitButton(event); }
void ButtonDown(const ButtonEvent* event) { EmitButton(event); }
void ButtonLeft(const ButtonEvent* event) { EmitButton(event); }
void ButtonRight(const ButtonEvent* event) { EmitButton(event); }

void EmitAxis(int type, double x, double y) {
    winehua::controller::PhysicalFeedAxis(type, x, y);
    if (!gAxisTsfn) return;
    auto* data = new AxisData{type, x, y};
    if (napi_call_threadsafe_function(gAxisTsfn, data, napi_tsfn_nonblocking) != napi_ok) delete data;
}

void LeftStick(const AxisEvent* event) {
    double x = 0, y = 0;
    if (gGetX) gGetX(event, &x);
    if (gGetY) gGetY(event, &y);
    EmitAxis(0, x, y);
}

void RightStick(const AxisEvent* event) {
    double x = 0, y = 0;
    if (gGetZ) gGetZ(event, &x);
    if (gGetRz) gGetRz(event, &y);
    EmitAxis(1, x, y);
}

void DpadAxis(const AxisEvent* event) {
    double x = 0, y = 0;
    if (gGetHatX) gGetHatX(event, &x);
    if (gGetHatY) gGetHatY(event, &y);
    EmitAxis(2, x, y);
}

void LeftTrigger(const AxisEvent* event) {
    double value = 0;
    if (gGetBrake) gGetBrake(event, &value);
    EmitAxis(3, value, 0);
}

void RightTrigger(const AxisEvent* event) {
    double value = 0;
    if (gGetGas) gGetGas(event, &value);
    EmitAxis(4, value, 0);
}

bool RegisterButtonMonitor(const char* registerName, const char* unregisterName, ButtonCallback callback) {
    const auto registerFunction = Resolve<RegisterButton>(registerName);
    if (!registerFunction || registerFunction(callback) != 0) return false;
    const auto unregisterFunction = Resolve<Unregister>(unregisterName);
    if (unregisterFunction) gUnregisterFunctions.push_back(unregisterFunction);
    return true;
}

bool RegisterAxisMonitor(const char* registerName, const char* unregisterName, AxisCallback callback) {
    const auto registerFunction = Resolve<RegisterAxis>(registerName);
    if (!registerFunction || registerFunction(callback) != 0) return false;
    const auto unregisterFunction = Resolve<Unregister>(unregisterName);
    if (unregisterFunction) gUnregisterFunctions.push_back(unregisterFunction);
    return true;
}

int InitializeNative() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gInitialized) return 0;
    gLibrary = dlopen("libohgame_controller.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!gLibrary) {
        OH_LOG_WARN(LOG_APP, "Game Controller Kit unavailable: %{public}s", dlerror());
        return -1;
    }
    gGetButtonAction = Resolve<decltype(gGetButtonAction)>("OH_GamePad_ButtonEvent_GetButtonAction");
    gGetButtonCode = Resolve<decltype(gGetButtonCode)>("OH_GamePad_ButtonEvent_GetButtonCode");
    gGetX = Resolve<decltype(gGetX)>("OH_GamePad_AxisEvent_GetXAxisValue");
    gGetY = Resolve<decltype(gGetY)>("OH_GamePad_AxisEvent_GetYAxisValue");
    gGetZ = Resolve<decltype(gGetZ)>("OH_GamePad_AxisEvent_GetZAxisValue");
    gGetRz = Resolve<decltype(gGetRz)>("OH_GamePad_AxisEvent_GetRZAxisValue");
    gGetHatX = Resolve<decltype(gGetHatX)>("OH_GamePad_AxisEvent_GetHatXAxisValue");
    gGetHatY = Resolve<decltype(gGetHatY)>("OH_GamePad_AxisEvent_GetHatYAxisValue");
    gGetBrake = Resolve<decltype(gGetBrake)>("OH_GamePad_AxisEvent_GetBrakeAxisValue");
    gGetGas = Resolve<decltype(gGetGas)>("OH_GamePad_AxisEvent_GetGasAxisValue");
    gGetDeviceChangedType = Resolve<decltype(gGetDeviceChangedType)>("OH_GameDevice_DeviceEvent_GetChangedType");
    gGetAllDeviceInfos = Resolve<decltype(gGetAllDeviceInfos)>("OH_GameDevice_GetAllDeviceInfos");
    gGetDeviceCount = Resolve<decltype(gGetDeviceCount)>("OH_GameDevice_AllDeviceInfos_GetCount");
    gDestroyDeviceInfos = Resolve<decltype(gDestroyDeviceInfos)>("OH_GameDevice_DestroyAllDeviceInfos");
    if (!gGetButtonAction || !gGetButtonCode) {
        dlclose(gLibrary);
        gLibrary = nullptr;
        return -1;
    }

    RegisterButtonMonitor("OH_GamePad_ButtonA_RegisterButtonInputMonitor", "OH_GamePad_ButtonA_UnregisterButtonInputMonitor", ButtonA);
    RegisterButtonMonitor("OH_GamePad_ButtonB_RegisterButtonInputMonitor", "OH_GamePad_ButtonB_UnregisterButtonInputMonitor", ButtonB);
    RegisterButtonMonitor("OH_GamePad_ButtonX_RegisterButtonInputMonitor", "OH_GamePad_ButtonX_UnregisterButtonInputMonitor", ButtonX);
    RegisterButtonMonitor("OH_GamePad_ButtonY_RegisterButtonInputMonitor", "OH_GamePad_ButtonY_UnregisterButtonInputMonitor", ButtonY);
    RegisterButtonMonitor("OH_GamePad_LeftShoulder_RegisterButtonInputMonitor", "OH_GamePad_LeftShoulder_UnregisterButtonInputMonitor", ButtonLs);
    RegisterButtonMonitor("OH_GamePad_RightShoulder_RegisterButtonInputMonitor", "OH_GamePad_RightShoulder_UnregisterButtonInputMonitor", ButtonRs);
    RegisterButtonMonitor("OH_GamePad_LeftTrigger_RegisterButtonInputMonitor", "OH_GamePad_LeftTrigger_UnregisterButtonInputMonitor", ButtonLt);
    RegisterButtonMonitor("OH_GamePad_RightTrigger_RegisterButtonInputMonitor", "OH_GamePad_RightTrigger_UnregisterButtonInputMonitor", ButtonRt);
    RegisterButtonMonitor("OH_GamePad_ButtonMenu_RegisterButtonInputMonitor", "OH_GamePad_ButtonMenu_UnregisterButtonInputMonitor", ButtonMenu);
    RegisterButtonMonitor("OH_GamePad_ButtonHome_RegisterButtonInputMonitor", "OH_GamePad_ButtonHome_UnregisterButtonInputMonitor", ButtonHome);
    RegisterButtonMonitor("OH_GamePad_LeftThumbstick_RegisterButtonInputMonitor", "OH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor", ButtonL3);
    RegisterButtonMonitor("OH_GamePad_RightThumbstick_RegisterButtonInputMonitor", "OH_GamePad_RightThumbstick_UnregisterButtonInputMonitor", ButtonR3);
    RegisterButtonMonitor("OH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor", ButtonUp);
    RegisterButtonMonitor("OH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor", ButtonDown);
    RegisterButtonMonitor("OH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor", ButtonLeft);
    RegisterButtonMonitor("OH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor", ButtonRight);
    RegisterAxisMonitor("OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor", "OH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor", LeftStick);
    RegisterAxisMonitor("OH_GamePad_RightThumbstick_RegisterAxisInputMonitor", "OH_GamePad_RightThumbstick_UnregisterAxisInputMonitor", RightStick);
    RegisterAxisMonitor("OH_GamePad_Dpad_RegisterAxisInputMonitor", "OH_GamePad_Dpad_UnregisterAxisInputMonitor", DpadAxis);
    RegisterAxisMonitor("OH_GamePad_LeftTrigger_RegisterAxisInputMonitor", "OH_GamePad_LeftTrigger_UnregisterAxisInputMonitor", LeftTrigger);
    RegisterAxisMonitor("OH_GamePad_RightTrigger_RegisterAxisInputMonitor", "OH_GamePad_RightTrigger_UnregisterAxisInputMonitor", RightTrigger);
    const auto registerDevice = Resolve<ErrorCode (*)(DeviceCallback)>("OH_GameDevice_RegisterDeviceMonitor");
    if (registerDevice && registerDevice(DeviceChanged) == 0) {
        const auto unregisterDevice = Resolve<Unregister>("OH_GameDevice_UnregisterDeviceMonitor");
        if (unregisterDevice) gUnregisterFunctions.push_back(unregisterDevice);
    }
    RefreshDeviceCount();
    gInitialized = true;
    OH_LOG_INFO(LOG_APP, "Game Controller Kit initialized with %{public}zu monitors", gUnregisterFunctions.size());
    return 0;
}

void CleanupNative() {
    std::lock_guard<std::mutex> lock(gMutex);
    for (auto function : gUnregisterFunctions) function();
    gUnregisterFunctions.clear();
    if (gLibrary) dlclose(gLibrary);
    gLibrary = nullptr;
    gInitialized = false;
    gDeviceCount.store(0, std::memory_order_relaxed);
}

void ReleaseTsfn(napi_threadsafe_function& value) {
    if (value) napi_release_threadsafe_function(value, napi_tsfn_release);
    value = nullptr;
}

bool InstallTsfn(napi_env env, napi_callback_info info, const char* name,
                 napi_threadsafe_function_call_js callback, napi_threadsafe_function& target) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    ReleaseTsfn(target);
    if (argc < 1) return false;
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) return false;
    napi_value resourceName;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &resourceName);
    return napi_create_threadsafe_function(env, args[0], nullptr, resourceName, 0, 1,
        nullptr, nullptr, nullptr, callback, &target) == napi_ok;
}

} // namespace

namespace winehua {
namespace controller {

int EnsurePhysicalGamepadInitialized()
{
    return InitializeNative();
}

}  // namespace controller
}  // namespace winehua

napi_value InitGameController(napi_env env, napi_callback_info) {
    napi_value result;
    napi_create_int32(env, winehua::controller::EnsurePhysicalGamepadInitialized(), &result);
    return result;
}

napi_value CleanupGameController(napi_env, napi_callback_info) {
    CleanupNative();
    winehua::controller::GamepadBridge::Instance().SetRumbleListener(nullptr);
    ReleaseTsfn(gButtonTsfn);
    ReleaseTsfn(gAxisTsfn);
    ReleaseTsfn(gDeviceTsfn);
    ReleaseTsfn(gRumbleTsfn);
    return nullptr;
}

napi_value IsGamepadConnected(napi_env env, napi_callback_info) {
    napi_value result;
    napi_get_boolean(env, gDeviceCount.load(std::memory_order_relaxed) > 0, &result);
    return result;
}

napi_value GetGamepadCount(napi_env env, napi_callback_info) {
    napi_value result;
    napi_create_int32(env, gDeviceCount.load(std::memory_order_relaxed), &result);
    return result;
}

napi_value SetGamepadButtonCallback(napi_env env, napi_callback_info info) {
    InstallTsfn(env, info, "WineGamepadButton", ButtonJs, gButtonTsfn);
    return nullptr;
}

napi_value SetGamepadAxisCallback(napi_env env, napi_callback_info info) {
    InstallTsfn(env, info, "WineGamepadAxis", AxisJs, gAxisTsfn);
    return nullptr;
}

napi_value SetGamepadDeviceCallback(napi_env env, napi_callback_info info) {
    InstallTsfn(env, info, "WineGamepadDevice", DeviceJs, gDeviceTsfn);
    return nullptr;
}

napi_value SetGamepadRumbleCallback(napi_env env, napi_callback_info info) {
    InstallTsfn(env, info, "WineGamepadRumble", RumbleJs, gRumbleTsfn);
    winehua::controller::GamepadBridge::Instance().SetRumbleListener(ForwardRumble);
    return nullptr;
}

/* smoke NAPI 模块 —— 自动化测试设施的原生侧, 与 ets/smoke/ 的物理隔离对应。
 *
 * 边界约定: 本 so 只提供测试编排需要的查询/注入原语, 不承载产品语义;
 * 产品代码 (bridge/napi_init.cpp 及其余域) 不引用本文件。符号来自 entry.so
 * (运行时由同进程的 entry so 解析), 因此必须与 entry 同包加载。
 *
 * 当前接口:
 *   smokeMapPoint(guestX, guestY) -> {px, py} | null
 *     guest 桌面坐标 → XComponent 物理坐标, 供 SmokeRunner 注入编排换算。
 *     与 CoordTransform 用同一 letterbox 几何的正向 FitMapDisplay 映射 ——
 *     映射判据单一实现, 测试侧不自带换算逻辑。renderer 解析必须与
 *     findToplevelAt 的 CoordTransform 同锚 (root): 两侧不同 renderer 的
 *     letterbox 互不逆 (窗口 renderer 的 off 是装饰尺寸, root 是恒等映射),
 *     注入点会系统性偏移一个装饰量 (2026-09-26 实测 Y+22)。
 */
#include <napi/native_api.h>
#include "compositor/input/input_space_mapper.h"
#include "compositor/frame/geometry.h"
#include "compositor/wayland_server.h"
#include "bridge/plugin_manager.h"
#include "graphics/egl_renderer.h"

static napi_value SmokeMapPoint(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    double gx, gy;
    napi_get_value_double(env, args[0], &gx);
    napi_get_value_double(env, args[1], &gy);

    auto* ws = WaylandServer::GetInstance();
    uint32_t anchorId = ws->GetDesktopRootToplevelId();
    EglRenderer* r = InputSpaceMapper::GetInstance()->ResolveRendererFor(anchorId > 0 ? anchorId : 1);
    if (!r) r = PluginManager::GetInstance()->GetAnyRenderer();
    if (!r) return nullptr;
    FitRect lb = r->GetInputLetterbox();
    if (lb.dstW <= 0 || lb.dstH <= 0 || lb.srcW <= 0 || lb.srcH <= 0) return nullptr;

    napi_value result, nx, ny;
    napi_create_object(env, &result);
    napi_create_int32(env, FitMapDisplayX(lb, static_cast<int64_t>(gx)), &nx);
    napi_create_int32(env, FitMapDisplayY(lb, static_cast<int64_t>(gy)), &ny);
    napi_set_named_property(env, result, "px", nx);
    napi_set_named_property(env, result, "py", ny);
    return result;
}

// DisplayRoute M0 bring-up 触发 (计划 M0-T7 Step 3): 测试设施不进产品路径,
// 显示路线入口在 entry.so 的 display/display_compositor.cpp, 同进程符号直解
extern "C" void WineHua_DisplayRoute_Start();
extern "C" void WineHua_DisplayRoute_Stop();
extern "C" void WineHua_SetDisplayRoute(const char* route);
// T8: surfaceId 非零时同步启动出图链 (XComponent → NativeWindow 直推);
// scriptEnabled = 真机门自动注入脚本 (smoke 验证编排, 默认关 —— 测试资产
// 不默认进产品行为)
extern "C" void WineHua_DisplayRoute_StartWithSurface(uint64_t surface_id,
                                                      bool script_enabled,
                                                      int out_w, int out_h,
                                                      bool canvas_egl_present);
// M1-T1: 注入桥 (display/display_input.c)。key = evdev 键码; motion =
// client surface 相对坐标 0..1, phase 0=enter 1=motion 2=leave。
// 线程纪律: wlr_seat 无锁, 注入必须落在合成器事件循环线程; NAPI 调用来自
// ArkTS 线程, 一律走 post 投递队列 (display_input 内部转循环线程执行),
// 不得直呼循环线程版 (2026-09-29 评审 fix#2)。
extern "C" void wl_ohos_input_post_key(uint32_t keycode, bool press);
extern "C" void wl_ohos_input_post_motion(float nx, float ny, int phase);
extern "C" void wl_ohos_input_post_button(uint32_t button, bool press);

static napi_value SmokeDisplayRoute(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool script_enabled = false;
    if (argc >= 2) {
        napi_get_value_bool(env, args[1], &script_enabled);
    }
    /* M3a: 可选 w/h (x11 台架 = surface 实际尺寸); 缺省 0 ⇒ 800x600,
     * smoke 台架调用方 (SmokeDevPanel/SmokeRunner) 不传即零差异 */
    int out_w = 0, out_h = 0;
    if (argc >= 4) {
        double w = 0, h = 0;
        napi_get_value_double(env, args[2], &w);
        napi_get_value_double(env, args[3], &h);
        out_w = static_cast<int>(w);
        out_h = static_cast<int>(h);
    }
    /* M3a-T7: 可选第 5 参 = 画布绑定 (DesktopAbility 全屏窗), present 走
     * EGL swap —— 该 surface 对手工 FlushBuffer 冻结 (语义见 ohos_output.h)。
     * 缺省 false = fusion/smoke 台架零差异。 */
    bool canvas_egl_present = false;
    if (argc >= 5) {
        napi_get_value_bool(env, args[4], &canvas_egl_present);
    }
    if (argc >= 1) {
        uint64_t surface_id = 0;
        bool lossless = false;
        napi_get_value_bigint_uint64(env, args[0], &surface_id, &lossless);
        WineHua_DisplayRoute_StartWithSurface(surface_id, script_enabled, out_w,
                                              out_h, canvas_egl_present);
    } else {
        WineHua_DisplayRoute_Start();
    }
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

// smokeDisplayRouteKey(keycode, press) — T1 键注入 (真机门自动定时器之外的
// 手动通道); 投递到合成循环执行 (线程纪律见上方注入桥注释)
static napi_value SmokeSetDisplayRoute(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        char route[16] = {0};
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], route, sizeof(route), &len);
        WineHua_SetDisplayRoute(route);
    }
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}
extern "C" void WineHua_DisplayRoute_Resize(int w, int h);

// smokeDisplayRouteResize(w, h) — D10: 画布尺寸动态响应 (折叠/旋转后
// onSurfaceChanged 的逻辑尺寸), 链已启动时同步 output 几何。
static napi_value SmokeDisplayRouteResize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 2) {
        double w = 0, h = 0;
        napi_get_value_double(env, args[0], &w);
        napi_get_value_double(env, args[1], &h);
        WineHua_DisplayRoute_Resize((int)w, (int)h);
    }
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}
static napi_value SmokeDisplayRouteStop(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    WineHua_DisplayRoute_Stop();
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}
static napi_value SmokeDisplayRouteKey(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    double code = 0;
    bool press = false;
    napi_get_value_double(env, args[0], &code);
    napi_get_value_bool(env, args[1], &press);
    wl_ohos_input_post_key(static_cast<uint32_t>(code), press);
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

// smokeDisplayRouteMotion(nx, ny, phase)
static napi_value SmokeDisplayRouteMotion(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    double nx = 0, ny = 0;
    double phase = 0;
    napi_get_value_double(env, args[0], &nx);
    napi_get_value_double(env, args[1], &ny);
    napi_get_value_double(env, args[2], &phase);
    wl_ohos_input_post_motion(static_cast<float>(nx), static_cast<float>(ny),
                              static_cast<int>(phase));
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

// smokeDisplayRouteButton(button, press) — M3a-T4 按钮注入; button = evdev
// 按钮码 (BTN_LEFT 0x110 / BTN_RIGHT 0x111 / BTN_MIDDLE 0x112)
static napi_value SmokeDisplayRouteButton(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    double button = 0;
    bool press = false;
    napi_get_value_double(env, args[0], &button);
    napi_get_value_bool(env, args[1], &press);
    wl_ohos_input_post_button(static_cast<uint32_t>(button), press);
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

EXTERN_C_START
static napi_value SmokeNapiInit(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"smokeMapPoint", nullptr, SmokeMapPoint, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRoute", nullptr, SmokeDisplayRoute, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRouteStop", nullptr, SmokeDisplayRouteStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRouteResize", nullptr, SmokeDisplayRouteResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayRoute", nullptr, SmokeSetDisplayRoute, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRouteKey", nullptr, SmokeDisplayRouteKey, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRouteMotion", nullptr, SmokeDisplayRouteMotion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"smokeDisplayRouteButton", nullptr, SmokeDisplayRouteButton, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}
EXTERN_C_END

static napi_module g_smokeNapiModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = SmokeNapiInit,
    .nm_modname = "smokeNapi",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterSmokeNapiModule(void) {
    napi_module_register(&g_smokeNapiModule);
}

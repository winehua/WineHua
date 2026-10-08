/* display_route NAPI 模块 —— x11 显示路线控制面的产品入口 (ArkTS → 原生)。
 *
 * 边界约定 (D21): 本模块承载全部产品调用面 (桌面窗 / 启动链 / IME 桥);
 * 测试专用原语 (smokeMapPoint) 留在 smoke_napi.so, 产品代码不引用 ——
 * 历史上这组函数寄居 smoke_napi (M0 bring-up 只服务测试台架), 产品功能
 * 顶着测试模块名, 违反测试资产不进产品路径的边界。
 *
 * 符号来自 entry.so (运行时由同进程的 entry so 解析), 因此必须与 entry
 * 同包加载。
 *
 * 接口一览:
 *   startDisplayRouteChain(sid?, script?, w?, h?, canvasEgl?) — 启动出图链
 *     (sid 非零 = 同步绑 XComponent → NativeWindow 直推; script = 真机门
 *     自动注入脚本, 测试编排专用, 产品恒 false; w/h = x11 台架输出尺寸;
 *     canvasEgl = 画布绑定走 EGL swap, 语义见 ohos_output.h)
 *   stopDisplayRouteChain() — 停链
 *   setDisplayRoute(route) — 路线选择 ("fusion"/"x11")
 *   resizeDisplayRouteOutput(w, h) — 画布尺寸动态响应 (折叠/旋转)
 *   injectDisplayRouteKey(keycode, press) — evdev 键注入
 *   injectDisplayRouteMotion(nx, ny, phase) — 指针注入 (0..1, 0=enter
 *     1=motion 2=leave)
 *   injectDisplayRouteButton(button, press) — 按钮注入 (BTN_LEFT 0x110 等)
 *   injectDisplayRouteAxis(which, steps) — 轴注入 (滚轮; D15 手势层)
 *   setDisplayRouteClipboard(utf8) — D19 CJK 剪贴板桥: UTF-8 串设为 seat
 *     selection (xwm 桥接 Xwayland CLIPBOARD), 配合 Ctrl+V 注入粘贴
 *
 * 线程纪律: 注入桥 (display_input.c) 的 wlr_seat 无锁, 一律走 post 投递
 * 队列 (内部转合成器事件循环线程执行); NAPI 调用来自 ArkTS 线程, 不得
 * 直呼循环线程版 (2026-09-29 评审 fix#2)。
 */
#include <napi/native_api.h>
#include <cstdlib>

extern "C" void WineHua_DisplayRoute_Start();
extern "C" void WineHua_DisplayRoute_Stop();
extern "C" void WineHua_SetDisplayRoute(const char* route);
extern "C" bool WineHua_DisplayRouteIsX11();
extern "C" void WineHua_DisplayRoute_StartWithSurface(uint64_t surface_id,
                                                      bool script_enabled,
                                                      int out_w, int out_h,
                                                      bool canvas_egl_present);
extern "C" void WineHua_DisplayRoute_Resize(int w, int h);
extern "C" void wl_ohos_output_frame_size(int *w, int *h);
extern "C" void wl_ohos_input_post_key(uint32_t keycode, bool press);
extern "C" void wl_ohos_input_post_motion(float nx, float ny, int phase);
extern "C" void wl_ohos_input_post_button(uint32_t button, bool press);
extern "C" void wl_ohos_input_post_axis(int which, int steps);
extern "C" void wl_ohos_input_post_text(const char* utf8);
extern "C" void wl_ohos_input_post_clipboard(const char *utf8);

static napi_value StartDisplayRouteChain(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool script_enabled = false;
    if (argc >= 2) {
        napi_get_value_bool(env, args[1], &script_enabled);
    }
    /* 可选 w/h (x11 台架 = surface 实际尺寸); 缺省 0 ⇒ 800x600 */
    int out_w = 0, out_h = 0;
    if (argc >= 4) {
        double w = 0, h = 0;
        napi_get_value_double(env, args[2], &w);
        napi_get_value_double(env, args[3], &h);
        out_w = static_cast<int>(w);
        out_h = static_cast<int>(h);
    }
    /* 可选第 5 参 = 画布绑定 (DesktopAbility 全屏窗), present 走 EGL swap */
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

static napi_value StopDisplayRouteChain(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    WineHua_DisplayRoute_Stop();
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

static napi_value SetDisplayRoute(napi_env env, napi_callback_info info) {
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

static napi_value ResizeDisplayRouteOutput(napi_env env, napi_callback_info info) {
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

static napi_value InjectDisplayRouteKey(napi_env env, napi_callback_info info) {
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

static napi_value InjectDisplayRouteMotion(napi_env env, napi_callback_info info) {
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

static napi_value InjectDisplayRouteButton(napi_env env, napi_callback_info info) {
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

/* displayRouteIsX11() — 进程内路线镜像 (wine_env.cpp 的唯一写点) 的读取口。
 * smoke 注入编排按它分叉: x11 走 injectDisplayRoute* (归一化画布坐标),
 * wayland 走 testNapi.sendPointerEvent (toplevel 像素坐标)。 */
static napi_value DisplayRouteIsX11(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value result;
    napi_get_boolean(env, WineHua_DisplayRouteIsX11(), &result);
    return result;
}

/* displayRouteFrameSize() — X 屏幕逻辑尺寸 (guest 桌面坐标系, 即 wine 桌面
 * 坐标 = 该坐标系)。C 型注入编排把 guest 坐标换算成 injectDisplayRouteMotion
 * 的 0..1 归一化坐标时作分母。链未起时返回 0x0, 调用方按失败处理。 */
static napi_value DisplayRouteFrameSize(napi_env env, napi_callback_info info) {
    (void)info;
    int w = 0, h = 0;
    wl_ohos_output_frame_size(&w, &h);
    napi_value result, nw, nh;
    napi_create_object(env, &result);
    napi_create_int32(env, w, &nw);
    napi_create_int32(env, h, &nh);
    napi_set_named_property(env, result, "w", nw);
    napi_set_named_property(env, result, "h", nh);
    return result;
}

/* injectDisplayRouteAxis(which, steps) — D15 手势层: 滚轮注入。which
 * 0=纵向 1=横向; steps=±N discrete 步, 方向判据见 display_input.c
 * (discrete>0 → 滚轮向下, 与 wayland 分支「向上=正」同号)。 */
static napi_value InjectDisplayRouteAxis(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    double which = 0, steps = 0;
    napi_get_value_double(env, args[0], &which);
    napi_get_value_double(env, args[1], &steps);
    wl_ohos_input_post_axis(static_cast<int>(which), static_cast<int>(steps));
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

/* injectDisplayRouteText(text) — XIM spec Task 4: x11 路线 IME commit。
 * UTF-8 串经 post 队列转 xim bridge, NCP xim server 以 XIM_COMMIT 投给
 * wine (Wayland 路线的上屏走 winewayland.drv text-input, 不经此)。 */
static napi_value InjectDisplayRouteText(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
        char *text = static_cast<char *>(malloc(len + 1));
        if (text) {
            napi_get_value_string_utf8(env, args[0], text, len + 1, &len);
            wl_ohos_input_post_text(text);
            free(text);
        }
    }
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

/* setDisplayRouteClipboard(text) — D19 CJK 剪贴板桥。典型调用序:
 * setDisplayRouteClipboard(中文串) → injectDisplayRouteKey(29,true)
 * injectDisplayRouteKey(47,true) injectDisplayRouteKey(47,false)
 * injectDisplayRouteKey(29,false)。 */
static napi_value SetDisplayRouteClipboard(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
        char *text = static_cast<char *>(malloc(len + 1));
        if (text) {
            napi_get_value_string_utf8(env, args[0], text, len + 1, &len);
            wl_ohos_input_post_clipboard(text);
            free(text);
        }
    }
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

EXTERN_C_START
static napi_value DisplayRouteNapiInit(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"startDisplayRouteChain", nullptr, StartDisplayRouteChain, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopDisplayRouteChain", nullptr, StopDisplayRouteChain, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayRoute", nullptr, SetDisplayRoute, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"displayRouteIsX11", nullptr, DisplayRouteIsX11, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"displayRouteFrameSize", nullptr, DisplayRouteFrameSize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeDisplayRouteOutput", nullptr, ResizeDisplayRouteOutput, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectDisplayRouteKey", nullptr, InjectDisplayRouteKey, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectDisplayRouteMotion", nullptr, InjectDisplayRouteMotion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectDisplayRouteButton", nullptr, InjectDisplayRouteButton, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectDisplayRouteAxis", nullptr, InjectDisplayRouteAxis, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectDisplayRouteText", nullptr, InjectDisplayRouteText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayRouteClipboard", nullptr, SetDisplayRouteClipboard, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}
EXTERN_C_END

static napi_module g_displayRouteNapiModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = DisplayRouteNapiInit,
    .nm_modname = "displayRouteNapi",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterDisplayRouteNapiModule(void) {
    napi_module_register(&g_displayRouteNapiModule);
}

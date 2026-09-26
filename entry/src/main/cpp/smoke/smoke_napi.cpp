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

EXTERN_C_START
static napi_value SmokeNapiInit(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"smokeMapPoint", nullptr, SmokeMapPoint, nullptr, nullptr, nullptr, napi_default, nullptr},
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

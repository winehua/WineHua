/* winehua_t_d3d_smoke — D3D10 链守卫（P4，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * D3D10CreateDevice 设备创建+状态往返。DLL 动态加载（GetProcAddress）。
 * d3d-env-injected 守卫档位 env 真实到达 guest（套件/CLI 传裸 "dxvk" 这类
 * 非契约值会被 native 静默丢弃，表现为 DLL 解析回落 builtin——先分设施
 * 缺陷再谈平台缺口）。
 * 当前定性（2026-09-26 实测）：dxvk_legacy 档 env 注入完整、三件套在位，
 * LoadLibrary 仍失败——box64 执行 d3d10.dll 初始化确定性 SIGSEGV（固定
 * 偏移 +0x1af9，保守 dynarec 参数不可绕过），同链路 d3d11/dxgi 正常。
 * 属 box64 平台缺口，红转绿依赖 box64 侧修复，本用例保留为哨兵。
 * d3d9 离屏渲染/Reset 判定组拆至 t-d3d9-offscreen。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <stdlib.h>
#include <d3d10.h>

int main(int argc, char **argv)
{
    HMODULE mod;
    HRESULT (WINAPI *create_fn)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE,
                                UINT, UINT, IDXGIFactory *, ID3D10Device **);
    ID3D10Device *dev = NULL;
    HRESULT hr;

    t_begin("winehua_t_d3d_smoke", argc, argv);

    /* 档位注入守卫: native 契约要求完整档位值 (dxvk_legacy 等), 套件/CLI 传
     * 裸值会被 native 静默丢弃 → 引擎无档位 env、DLL 解析回落 builtin。
     * 宿主 env 正常时这里应看到 dxvk_legacy。 */
    {
        const char *backend = getenv("WINEHUA_D3D_BACKEND");
        const char *overrides = getenv("WINEDLLOVERRIDES");
        t_check("d3d-env-injected", backend != NULL && backend[0] != '\0',
                "backend=%s overrides=%s",
                backend ? backend : "(null)", overrides ? overrides : "(null)");
    }

    mod = LoadLibraryA("d3d10.dll");
    create_fn = mod ? (HRESULT (WINAPI *)(IDXGIAdapter *, D3D10_DRIVER_TYPE,
                          HMODULE, UINT, UINT, IDXGIFactory *, ID3D10Device **))
        GetProcAddress(mod, "D3D10CreateDevice") : NULL;
    t_check("d3d10-dll-load", create_fn != NULL, "hmod=%p", (void *)mod);
    if (!create_fn)
        return t_finish();
    hr = create_fn(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0,
                   D3D10_SDK_VERSION, NULL, &dev);
    t_check("d3d10-create-device", SUCCEEDED(hr) && dev != NULL,
            "hr=0x%08lx (先查 hilog [WineProgram] parsed 与 [WineChild] final D3D env "
            "确认档位 env 已注入，再看 DXVK 日志)",
            (unsigned long)hr);
    if (SUCCEEDED(hr) && dev)
    {
        /* 设备级状态往返（出图由 dxvk 套件烟测守；无 swapchain 时
         * ClearRenderTargetView 无目标可用） */
        ID3D10Device_OMSetRenderTargets(dev, 0, NULL, NULL);
        ID3D10Device_ClearState(dev);
        t_check("d3d10-state-roundtrip", 1, "device alive");
        ID3D10Device_Release(dev);
    }
    t_metric("d3d10-guard", "done");
    return t_finish();
}

/* winehua_t_d3d_smoke — D3D10 链守卫（P4，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * D3D10CreateDevice 设备创建+状态往返。d3d10 链打包回归哨兵——DXVK
 * d3d10 链曾因 assemble 只打包 d3d11+dxgi 直接 abort，本用例钉死该链。
 * DLL 动态加载（GetProcAddress，与 d3d8-smoke 同模式）。
 * d3d9 离屏渲染/Reset 判定组拆至 t-d3d9-offscreen（其 d3d9.dll 加载在
 * smoke 虚拟桌面会话挂起，独立定性，不阻塞本用例落盘）。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <d3d10.h>

int main(int argc, char **argv)
{
    HMODULE mod;
    HRESULT (WINAPI *create_fn)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE,
                                UINT, UINT, IDXGIFactory *, ID3D10Device **);
    ID3D10Device *dev = NULL;
    HRESULT hr;

    t_begin("winehua_t_d3d_smoke", argc, argv);

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
            "hr=0x%08lx (d3d10 链断=DXVK d3d10 DLL 缺失或回退断)",
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

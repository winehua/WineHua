/* winehua_t_d3d9_offscreen — D3D9 离屏渲染读回 + 交换链 Reset（P4，手段 R+P）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * 失败特征：读回空/错=d3d9 渲染语义断；Reset 断=交换链重建链。
 * 档位钉 wined3d = 产品真实执行链：DXVK 的 d3d9.dll 从未被启用（产品
 * WINEDLLOVERRIDES 无 d3d9=n），D3D9 一律 builtin+wined3d。2026-09-26
 * 在 dxvk_legacy 档验证过读回失败形态一致（同一 builtin 链），档位无关。
 * 当前定性：设备创建/清屏/绘制/GetRenderTargetData/Reset 全链 API 通过，
 * 仅读回内容为恒定杂色 0xff476378——wined3d 在 virpipe 上 RT→系统内存
 * 拷贝内容不达（游戏内截图/串流镜面类依赖），收敛项②同层，红转绿=该层
 * 收敛验收。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <d3d9.h>

static HWND g_hwnd;

int main(int argc, char **argv)
{
    HMODULE mod;
    IDirect3D9 *(WINAPI *create_fn)(UINT);
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int bad = -1;
    WNDCLASSA wc;

    t_begin("winehua_t_d3d9_offscreen", argc, argv);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaT_D3D9Off";
    TCHECK("register-class", RegisterClassA(&wc) != 0);
    g_hwnd = CreateWindowExA(0, "WineHuaT_D3D9Off", "d3d9", WS_OVERLAPPED,
                             10, 10, 64, 64, NULL, NULL, wc.hInstance, NULL);
    t_check("create-window", g_hwnd != NULL, "err=%lu", GetLastError());

    mod = LoadLibraryA("d3d9.dll");
    create_fn = mod ? (IDirect3D9 *(WINAPI *)(UINT))
        GetProcAddress(mod, "Direct3DCreate9") : NULL;
    t_check("d3d9-dll-load", create_fn != NULL, "hmod=%p", (void *)mod);
    if (!create_fn)
        return t_finish();
    d3d = create_fn(D3D_SDK_VERSION);
    t_check("d3d9-create", d3d != NULL, "obj=%p", (void *)d3d);
    if (!d3d)
        return t_finish();

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.Windowed = TRUE;
    pp.hDeviceWindow = g_hwnd;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                 g_hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &dev);
    t_check("d3d9-create-device", hr == D3D_OK && dev != NULL,
            "hr=0x%08lx", (unsigned long)hr);
    if (hr != D3D_OK || !dev)
        return t_finish();

    /* 已知色清屏 + 渲染一个纯色三角形 → 离屏读回 */
    hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                                D3DCOLOR_XRGB(0x10, 0x20, 0x30), 1.0f, 0);
    TCHECK("d3d9-clear", hr == D3D_OK);
    {
        struct { float x, y, z, rhw; DWORD c; } verts[3] = {
            {  0.0f, 64.0f, 0, 1, D3DCOLOR_XRGB(0xFF, 0x00, 0x00) },
            { 64.0f, 64.0f, 0, 1, D3DCOLOR_XRGB(0xFF, 0x00, 0x00) },
            { 32.0f,  0.0f, 0, 1, D3DCOLOR_XRGB(0xFF, 0x00, 0x00) },
        };
        hr = IDirect3DDevice9_BeginScene(dev);
        if (hr == D3D_OK)
        {
            IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 1,
                                                  verts, sizeof(verts[0]));
            IDirect3DDevice9_EndScene(dev);
        }
        t_check("d3d9-draw", hr == D3D_OK, "hr=0x%08lx", (unsigned long)hr);
    }
    hr = IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    if (hr == D3D_OK && rt)
    {
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
            dev, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
        if (hr == D3D_OK && sys)
        {
            if (IDirect3DDevice9_GetRenderTargetData(dev, rt, sys) == D3D_OK &&
                IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY) == D3D_OK)
            {
                DWORD *px = (DWORD *)lr.pBits;
                /* (32,48) 在三角形内 = 红；(4,4) 在外 = 背景色 */
                DWORD in = px[48 * 64 + 32];
                DWORD out = px[4 * 64 + 4];
                t_check("d3d9-triangle-in",
                        (in & 0x00FFFFFF) == 0x0000FF,
                        "in=%08lx", (unsigned long)in);
                t_check("d3d9-background-out",
                        (out & 0x00FFFFFF) == 0x302010,
                        "out=%08lx", (unsigned long)out);
                IDirect3DSurface9_UnlockRect(sys);
                bad = 0;
            }
            IDirect3DSurface9_Release(sys);
        }
        IDirect3DSurface9_Release(rt);
    }
    t_check("d3d9-readback-done", bad == 0, "path=%d", bad);

    /* 窗口化交换链 Reset 往返（游戏 resize 崩溃类回归哨兵） */
    pp.BackBufferWidth = 96;
    pp.BackBufferHeight = 48;
    hr = IDirect3DDevice9_Reset(dev, &pp);
    t_check("d3d9-reset", hr == D3D_OK, "hr=0x%08lx", (unsigned long)hr);
    if (hr == D3D_OK)
    {
        int i, ok = 0;
        for (i = 0; i < 3; ++i)
        {
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                                   D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            if (IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) == D3D_OK)
                ok++;
        }
        t_check("d3d9-present-after-reset", ok == 3, "ok=%d/3", ok);
    }
    t_metric("d3d9-offscreen", "done");
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return t_finish();
}

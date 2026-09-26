/* winehua_t_ddraw_surface — DirectDraw 表面协作（P4，手段 R+P）。
 * 判定规格见 docs/engineering/testing-programs.md §3.20。
 * 失败特征：Lock 失败=表面管理断；错色=Blt 路径断（cnc-ddraw 类 wrap 链守卫）。
 * 协议：DirectDrawCreateEx（动态加载）→离屏 32bpp 表面×2→Lock 写已知图案→
 * Unlock→Blt→读回一致；BltColorFill；8bpp 调色板表面索引渲染读回。
 * 主表面/Flip 的像素半段受缺口②（屏幕回灌缺失）限，Flip 只做 API 往返。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <ddraw.h>

static HMODULE g_ddraw;
static LPDIRECTDRAW7 g_dd;
static HWND g_hwnd;

static int create_ddraw(void)
{
    HRESULT (WINAPI *create_fn)(GUID *, LPVOID *, REFIID, IUnknown *);
    g_ddraw = LoadLibraryA("ddraw.dll");
    if (!g_ddraw) return 0;
    create_fn = (HRESULT (WINAPI *)(GUID *, LPVOID *, REFIID, IUnknown *))
        GetProcAddress(g_ddraw, "DirectDrawCreateEx");
    if (!create_fn) return 0;
    return create_fn(NULL, (LPVOID *)&g_dd, &IID_IDirectDraw7, NULL) == DD_OK && g_dd;
}

/* bpp>0 时显式声明像素格式（8bpp 调色板表面必须显式）；0 = 跟随主表面
 * 格式（wine 对离屏表面显式声明 32bpp RGB 格式会拒绝） */
static int create_offscreen(LPDIRECTDRAWSURFACE7 *out, int w, int h, int bpp)
{
    DDSURFACEDESC2 desc;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = w;
    desc.dwHeight = h;
    if (bpp > 0)
    {
        desc.dwFlags |= DDSD_PIXELFORMAT;
        desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
        desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        desc.ddpfPixelFormat.dwRGBBitCount = bpp;
    }
    return IDirectDraw7_CreateSurface(g_dd, &desc, out, NULL) == DD_OK && *out;
}

int main(int argc, char **argv)
{
    LPDIRECTDRAWSURFACE7 src = NULL, dst = NULL, pal_surf = NULL;
    DDSURFACEDESC2 desc;
    HRESULT hr;

    t_begin("winehua_t_ddraw_surface", argc, argv);

    g_hwnd = CreateWindowExA(0, "STATIC", "ddraw", 0, 0, 0, 8, 8, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    t_check("dll-load", create_ddraw(), "err=%lu", GetLastError());
    if (!g_dd)
        return t_finish();
    TCHECK("cooperative-level", IDirectDraw7_SetCooperativeLevel(g_dd, g_hwnd, DDSCL_NORMAL) == DD_OK);

    /* --- 32bpp 离屏图案 Blt 往返 --- */
    TCHECK("create-src-32bpp", create_offscreen(&src, 32, 16, 0));
    TCHECK("create-dst-32bpp", create_offscreen(&dst, 32, 16, 0));
    if (src && dst)
    {
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        hr = IDirectDrawSurface7_Lock(src, NULL, &desc, DDLOCK_WAIT, NULL);
        TCHECK("lock-src", hr == DD_OK);
        if (hr == DD_OK)
        {
            int x, y;
            DWORD *row;
            for (y = 0; y < 16; ++y)
            {
                row = (DWORD *)((char *)desc.lpSurface + y * desc.lPitch);
                for (x = 0; x < 32; ++x)
                    row[x] = (DWORD)((y << 20) | (x << 2) | 0xFF000000u);
            }
            IDirectDrawSurface7_Unlock(src, NULL);

            /* 目标先 colorfill 成反色基准色 */
            {
                DDBLTFX fx;
                memset(&fx, 0, sizeof(fx));
                fx.dwSize = sizeof(fx);
                fx.dwFillColor = 0x00123456;
                IDirectDrawSurface7_Blt(dst, NULL, NULL, NULL,
                                        DDBLT_COLORFILL | DDBLT_WAIT, &fx);
            }
            {
                RECT rc = {0, 0, 32, 16};
                TCHECK("blt", IDirectDrawSurface7_Blt(
                           dst, &rc, src, &rc, DDBLT_WAIT, NULL) == DD_OK);
            }

            memset(&desc, 0, sizeof(desc));
            desc.dwSize = sizeof(desc);
            hr = IDirectDrawSurface7_Lock(dst, NULL, &desc, DDLOCK_WAIT, NULL);
            TCHECK("lock-dst", hr == DD_OK);
            if (hr == DD_OK)
            {
                int x, y, bad = 0;
                for (y = 0; y < 16 && !bad; ++y)
                {
                    DWORD *row = (DWORD *)((char *)desc.lpSurface + y * desc.lPitch);
                    for (x = 0; x < 32; ++x)
                        if ((row[x] & 0x00FFFFFF) != (DWORD)((y << 20) | (x << 2)))
                        { bad = y * 32 + x + 1; break; }
                }
                t_check("blt-roundtrip", bad == 0, "first_bad_px=%d", bad);
                IDirectDrawSurface7_Unlock(dst, NULL);
            }
        }
    }

    /* --- 8bpp 调色板表面索引渲染 --- */
    TCHECK("create-pal-8bpp", create_offscreen(&pal_surf, 8, 4, 8));
    if (pal_surf)
    {
        LPDIRECTDRAWPALETTE pal = NULL;
        PALETTEENTRY entries[256];
        memset(entries, 0, sizeof(entries));
        entries[1].peRed = 0x33; entries[1].peGreen = 0x66; entries[1].peBlue = 0x99;
        entries[2].peRed = 0xCC; entries[2].peGreen = 0x88; entries[2].peBlue = 0x44;
        TCHECK("create-palette", IDirectDraw7_CreatePalette(g_dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
                                          entries, &pal, NULL) == DD_OK && pal);
        if (pal)
        {
            HRESULT hrsp = IDirectDrawSurface7_SetPalette(pal_surf, pal);
            t_check("set-palette", hrsp == DD_OK, "hr=0x%08lx",
                    (unsigned long)hrsp);
            memset(&desc, 0, sizeof(desc));
            desc.dwSize = sizeof(desc);
            if (IDirectDrawSurface7_Lock(pal_surf, NULL, &desc, DDLOCK_WAIT, NULL) == DD_OK)
            {
                int y;
                for (y = 0; y < 4; ++y)
                {
                    unsigned char *row =
                        (unsigned char *)((char *)desc.lpSurface + y * desc.lPitch);
                    row[0] = 1; row[1] = 2; row[2] = 1; row[3] = 2;
                }
                IDirectDrawSurface7_Unlock(pal_surf, NULL);

                /* 索引在表面内存里原样存取（读回校验表面管理） */
                memset(&desc, 0, sizeof(desc));
                desc.dwSize = sizeof(desc);
                if (IDirectDrawSurface7_Lock(pal_surf, NULL, &desc, DDLOCK_WAIT, NULL) == DD_OK)
                {
                    unsigned char *row =
                        (unsigned char *)((char *)desc.lpSurface + 2 * desc.lPitch);
                    t_check("pal-index-roundtrip",
                            row[0] == 1 && row[1] == 2,
                            "idx=(%u,%u)", row[0], row[1]);
                    IDirectDrawSurface7_Unlock(pal_surf, NULL);
                }
            }
            /* 调色板条目读回（翻译表往返） */
            {
                PALETTEENTRY back[3];
                memset(back, 0, sizeof(back));
                TCHECK("palette-getentries", IDirectDrawPalette_GetEntries(pal, 0, 0, 3, back) == DD_OK);
                t_check("palette-entries-roundtrip",
                        back[1].peRed == 0x33 && back[2].peBlue == 0x44,
                        "(%02x%02x%02x)(%02x%02x%02x)",
                        back[1].peRed, back[1].peGreen, back[1].peBlue,
                        back[2].peRed, back[2].peGreen, back[2].peBlue);
            }
            IDirectDrawPalette_Release(pal);
        }
        IDirectDrawSurface7_Release(pal_surf);
    }
    if (dst) IDirectDrawSurface7_Release(dst);
    if (src) IDirectDrawSurface7_Release(src);

    /* --- 主表面+后备缓冲 Flip（API 往返；像素受缺口②限不做） --- */
    {
        DDSURFACEDESC2 prim;
        LPDIRECTDRAWSURFACE7 prim_surf = NULL, back = NULL;
        memset(&prim, 0, sizeof(prim));
        prim.dwSize = sizeof(prim);
        prim.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        prim.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                              DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
        prim.dwBackBufferCount = 1;
        hr = IDirectDraw7_CreateSurface(g_dd, &prim, &prim_surf, NULL);
        /* DDSCL_NORMAL 下 FLIP 复杂主表面按语义应被拒（独占模式专属）：
         * 返回 DDERR_NOEXCLUSIVEMODE = ddraw 正确执行了模式检查 */
        t_check("flip-mode-check", hr == DDERR_NOEXCLUSIVEMODE,
                "hr=0x%08lx", (unsigned long)hr);
        if (hr == DD_OK && prim_surf)
        {
            DDSCAPS2 caps;
            memset(&caps, 0, sizeof(caps));
            caps.dwCaps = DDSCAPS_BACKBUFFER;
            hr = IDirectDrawSurface7_GetAttachedSurface(prim_surf, &caps, &back);
            t_check("get-backbuffer", hr == DD_OK && back != NULL,
                    "hr=0x%08lx", (unsigned long)hr);
            if (back)
            {
                /* SYSTEMMEMORY 主表面不支持 Flip（DDERR_NOEXCLUSIVEMODE/
                 * 不支持路径），Flip 断言以「返回明确 DDERR 语义」为过——
                 * 独占模式 Flip 链由 ddraw_flip 全屏档另行覆盖 */
                hr = IDirectDrawSurface7_Flip(prim_surf, NULL, DDFLIP_WAIT);
                t_check("flip-returns-defined", hr == DD_OK ||
                        hr == DDERR_NOEXCLUSIVEMODE || hr == DDERR_NOTFLIPPABLE ||
                        hr == DDERR_SURFACEBUSY,
                        "hr=0x%08lx", (unsigned long)hr);
                IDirectDrawSurface7_Release(back);
            }
            IDirectDrawSurface7_Release(prim_surf);
        }
    }

    t_metric("ddraw-result", "done");
    if (g_dd) IDirectDraw7_Release(g_dd);
    if (g_ddraw) FreeLibrary(g_ddraw);
    DestroyWindow(g_hwnd);
    return t_finish();
}

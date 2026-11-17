#define COBJMACROS

#include <windows.h>
#include <d3d8.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "winehua_smoke_protocol.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct device_case_result
{
    HRESULT create;
    HRESULT texture;
    HRESULT clear;
    HRESULT draw;
    HRESULT present;
};

struct probe_state
{
    struct winehua_smoke_options options;
    HWND window;
    HMODULE d3d8_module;
    IDirect3D8 *d3d;
    D3DADAPTER_IDENTIFIER8 identifier;
    D3DDISPLAYMODE current_mode;
    D3DCAPS8 caps;
    UINT adapter_count;
    UINT mode_count;
    BOOL mode_640_480_16;
    BOOL mode_640_480_32;
    BOOL mode_800_600_16;
    BOOL mode_800_600_32;
    BOOL mode_1024_768_16;
    BOOL mode_1024_768_32;
    BOOL mode_1280_800_16;
    BOOL mode_1280_800_32;
    HRESULT check_windowed_32;
    HRESULT check_windowed_16;
    HRESULT check_fullscreen_32;
    HRESULT check_fullscreen_16;
    HRESULT format_r5g6b5;
    HRESULT format_a1r5g5b5;
    HRESULT format_a4r4g4b4;
    HRESULT format_d16;
    HRESULT format_d24s8;
    HRESULT format_dxt1;
    HRESULT format_dxt3;
    HRESULT format_dxt5;
    HRESULT depth_x8_d16;
    HRESULT depth_x8_d24s8;
    HRESULT depth_r5_d16;
    struct device_case_result windowed_sw;
    struct device_case_result windowed_hw;
    struct device_case_result windowed_mixed;
    struct device_case_result fullscreen_32;
    struct device_case_result fullscreen_wide_32;
    struct device_case_result fullscreen_16;
    UINT failures;
    char first_stage[64];
    char first_message[256];
};

struct smoke_vertex
{
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

typedef IDirect3D8 *(WINAPI *direct3d_create8_fn)(UINT sdk_version);

static const char *format_name(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    default: return "OTHER";
    }
}

static void record_failure(struct probe_state *state, const char *stage, const char *format, ...)
{
    va_list args;

    ++state->failures;
    if (state->first_stage[0]) return;
    lstrcpynA(state->first_stage, stage, (int)sizeof(state->first_stage));
    va_start(args, format);
    vsnprintf(state->first_message, sizeof(state->first_message), format, args);
    va_end(args);
}

static BOOL require_hr(struct probe_state *state, const char *stage, const char *name, HRESULT hr)
{
    fprintf(stderr, "[%s] %s hr=0x%08lx\n", SUCCEEDED(hr) ? "PASS" : "FAIL", name,
            (unsigned long)hr);
    if (SUCCEEDED(hr)) return TRUE;
    record_failure(state, stage, "%s failed with HRESULT 0x%08lx", name, (unsigned long)hr);
    return FALSE;
}

static void require_capability_result(struct probe_state *state, const char *stage,
        const char *name, HRESULT hr)
{
    BOOL valid = SUCCEEDED(hr) || hr == D3DERR_NOTAVAILABLE;

    fprintf(stderr, "[%s] %s hr=0x%08lx\n", valid ? "PASS" : "FAIL", name,
            (unsigned long)hr);
    if (!valid)
        record_failure(state, stage, "%s returned unexpected HRESULT 0x%08lx",
                name, (unsigned long)hr);
}

static void require_condition(struct probe_state *state, const char *stage, const char *name, BOOL value)
{
    fprintf(stderr, "[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) record_failure(state, stage, "%s", name);
}

static BOOL is_16_bit_mode(D3DFORMAT format)
{
    return format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5;
}

static BOOL is_32_bit_mode(D3DFORMAT format)
{
    return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8;
}

static void remember_mode(struct probe_state *state, const D3DDISPLAYMODE *mode)
{
    BOOL depth16 = is_16_bit_mode(mode->Format);
    BOOL depth32 = is_32_bit_mode(mode->Format);

    if (mode->Width == 640 && mode->Height == 480)
    {
        state->mode_640_480_16 |= depth16;
        state->mode_640_480_32 |= depth32;
    }
    else if (mode->Width == 800 && mode->Height == 600)
    {
        state->mode_800_600_16 |= depth16;
        state->mode_800_600_32 |= depth32;
    }
    else if (mode->Width == 1024 && mode->Height == 768)
    {
        state->mode_1024_768_16 |= depth16;
        state->mode_1024_768_32 |= depth32;
    }
    else if (mode->Width == 1280 && mode->Height == 800)
    {
        state->mode_1280_800_16 |= depth16;
        state->mode_1280_800_32 |= depth32;
    }
}

static void enumerate_modes(struct probe_state *state)
{
    UINT i;

    state->mode_count = IDirect3D8_GetAdapterModeCount(state->d3d, D3DADAPTER_DEFAULT);
    fprintf(stderr, "D3D8 adapter mode count: %u\n", state->mode_count);
    for (i = 0; i < state->mode_count; ++i)
    {
        D3DDISPLAYMODE mode;
        HRESULT hr = IDirect3D8_EnumAdapterModes(state->d3d, D3DADAPTER_DEFAULT, i, &mode);
        if (FAILED(hr))
        {
            record_failure(state, "mode-enumeration", "EnumAdapterModes(%u) failed with HRESULT 0x%08lx",
                    i, (unsigned long)hr);
            continue;
        }
        fprintf(stderr, "  mode[%u]=%ux%u@%u %s\n", i, mode.Width, mode.Height,
                mode.RefreshRate, format_name(mode.Format));
        remember_mode(state, &mode);
    }

    require_condition(state, "mode-enumeration", "640x480 16-bit mode is available", state->mode_640_480_16);
    require_condition(state, "mode-enumeration", "640x480 32-bit mode is available", state->mode_640_480_32);
    require_condition(state, "mode-enumeration", "800x600 16-bit mode is available", state->mode_800_600_16);
    require_condition(state, "mode-enumeration", "800x600 32-bit mode is available", state->mode_800_600_32);
    require_condition(state, "mode-enumeration", "1024x768 16-bit mode is available", state->mode_1024_768_16);
    require_condition(state, "mode-enumeration", "1024x768 32-bit mode is available", state->mode_1024_768_32);
    require_condition(state, "mode-enumeration", "1280x800 16-bit mode is available", state->mode_1280_800_16);
    require_condition(state, "mode-enumeration", "1280x800 32-bit mode is available", state->mode_1280_800_32);
}

static HRESULT create_test_texture(IDirect3DDevice8 *device, IDirect3DTexture8 **texture)
{
    D3DLOCKED_RECT locked;
    HRESULT hr;
    UINT x, y;

    *texture = NULL;
    hr = IDirect3DDevice8_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, texture);
    if (FAILED(hr)) return hr;
    hr = IDirect3DTexture8_LockRect(*texture, 0, &locked, NULL, 0);
    if (FAILED(hr)) return hr;
    for (y = 0; y < 2; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
        for (x = 0; x < 2; ++x)
            row[x] = (x ^ y) ? 0xff40c0ff : 0xffffc040;
    }
    return IDirect3DTexture8_UnlockRect(*texture, 0);
}

static HRESULT render_test_frame(IDirect3DDevice8 *device, UINT width, UINT height,
        HRESULT *texture_hr, HRESULT *clear_hr, HRESULT *draw_hr, HRESULT *present_hr)
{
    struct smoke_vertex vertices[3];
    IDirect3DTexture8 *texture = NULL;
    HRESULT hr, end_hr;

    vertices[0] = (struct smoke_vertex){width * 0.50f, height * 0.12f, 0.5f, 1.0f, 0xffff4040, 0.5f, 0.0f};
    vertices[1] = (struct smoke_vertex){width * 0.12f, height * 0.85f, 0.5f, 1.0f, 0xff40ff40, 0.0f, 1.0f};
    vertices[2] = (struct smoke_vertex){width * 0.88f, height * 0.85f, 0.5f, 1.0f, 0xff4040ff, 1.0f, 1.0f};

    *texture_hr = create_test_texture(device, &texture);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)texture);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    *clear_hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(24, 36, 56), 1.0f, 0);
    hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                vertices, sizeof(vertices[0]));
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr)) hr = end_hr;
    }
    *draw_hr = hr;
    *present_hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);

    IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (texture) IDirect3DTexture8_Release(texture);
    if (FAILED(*texture_hr)) return *texture_hr;
    if (FAILED(*clear_hr)) return *clear_hr;
    if (FAILED(*draw_hr)) return *draw_hr;
    return *present_hr;
}

static void run_device_case(struct probe_state *state, const char *name, BOOL windowed,
        UINT width, UINT height, D3DFORMAT format, DWORD behavior,
        struct device_case_result *result)
{
    D3DPRESENT_PARAMETERS parameters;
    IDirect3DDevice8 *device = NULL;
    LONG_PTR original_style;
    HRESULT render_hr;

    memset(result, 0, sizeof(*result));
    result->create = result->texture = result->clear = result->draw = result->present = E_FAIL;
    original_style = GetWindowLongPtrA(state->window, GWL_STYLE);
    SetWindowLongPtrA(state->window, GWL_STYLE, windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP);
    SetWindowPos(state->window, HWND_TOP, 0, 0, width, height,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | (windowed ? SWP_NOMOVE : 0));

    memset(&parameters, 0, sizeof(parameters));
    parameters.BackBufferWidth = width;
    parameters.BackBufferHeight = height;
    parameters.BackBufferFormat = format;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = state->window;
    parameters.Windowed = windowed;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D16;
    parameters.FullScreen_RefreshRateInHz = 0;
    parameters.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    result->create = IDirect3D8_CreateDevice(state->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            state->window, behavior, &parameters, &device);
    if (SUCCEEDED(result->create))
    {
        render_hr = render_test_frame(device, width, height, &result->texture,
                &result->clear, &result->draw, &result->present);
        if (FAILED(render_hr))
            record_failure(state, "render", "%s rendering failed with HRESULT 0x%08lx",
                    name, (unsigned long)render_hr);
        IDirect3DDevice8_Release(device);
    }
    else
    {
        record_failure(state, "create-device", "%s CreateDevice failed with HRESULT 0x%08lx",
                name, (unsigned long)result->create);
    }

    fprintf(stderr, "[%s] %s create=0x%08lx texture=0x%08lx clear=0x%08lx draw=0x%08lx present=0x%08lx\n",
            SUCCEEDED(result->create) && SUCCEEDED(result->texture) && SUCCEEDED(result->clear) &&
            SUCCEEDED(result->draw) && SUCCEEDED(result->present) ? "PASS" : "FAIL", name,
            (unsigned long)result->create, (unsigned long)result->texture, (unsigned long)result->clear,
            (unsigned long)result->draw, (unsigned long)result->present);

    if (!windowed) ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
    SetWindowLongPtrA(state->window, GWL_STYLE, original_style);
    SetWindowPos(state->window, NULL, 80, 80, 640, 480,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
}

static void run_capability_checks(struct probe_state *state)
{
    D3DFORMAT adapter_format = state->current_mode.Format;

    state->check_windowed_32 = IDirect3D8_CheckDeviceType(state->d3d, 0, D3DDEVTYPE_HAL,
            adapter_format, D3DFMT_X8R8G8B8, TRUE);
    state->check_windowed_16 = IDirect3D8_CheckDeviceType(state->d3d, 0, D3DDEVTYPE_HAL,
            adapter_format, D3DFMT_R5G6B5, TRUE);
    state->check_fullscreen_32 = IDirect3D8_CheckDeviceType(state->d3d, 0, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, FALSE);
    state->check_fullscreen_16 = IDirect3D8_CheckDeviceType(state->d3d, 0, D3DDEVTYPE_HAL,
            D3DFMT_R5G6B5, D3DFMT_R5G6B5, FALSE);
    require_hr(state, "check-device-type", "CheckDeviceType windowed X8R8G8B8", state->check_windowed_32);
    /* A 32-bit desktop may legitimately reject a 16-bit windowed backbuffer
     * when the driver does not advertise present conversion. Fullscreen
     * R5G6B5 below remains a required compatibility path. */
    require_capability_result(state, "check-device-type",
            "CheckDeviceType windowed R5G6B5", state->check_windowed_16);
    require_hr(state, "check-device-type", "CheckDeviceType fullscreen X8R8G8B8", state->check_fullscreen_32);
    require_hr(state, "check-device-type", "CheckDeviceType fullscreen R5G6B5", state->check_fullscreen_16);

#define CHECK_TEXTURE(member, label, value) \
    do { \
        state->member = IDirect3D8_CheckDeviceFormat(state->d3d, 0, D3DDEVTYPE_HAL, adapter_format, \
                0, D3DRTYPE_TEXTURE, value); \
        require_hr(state, "format", label, state->member); \
    } while (0)
    CHECK_TEXTURE(format_r5g6b5, "Texture R5G6B5", D3DFMT_R5G6B5);
    CHECK_TEXTURE(format_a1r5g5b5, "Texture A1R5G5B5", D3DFMT_A1R5G5B5);
    CHECK_TEXTURE(format_a4r4g4b4, "Texture A4R4G4B4", D3DFMT_A4R4G4B4);
    CHECK_TEXTURE(format_dxt1, "Texture DXT1", D3DFMT_DXT1);
    CHECK_TEXTURE(format_dxt3, "Texture DXT3", D3DFMT_DXT3);
    CHECK_TEXTURE(format_dxt5, "Texture DXT5", D3DFMT_DXT5);
#undef CHECK_TEXTURE

    state->format_d16 = IDirect3D8_CheckDeviceFormat(state->d3d, 0, D3DDEVTYPE_HAL, adapter_format,
            D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D16);
    state->format_d24s8 = IDirect3D8_CheckDeviceFormat(state->d3d, 0, D3DDEVTYPE_HAL, adapter_format,
            D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8);
    require_hr(state, "format", "Depth D16", state->format_d16);
    require_hr(state, "format", "Depth D24S8", state->format_d24s8);

    state->depth_x8_d16 = IDirect3D8_CheckDepthStencilMatch(state->d3d, 0, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D16);
    state->depth_x8_d24s8 = IDirect3D8_CheckDepthStencilMatch(state->d3d, 0, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8);
    state->depth_r5_d16 = IDirect3D8_CheckDepthStencilMatch(state->d3d, 0, D3DDEVTYPE_HAL,
            D3DFMT_R5G6B5, D3DFMT_R5G6B5, D3DFMT_D16);
    require_hr(state, "depth-stencil", "X8R8G8B8 + D16", state->depth_x8_d16);
    require_hr(state, "depth-stencil", "X8R8G8B8 + D24S8", state->depth_x8_d24s8);
    require_hr(state, "depth-stencil", "R5G6B5 + D16", state->depth_r5_d16);
}

static void write_result(struct probe_state *state)
{
    char metrics[8192];
    const char *status = state->failures ? "FAIL" : "PASS";
    const char *stage = state->failures ? state->first_stage : "complete";
    const char *message = state->failures ? state->first_message :
            "D3D8 modes, capabilities, formats, device creation and rendering passed";

    snprintf(metrics, sizeof(metrics),
            "{\"adapterCount\":%u,\"modeCount\":%u,"
            "\"currentMode\":{\"width\":%u,\"height\":%u,\"refresh\":%u,\"format\":%u},"
            "\"adapter\":{\"vendorId\":%lu,\"deviceId\":%lu},"
            "\"modes\":{\"640x480_16\":%s,\"640x480_32\":%s,"
            "\"800x600_16\":%s,\"800x600_32\":%s,"
            "\"1024x768_16\":%s,\"1024x768_32\":%s,"
            "\"1280x800_16\":%s,\"1280x800_32\":%s},"
            "\"caps\":{\"hwTnl\":%s,\"maxTextureWidth\":%lu,\"maxTextureHeight\":%lu,"
            "\"maxTextureBlendStages\":%lu,\"maxSimultaneousTextures\":%lu,"
            "\"vertexShaderVersion\":%lu,\"pixelShaderVersion\":%lu},"
            "\"checks\":{\"windowed32\":\"0x%08lx\",\"windowed16\":\"0x%08lx\","
            "\"fullscreen32\":\"0x%08lx\",\"fullscreen16\":\"0x%08lx\","
            "\"r5g6b5\":\"0x%08lx\",\"a1r5g5b5\":\"0x%08lx\","
            "\"a4r4g4b4\":\"0x%08lx\",\"d16\":\"0x%08lx\","
            "\"d24s8\":\"0x%08lx\",\"dxt1\":\"0x%08lx\","
            "\"dxt3\":\"0x%08lx\",\"dxt5\":\"0x%08lx\","
            "\"depthX8D16\":\"0x%08lx\",\"depthX8D24S8\":\"0x%08lx\","
            "\"depthR5D16\":\"0x%08lx\"},"
            "\"devices\":{"
            "\"windowedSw\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"},"
            "\"windowedHw\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"},"
            "\"windowedMixed\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"},"
            "\"fullscreen1024x768x32\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"},"
            "\"fullscreen1280x800x32\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"},"
            "\"fullscreen800x600x16\":{\"create\":\"0x%08lx\",\"texture\":\"0x%08lx\","
            "\"clear\":\"0x%08lx\",\"draw\":\"0x%08lx\",\"present\":\"0x%08lx\"}},"
            "\"failureCount\":%u}",
            state->adapter_count, state->mode_count,
            state->current_mode.Width, state->current_mode.Height, state->current_mode.RefreshRate,
            (unsigned int)state->current_mode.Format,
            (unsigned long)state->identifier.VendorId, (unsigned long)state->identifier.DeviceId,
            state->mode_640_480_16 ? "true" : "false", state->mode_640_480_32 ? "true" : "false",
            state->mode_800_600_16 ? "true" : "false", state->mode_800_600_32 ? "true" : "false",
            state->mode_1024_768_16 ? "true" : "false", state->mode_1024_768_32 ? "true" : "false",
            state->mode_1280_800_16 ? "true" : "false", state->mode_1280_800_32 ? "true" : "false",
            state->caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT ? "true" : "false",
            (unsigned long)state->caps.MaxTextureWidth, (unsigned long)state->caps.MaxTextureHeight,
            (unsigned long)state->caps.MaxTextureBlendStages,
            (unsigned long)state->caps.MaxSimultaneousTextures,
            (unsigned long)state->caps.VertexShaderVersion, (unsigned long)state->caps.PixelShaderVersion,
            (unsigned long)state->check_windowed_32, (unsigned long)state->check_windowed_16,
            (unsigned long)state->check_fullscreen_32, (unsigned long)state->check_fullscreen_16,
            (unsigned long)state->format_r5g6b5, (unsigned long)state->format_a1r5g5b5,
            (unsigned long)state->format_a4r4g4b4, (unsigned long)state->format_d16,
            (unsigned long)state->format_d24s8, (unsigned long)state->format_dxt1,
            (unsigned long)state->format_dxt3, (unsigned long)state->format_dxt5,
            (unsigned long)state->depth_x8_d16, (unsigned long)state->depth_x8_d24s8,
            (unsigned long)state->depth_r5_d16,
            (unsigned long)state->windowed_sw.create, (unsigned long)state->windowed_sw.texture,
            (unsigned long)state->windowed_sw.clear, (unsigned long)state->windowed_sw.draw,
            (unsigned long)state->windowed_sw.present,
            (unsigned long)state->windowed_hw.create, (unsigned long)state->windowed_hw.texture,
            (unsigned long)state->windowed_hw.clear, (unsigned long)state->windowed_hw.draw,
            (unsigned long)state->windowed_hw.present,
            (unsigned long)state->windowed_mixed.create, (unsigned long)state->windowed_mixed.texture,
            (unsigned long)state->windowed_mixed.clear, (unsigned long)state->windowed_mixed.draw,
            (unsigned long)state->windowed_mixed.present,
            (unsigned long)state->fullscreen_32.create, (unsigned long)state->fullscreen_32.texture,
            (unsigned long)state->fullscreen_32.clear, (unsigned long)state->fullscreen_32.draw,
            (unsigned long)state->fullscreen_32.present,
            (unsigned long)state->fullscreen_wide_32.create, (unsigned long)state->fullscreen_wide_32.texture,
            (unsigned long)state->fullscreen_wide_32.clear, (unsigned long)state->fullscreen_wide_32.draw,
            (unsigned long)state->fullscreen_wide_32.present,
            (unsigned long)state->fullscreen_16.create, (unsigned long)state->fullscreen_16.texture,
            (unsigned long)state->fullscreen_16.clear, (unsigned long)state->fullscreen_16.draw,
            (unsigned long)state->fullscreen_16.present,
            state->failures);
    winehua_smoke_write_result(&state->options, status, stage, message, metrics);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int main(int argc, char **argv)
{
    struct probe_state state;
    WNDCLASSA window_class;
    direct3d_create8_fn create_d3d8;
    FARPROC create_d3d8_proc;
    HRESULT hr;

    memset(&state, 0, sizeof(state));
    if (!winehua_smoke_parse_options(&state.options, argc, argv, 5)) return 2;
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "WineHuaD3D8SmokeWindow";
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        record_failure(&state, "window", "RegisterClass failed with error %lu", GetLastError());
        write_result(&state);
        return 1;
    }
    state.window = CreateWindowExA(0, window_class.lpszClassName, "WineHua D3D8 smoke",
            WS_OVERLAPPEDWINDOW, 80, 80, 640, 480, NULL, NULL, window_class.hInstance, NULL);
    if (!state.window)
    {
        record_failure(&state, "window", "CreateWindow failed with error %lu", GetLastError());
        write_result(&state);
        return 1;
    }
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);

    state.d3d8_module = LoadLibraryA("d3d8.dll");
    require_condition(&state, "dll-load", "d3d8.dll loaded", state.d3d8_module != NULL);
    create_d3d8 = NULL;
    create_d3d8_proc = state.d3d8_module ? GetProcAddress(state.d3d8_module, "Direct3DCreate8") : NULL;
    if (create_d3d8_proc) memcpy(&create_d3d8, &create_d3d8_proc, sizeof(create_d3d8));
    require_condition(&state, "dll-load", "Direct3DCreate8 export is available", create_d3d8 != NULL);
    state.d3d = create_d3d8 ? create_d3d8(D3D_SDK_VERSION) : NULL;
    require_condition(&state, "direct3d-create", "Direct3DCreate8 returned an interface", state.d3d != NULL);
    if (!state.d3d)
    {
        write_result(&state);
        if (state.d3d8_module) FreeLibrary(state.d3d8_module);
        DestroyWindow(state.window);
        UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    state.adapter_count = IDirect3D8_GetAdapterCount(state.d3d);
    require_condition(&state, "adapter", "At least one D3D8 adapter is available", state.adapter_count > 0);
    hr = IDirect3D8_GetAdapterIdentifier(state.d3d, D3DADAPTER_DEFAULT, 0, &state.identifier);
    require_hr(&state, "adapter", "GetAdapterIdentifier", hr);
    hr = IDirect3D8_GetAdapterDisplayMode(state.d3d, D3DADAPTER_DEFAULT, &state.current_mode);
    if (!require_hr(&state, "adapter", "GetAdapterDisplayMode", hr)) goto done;
    fprintf(stderr, "Current D3D8 mode: %ux%u@%u %s\n", state.current_mode.Width,
            state.current_mode.Height, state.current_mode.RefreshRate, format_name(state.current_mode.Format));

    enumerate_modes(&state);
    hr = IDirect3D8_GetDeviceCaps(state.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &state.caps);
    require_hr(&state, "caps", "GetDeviceCaps", hr);
    require_condition(&state, "caps", "D3DDEVCAPS_HWTRANSFORMANDLIGHT is reported",
            !!(state.caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT));
    run_capability_checks(&state);

    run_device_case(&state, "windowed SWVP", TRUE, 640, 480, state.current_mode.Format,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &state.windowed_sw);
    run_device_case(&state, "windowed HWVP", TRUE, 640, 480, state.current_mode.Format,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &state.windowed_hw);
    run_device_case(&state, "windowed mixed VP", TRUE, 640, 480, state.current_mode.Format,
            D3DCREATE_MIXED_VERTEXPROCESSING, &state.windowed_mixed);
    run_device_case(&state, "fullscreen 1024x768 X8R8G8B8", FALSE, 1024, 768, D3DFMT_X8R8G8B8,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &state.fullscreen_32);
    run_device_case(&state, "fullscreen 1280x800 X8R8G8B8", FALSE, 1280, 800, D3DFMT_X8R8G8B8,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &state.fullscreen_wide_32);
    run_device_case(&state, "fullscreen 800x600 R5G6B5", FALSE, 800, 600, D3DFMT_R5G6B5,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &state.fullscreen_16);

done:
    write_result(&state);
    IDirect3D8_Release(state.d3d);
    FreeLibrary(state.d3d8_module);
    DestroyWindow(state.window);
    UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
    return state.failures ? 1 : 0;
}

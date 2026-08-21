#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef enum RendererKind {
    RENDERER_D3D9 = 0,
    RENDERER_D3D11 = 1
} RendererKind;

typedef struct Mat4 {
    float m[4][4];
} Mat4;

typedef struct D3D9Vertex {
    float x, y, z;
    DWORD color;
} D3D9Vertex;

typedef struct D3D11Vertex {
    float x, y, z;
    float r, g, b, a;
} D3D11Vertex;

typedef struct D3D9State {
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    IDirect3DVertexBuffer9 *vb;
    IDirect3DIndexBuffer9 *ib;
} D3D9State;

typedef struct D3D11State {
    HMODULE d3dcompiler;
    D3D_FEATURE_LEVEL feature_level;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swap_chain;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *depth_texture;
    ID3D11DepthStencilView *dsv;
    ID3D11Buffer *vb;
    ID3D11Buffer *ib;
    ID3D11Buffer *cb;
    ID3D11InputLayout *layout;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11DepthStencilState *depth_state;
    ID3D11RasterizerState *raster_state;
} D3D11State;

typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID src_data, SIZE_T src_data_size,
                                       LPCSTR source_name,
                                       const D3D_SHADER_MACRO *defines,
                                       ID3DInclude *include,
                                       LPCSTR entrypoint, LPCSTR target,
                                       UINT flags1, UINT flags2,
                                       ID3DBlob **code,
                                       ID3DBlob **error_msgs);

typedef struct AppState {
    HWND hwnd;
    int width;
    int height;
    int paused;
    int need_recreate;
    int running;
    RendererKind renderer;
    D3D9State d3d9;
    D3D11State d3d11;
    LARGE_INTEGER qpc_freq;
    LARGE_INTEGER start_qpc;
    double last_title_time;
    unsigned int frame_count;
    unsigned int total_frame_count;
    unsigned int render_sequence;
    unsigned int angle_regression_count;
    double last_render_angle;
    int renderer_ready;
    int automation;
    DWORD duration_ms;
    ULONGLONG run_start_ms;
    HRESULT init_result;
    HRESULT present_result;
    char run_id[96];
    char test_id[96];
    char result_path[MAX_PATH];
    char status[256];
} AppState;

static AppState g_app;

static const char *feature_level_name(D3D_FEATURE_LEVEL level);

static const WORD g_indices[] = {
    0, 1, 2, 0, 2, 3,
    4, 6, 5, 4, 7, 6,
    4, 5, 1, 4, 1, 0,
    3, 2, 6, 3, 6, 7,
    1, 5, 6, 1, 6, 2,
    4, 0, 3, 4, 3, 7
};

static const WORD g_d3d11_indices[] = {
    0, 1, 2, 0, 2, 3,
    4, 6, 5, 4, 7, 6,
    4, 5, 1, 4, 1, 0,
    3, 2, 6, 3, 6, 7,
    1, 5, 6, 1, 6, 2,
    4, 0, 3, 4, 3, 7,
    8, 9, 10, 8, 10, 11
};

static const D3D9Vertex g_d3d9_vertices[] = {
    {-1.0f, -1.0f, -1.0f, 0xffff4040},
    { 1.0f, -1.0f, -1.0f, 0xff40ff40},
    { 1.0f,  1.0f, -1.0f, 0xff4040ff},
    {-1.0f,  1.0f, -1.0f, 0xffffff40},
    {-1.0f, -1.0f,  1.0f, 0xffff40ff},
    { 1.0f, -1.0f,  1.0f, 0xff40ffff},
    { 1.0f,  1.0f,  1.0f, 0xffffffff},
    {-1.0f,  1.0f,  1.0f, 0xffff8040}
};

static const D3D11Vertex g_d3d11_vertices[] = {
    {-1.0f, -1.0f, -1.0f, 1.00f, 0.25f, 0.25f, 1.0f},
    { 1.0f, -1.0f, -1.0f, 0.25f, 1.00f, 0.25f, 1.0f},
    { 1.0f,  1.0f, -1.0f, 0.25f, 0.25f, 1.00f, 1.0f},
    {-1.0f,  1.0f, -1.0f, 1.00f, 1.00f, 0.25f, 1.0f},
    {-1.0f, -1.0f,  1.0f, 1.00f, 0.25f, 1.00f, 1.0f},
    { 1.0f, -1.0f,  1.0f, 0.25f, 1.00f, 1.00f, 1.0f},
    { 1.0f,  1.0f,  1.0f, 1.00f, 1.00f, 1.00f, 1.0f},
    {-1.0f,  1.0f,  1.0f, 1.00f, 0.50f, 0.25f, 1.0f}
};

static Mat4 mat4_identity(void)
{
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][3] = 1.0f;
    return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    int i, j, k;
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            for (k = 0; k < 4; ++k) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

static Mat4 mat4_rotation_x(float angle)
{
    Mat4 r = mat4_identity();
    float c = cosf(angle);
    float s = sinf(angle);
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

static Mat4 mat4_rotation_y(float angle)
{
    Mat4 r = mat4_identity();
    float c = cosf(angle);
    float s = sinf(angle);
    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
}

static Mat4 mat4_translation(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[3][0] = x;
    r.m[3][1] = y;
    r.m[3][2] = z;
    return r;
}

static Mat4 mat4_perspective_lh(float fovy, float aspect, float zn, float zf)
{
    Mat4 r;
    float y_scale = 1.0f / tanf(fovy * 0.5f);
    float x_scale = y_scale / aspect;
    memset(&r, 0, sizeof(r));
    r.m[0][0] = x_scale;
    r.m[1][1] = y_scale;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1.0f;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

static void transform_to_ndc(const D3D11Vertex *src, D3D11Vertex *dst, size_t count, Mat4 mvp)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        float x = src[i].x;
        float y = src[i].y;
        float z = src[i].z;
        float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
        float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
        float cz = x * mvp.m[0][2] + y * mvp.m[1][2] + z * mvp.m[2][2] + mvp.m[3][2];
        float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
        if (fabsf(cw) < 0.00001f) {
            cw = 1.0f;
        }
        dst[i] = src[i];
        dst[i].x = cx / cw;
        dst[i].y = cy / cw;
        dst[i].z = cz / cw;
    }
}

static double app_time_seconds(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_app.start_qpc.QuadPart) /
           (double)g_app.qpc_freq.QuadPart;
}

static const char *renderer_name(RendererKind renderer)
{
    return renderer == RENDERER_D3D11 ? "D3D11" : "D3D9";
}

static void app_log(const char *fmt, ...)
{
    FILE *fp;
    va_list args;
    fp = fopen("wined3d_switch_cube.log", "a");
    if (!fp) {
        return;
    }
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fputc('\n', fp);
    fclose(fp);
}

static void parse_command_line(RendererKind *initial_renderer)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int i;

    *initial_renderer = RENDERER_D3D11;
    if (!argv) return;
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--d3d9")) {
            *initial_renderer = RENDERER_D3D9;
        } else if (!wcscmp(argv[i], L"--d3d11")) {
            *initial_renderer = RENDERER_D3D11;
        } else if (!wcscmp(argv[i], L"--automation")) {
            g_app.automation = 1;
        } else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) {
            unsigned long seconds = wcstoul(argv[++i], NULL, 10);
            if (seconds > 3600) seconds = 3600;
            g_app.duration_ms = seconds * 1000;
        } else if (!wcscmp(argv[i], L"--result") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_app.result_path,
                                (int)sizeof(g_app.result_path), NULL, NULL);
        } else if (!wcscmp(argv[i], L"--run-id") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_app.run_id,
                                (int)sizeof(g_app.run_id), NULL, NULL);
        } else if (!wcscmp(argv[i], L"--test-id") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_app.test_id,
                                (int)sizeof(g_app.test_id), NULL, NULL);
        }
    }
    LocalFree(argv);
}

static const char *active_d3d_backend(void)
{
    const char *backend = getenv("WINEHUA_D3D_BACKEND");
    return backend && backend[0] ? backend : "unknown";
}

static void loaded_module_path(const char *module_name, char *path, size_t path_size)
{
    HMODULE module = GetModuleHandleA(module_name);
    DWORD length;
    DWORD i;

    if (!path_size) return;
    path[0] = '\0';
    if (!module) return;
    length = GetModuleFileNameA(module, path, (DWORD)path_size);
    if (!length || length >= path_size) path[0] = '\0';
    for (i = 0; i < length; ++i) {
        if (path[i] == '\\') path[i] = '/';
    }
}

static void write_automation_result(const char *status_override, const char *message_override)
{
    char temporary[MAX_PATH + 8];
    char d3d11_path[MAX_PATH];
    char dxgi_path[MAX_PATH];
    FILE *fp;
    const char *status = status_override ? status_override :
        (g_app.renderer_ready && SUCCEEDED(g_app.present_result) &&
        g_app.total_frame_count >= (g_app.automation ? 60u : 1u) ? "PASS" : "FAIL");
    const char *message = message_override ? message_override :
        (g_app.renderer_ready ? "cube rendered and presented" : g_app.status);

    if (!g_app.result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp", g_app.result_path);
    loaded_module_path("d3d11.dll", d3d11_path, sizeof(d3d11_path));
    loaded_module_path("dxgi.dll", dxgi_path, sizeof(dxgi_path));
    fp = fopen(temporary, "wb");
    if (!fp) return;
    fprintf(fp,
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"runId\": \"%s\",\n"
            "  \"testId\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"stage\": \"%s\",\n"
            "  \"message\": \"%s\",\n"
            "  \"renderer\": \"%s\",\n"
            "  \"d3dBackend\": \"%s\",\n"
            "  \"peArchitecture\": \"%s\",\n"
            "  \"featureLevel\": \"%s\",\n"
            "  \"initHresult\": \"0x%08lx\",\n"
            "  \"presentHresult\": \"0x%08lx\",\n"
            "  \"frames\": %u,\n"
            "  \"renderSequence\": %u,\n"
            "  \"angleRegressions\": %u,\n"
            "  \"lastAngle\": %.9f,\n"
            "  \"d3d11Dll\": \"%s\",\n"
            "  \"dxgiDll\": \"%s\"\n"
            "}\n",
            g_app.run_id, g_app.test_id, status,
            g_app.renderer_ready ? "present" : "initialization",
            message,
            renderer_name(g_app.renderer), active_d3d_backend(),
#ifdef _WIN64
            "x64",
#else
            "x86",
#endif
            g_app.renderer == RENDERER_D3D11
                ? feature_level_name(g_app.d3d11.feature_level) : "d3d9",
            (unsigned long)g_app.init_result,
            (unsigned long)g_app.present_result,
            g_app.total_frame_count, g_app.render_sequence,
            g_app.angle_regression_count, g_app.last_render_angle,
            d3d11_path, dxgi_path);
    fclose(fp);
    MoveFileExA(temporary, g_app.result_path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void set_status_hr(const char *where, HRESULT hr)
{
    snprintf(g_app.status, sizeof(g_app.status), "%s failed: HRESULT 0x%08lx",
             where, (unsigned long)hr);
    app_log("%s", g_app.status);
}

static void set_d3d11_stage_hr(const char *where, HRESULT hr)
{
    snprintf(g_app.status, sizeof(g_app.status), "D3D11 %s failed: HRESULT 0x%08lx",
             where, (unsigned long)hr);
    app_log("%s", g_app.status);
}

static void update_title(double now, int force)
{
    char title[384];
    double dt = now - g_app.last_title_time;
    if (!force && dt < 0.5) {
        return;
    }
    if (dt <= 0.0) {
        dt = 1.0;
    }
    snprintf(title, sizeof(title),
             "WineHua switch cube - %s / %s - FL %s - %.1f FPS - frame %u angle %.3f regress %u - 1/F1 D3D9, 2/F2 D3D11%s%s",
             renderer_name(g_app.renderer),
             active_d3d_backend(),
             g_app.renderer == RENDERER_D3D11
                 ? feature_level_name(g_app.d3d11.feature_level) : "D3D9",
             (double)g_app.frame_count / dt,
             g_app.render_sequence, g_app.last_render_angle,
             g_app.angle_regression_count,
             g_app.status[0] ? " - " : "",
             g_app.status);
    SetWindowTextA(g_app.hwnd, title);
    g_app.last_title_time = now;
    g_app.frame_count = 0;
}

static void release_d3d9(void)
{
    D3D9State *s = &g_app.d3d9;
    if (s->ib) IDirect3DIndexBuffer9_Release(s->ib);
    if (s->vb) IDirect3DVertexBuffer9_Release(s->vb);
    if (s->device) IDirect3DDevice9_Release(s->device);
    if (s->d3d) IDirect3D9_Release(s->d3d);
    memset(s, 0, sizeof(*s));
}

static HRESULT init_d3d9(void)
{
    D3D9State *s = &g_app.d3d9;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    HRESULT hr;
    void *data;

    s->d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!s->d3d) {
        return E_FAIL;
    }

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = (UINT)g_app.width;
    pp.BackBufferHeight = (UINT)g_app.height;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3D9_GetAdapterDisplayMode(s->d3d, D3DADAPTER_DEFAULT, &mode);
    (void)mode;

    hr = IDirect3D9_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                 g_app.hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                 &pp, &s->device);
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                     g_app.hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp, &s->device);
    }
    if (FAILED(hr)) {
        pp.AutoDepthStencilFormat = D3DFMT_D16;
        hr = IDirect3D9_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                     g_app.hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp, &s->device);
    }
    if (FAILED(hr)) return hr;

    hr = IDirect3DDevice9_CreateVertexBuffer(s->device, sizeof(g_d3d9_vertices),
                                             0, D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                             D3DPOOL_MANAGED, &s->vb, NULL);
    if (FAILED(hr)) return hr;
    hr = IDirect3DVertexBuffer9_Lock(s->vb, 0, 0, &data, 0);
    if (FAILED(hr)) return hr;
    memcpy(data, g_d3d9_vertices, sizeof(g_d3d9_vertices));
    IDirect3DVertexBuffer9_Unlock(s->vb);

    hr = IDirect3DDevice9_CreateIndexBuffer(s->device, sizeof(g_indices), 0,
                                            D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                            &s->ib, NULL);
    if (FAILED(hr)) return hr;
    hr = IDirect3DIndexBuffer9_Lock(s->ib, 0, 0, &data, 0);
    if (FAILED(hr)) return hr;
    memcpy(data, g_indices, sizeof(g_indices));
    IDirect3DIndexBuffer9_Unlock(s->ib);

    IDirect3DDevice9_SetRenderState(s->device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s->device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s->device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(s->device, D3DRS_MULTISAMPLEANTIALIAS, FALSE);
    IDirect3DDevice9_SetFVF(s->device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    return S_OK;
}

static void render_d3d9(float angle)
{
    D3D9State *s = &g_app.d3d9;
    D3DVIEWPORT9 viewport;
    Mat4 world = mat4_mul(mat4_rotation_x(angle * 0.67f), mat4_rotation_y(angle));
    Mat4 view = mat4_translation(0.0f, 0.0f, 5.0f);
    Mat4 proj = mat4_perspective_lh(60.0f * 3.1415926535f / 180.0f,
                                    (float)g_app.width / (float)g_app.height,
                                    0.1f, 100.0f);

    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = (DWORD)g_app.width;
    viewport.Height = (DWORD)g_app.height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(s->device, &viewport);

    IDirect3DDevice9_Clear(s->device, 0, NULL,
                           D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(15, 18, 24), 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(s->device))) {
        IDirect3DDevice9_SetTransform(s->device, D3DTS_WORLD, (const D3DMATRIX *)&world);
        IDirect3DDevice9_SetTransform(s->device, D3DTS_VIEW, (const D3DMATRIX *)&view);
        IDirect3DDevice9_SetTransform(s->device, D3DTS_PROJECTION, (const D3DMATRIX *)&proj);
        IDirect3DDevice9_SetStreamSource(s->device, 0, s->vb, 0, sizeof(D3D9Vertex));
        IDirect3DDevice9_SetIndices(s->device, s->ib);
        IDirect3DDevice9_DrawIndexedPrimitive(s->device, D3DPT_TRIANGLELIST,
                                              0, 0, (UINT)ARRAY_SIZE(g_d3d9_vertices),
                                              0, (UINT)ARRAY_SIZE(g_indices) / 3);
        IDirect3DDevice9_EndScene(s->device);
    }
    g_app.present_result = IDirect3DDevice9_Present(s->device, NULL, NULL, NULL, NULL);
}

static void release_d3d11(void)
{
    D3D11State *s = &g_app.d3d11;
    if (s->context) ID3D11DeviceContext_ClearState(s->context);
    if (s->raster_state) ID3D11RasterizerState_Release(s->raster_state);
    if (s->depth_state) ID3D11DepthStencilState_Release(s->depth_state);
    if (s->ps) ID3D11PixelShader_Release(s->ps);
    if (s->vs) ID3D11VertexShader_Release(s->vs);
    if (s->layout) ID3D11InputLayout_Release(s->layout);
    if (s->cb) ID3D11Buffer_Release(s->cb);
    if (s->ib) ID3D11Buffer_Release(s->ib);
    if (s->vb) ID3D11Buffer_Release(s->vb);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->depth_texture) ID3D11Texture2D_Release(s->depth_texture);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->swap_chain) IDXGISwapChain_Release(s->swap_chain);
    if (s->context) ID3D11DeviceContext_Release(s->context);
    if (s->device) ID3D11Device_Release(s->device);
    if (s->d3dcompiler) FreeLibrary(s->d3dcompiler);
    memset(s, 0, sizeof(*s));
}

static HRESULT compile_shader(const char *entry, const char *target, ID3DBlob **blob)
{
    static const char shader_src[] =
        "struct VSIn { float3 pos : POSITION; float4 color : COLOR0; };\n"
        "struct PSIn { float4 pos : SV_POSITION; float4 color : COLOR0; };\n"
        "PSIn vs_main(VSIn input) {\n"
        "    PSIn output;\n"
        "    output.pos = float4(input.pos, 1.0);\n"
        "    output.color = input.color;\n"
        "    return output;\n"
        "}\n"
        "float4 ps_main(PSIn input) : SV_TARGET { return input.color; }\n";
    ID3DBlob *errors = NULL;
    D3DCompileFn compile;
    union {
        FARPROC proc;
        D3DCompileFn compile;
    } loader;
    HRESULT hr;

    if (!g_app.d3d11.d3dcompiler) {
        g_app.d3d11.d3dcompiler = LoadLibraryA("D3DCOMPILER_47.dll");
        if (!g_app.d3d11.d3dcompiler) {
            g_app.d3d11.d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
        }
        if (!g_app.d3d11.d3dcompiler) {
            g_app.d3d11.d3dcompiler = LoadLibraryA("d3dcompiler_43.dll");
        }
    }
    if (!g_app.d3d11.d3dcompiler) {
        snprintf(g_app.status, sizeof(g_app.status), "D3D11 shader compiler DLL not found");
        app_log("%s", g_app.status);
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
    loader.proc = GetProcAddress(g_app.d3d11.d3dcompiler, "D3DCompile");
    compile = loader.compile;
    if (!compile) {
        snprintf(g_app.status, sizeof(g_app.status), "D3DCompile export not found");
        app_log("%s", g_app.status);
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }

    hr = compile(shader_src, strlen(shader_src), "embedded_hlsl",
                 NULL, NULL, entry, target, 0, 0, blob, &errors);
    if (FAILED(hr) && errors) {
        snprintf(g_app.status, sizeof(g_app.status), "D3DCompile %s failed: %.*s",
                 entry, (int)ID3D10Blob_GetBufferSize(errors),
                 (const char *)ID3D10Blob_GetBufferPointer(errors));
        app_log("%s", g_app.status);
    }
    if (errors) ID3D10Blob_Release(errors);
    return hr;
}

static HRESULT init_d3d11_targets(void)
{
    D3D11State *s = &g_app.d3d11;
    ID3D11Texture2D *backbuffer = NULL;
    D3D11_TEXTURE2D_DESC depth_desc;
    D3D11_VIEWPORT viewport;
    HRESULT hr;

    hr = IDXGISwapChain_GetBuffer(s->swap_chain, 0, &IID_ID3D11Texture2D,
                                  (void **)&backbuffer);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("IDXGISwapChain_GetBuffer", hr);
        return hr;
    }
    hr = ID3D11Device_CreateRenderTargetView(s->device, (ID3D11Resource *)backbuffer,
                                             NULL, &s->rtv);
    ID3D11Texture2D_Release(backbuffer);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateRenderTargetView", hr);
        return hr;
    }

    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = (UINT)g_app.width;
    depth_desc.Height = (UINT)g_app.height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(s->device, &depth_desc, NULL, &s->depth_texture);
    if (FAILED(hr)) {
        app_log("D3D11 CreateTexture2D D24S8 failed: HRESULT 0x%08lx; retry D16",
                (unsigned long)hr);
        depth_desc.Format = DXGI_FORMAT_D16_UNORM;
        hr = ID3D11Device_CreateTexture2D(s->device, &depth_desc, NULL, &s->depth_texture);
    }
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateTexture2D depth", hr);
        return hr;
    }
    hr = ID3D11Device_CreateDepthStencilView(s->device, (ID3D11Resource *)s->depth_texture,
                                             NULL, &s->dsv);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateDepthStencilView", hr);
        return hr;
    }

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = (float)g_app.width;
    viewport.Height = (float)g_app.height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s->context, 1, &viewport);
    return S_OK;
}

static const char *feature_level_name(D3D_FEATURE_LEVEL level)
{
    switch (level) {
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    default: return "unknown";
    }
}

static HRESULT init_d3d11(void)
{
    D3D11State *s = &g_app.d3d11;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_INPUT_ELEMENT_DESC elems[2];
    D3D11_DEPTH_STENCIL_DESC depth_state_desc;
    D3D11_RASTERIZER_DESC raster_desc;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    const char *vs_target;
    const char *ps_target;
    HRESULT hr;

    memset(&sd, 0, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)g_app.width;
    sd.BufferDesc.Height = (UINT)g_app.height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_app.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    app_log("D3D11 init begin: %dx%d", g_app.width, g_app.height);
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       levels, (UINT)ARRAY_SIZE(levels),
                                       D3D11_SDK_VERSION, &sd, &s->swap_chain,
                                       &s->device, &s->feature_level, &s->context);
    if (FAILED(hr)) {
        app_log("D3D11CreateDeviceAndSwapChain HARDWARE BufferCount=2 failed: HRESULT 0x%08lx",
                (unsigned long)hr);
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                           levels, (UINT)ARRAY_SIZE(levels),
                                           D3D11_SDK_VERSION, &sd, &s->swap_chain,
                                           &s->device, &s->feature_level, &s->context);
    }
    if (FAILED(hr)) {
        app_log("D3D11CreateDeviceAndSwapChain HARDWARE BufferCount=1 failed: HRESULT 0x%08lx",
                (unsigned long)hr);
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                                           levels, (UINT)ARRAY_SIZE(levels),
                                           D3D11_SDK_VERSION, &sd, &s->swap_chain,
                                           &s->device, &s->feature_level, &s->context);
    }
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateDeviceAndSwapChain", hr);
        return hr;
    }
    app_log("D3D11CreateDeviceAndSwapChain ok: feature level %s",
            feature_level_name(s->feature_level));

    hr = init_d3d11_targets();
    if (FAILED(hr)) return hr;

    if (s->feature_level <= D3D_FEATURE_LEVEL_9_3) {
        vs_target = "vs_4_0_level_9_1";
        ps_target = "ps_4_0_level_9_1";
    } else {
        vs_target = "vs_4_0";
        ps_target = "ps_4_0";
    }
    app_log("D3D11 shader targets: %s / %s", vs_target, ps_target);

    hr = compile_shader("vs_main", vs_target, &vs_blob);
    if (FAILED(hr)) return hr;
    hr = compile_shader("ps_main", ps_target, &ps_blob);
    if (FAILED(hr)) goto done;

    hr = ID3D11Device_CreateVertexShader(s->device,
                                         ID3D10Blob_GetBufferPointer(vs_blob),
                                         ID3D10Blob_GetBufferSize(vs_blob),
                                         NULL, &s->vs);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateVertexShader", hr);
        goto done;
    }
    hr = ID3D11Device_CreatePixelShader(s->device,
                                        ID3D10Blob_GetBufferPointer(ps_blob),
                                        ID3D10Blob_GetBufferSize(ps_blob),
                                        NULL, &s->ps);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreatePixelShader", hr);
        goto done;
    }

    memset(elems, 0, sizeof(elems));
    elems[0].SemanticName = "POSITION";
    elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[1].SemanticName = "COLOR";
    elems[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    elems[1].AlignedByteOffset = 12;
    elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    hr = ID3D11Device_CreateInputLayout(s->device, elems, 2,
                                        ID3D10Blob_GetBufferPointer(vs_blob),
                                        ID3D10Blob_GetBufferSize(vs_blob),
                                        &s->layout);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateInputLayout", hr);
        goto done;
    }

    memset(&bd, 0, sizeof(bd));
    memset(&init, 0, sizeof(init));
    bd.ByteWidth = sizeof(D3D11Vertex) * 12;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(s->device, &bd, NULL, &s->vb);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateBuffer vertex", hr);
        goto done;
    }

    memset(&bd, 0, sizeof(bd));
    memset(&init, 0, sizeof(init));
    bd.ByteWidth = sizeof(g_d3d11_indices);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = g_d3d11_indices;
    hr = ID3D11Device_CreateBuffer(s->device, &bd, &init, &s->ib);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateBuffer index", hr);
        goto done;
    }

    memset(&depth_state_desc, 0, sizeof(depth_state_desc));
    depth_state_desc.DepthEnable = TRUE;
    depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;
    hr = ID3D11Device_CreateDepthStencilState(s->device, &depth_state_desc, &s->depth_state);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateDepthStencilState", hr);
        goto done;
    }

    memset(&raster_desc, 0, sizeof(raster_desc));
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(s->device, &raster_desc, &s->raster_state);
    if (FAILED(hr)) {
        set_d3d11_stage_hr("CreateRasterizerState", hr);
    }

done:
    if (vs_blob) ID3D10Blob_Release(vs_blob);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    return hr;
}

static void render_d3d11(float angle, unsigned int frame_sequence)
{
    D3D11State *s = &g_app.d3d11;
    float clear_color[4] = {0.06f, 0.08f, 0.11f, 1.0f};
    UINT stride = sizeof(D3D11Vertex);
    UINT offset = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11Vertex transformed[12];
    HRESULT hr;
    Mat4 model = mat4_mul(mat4_rotation_x(angle * 0.67f), mat4_rotation_y(angle));
    Mat4 view = mat4_translation(0.0f, 0.0f, 5.0f);
    Mat4 proj = mat4_perspective_lh(60.0f * 3.1415926535f / 180.0f,
                                    (float)g_app.width / (float)g_app.height,
                                    0.1f, 100.0f);
    Mat4 mvp = mat4_mul(mat4_mul(model, view), proj);

    transform_to_ndc(g_d3d11_vertices, transformed, ARRAY_SIZE(g_d3d11_vertices), mvp);
    {
        const unsigned int marker = frame_sequence & 0xffu;
        const float marker_r = (float)(((marker >> 4) & 0xfu) * 8u + 4u) / 255.0f;
        const float marker_g = (float)((marker & 0xfu) * 8u + 4u) / 255.0f;
        const D3D11Vertex marker_vertices[4] = {
            {-0.96f,  0.96f, 0.0f, marker_r, marker_g, 1.0f, 1.0f},
            {-0.72f,  0.96f, 0.0f, marker_r, marker_g, 1.0f, 1.0f},
            {-0.72f,  0.78f, 0.0f, marker_r, marker_g, 1.0f, 1.0f},
            {-0.96f,  0.78f, 0.0f, marker_r, marker_g, 1.0f, 1.0f}
        };
        memcpy(&transformed[8], marker_vertices, sizeof(marker_vertices));
    }
    hr = ID3D11DeviceContext_Map(s->context, (ID3D11Resource *)s->vb, 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, transformed, sizeof(transformed));
        ID3D11DeviceContext_Unmap(s->context, (ID3D11Resource *)s->vb, 0);
    } else {
        set_d3d11_stage_hr("Map vertex buffer", hr);
        return;
    }

    ID3D11DeviceContext_ClearRenderTargetView(s->context, s->rtv, clear_color);
    ID3D11DeviceContext_ClearDepthStencilView(s->context, s->dsv, D3D11_CLEAR_DEPTH,
                                              1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s->context, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(s->context, s->depth_state, 0);
    ID3D11DeviceContext_RSSetState(s->context, s->raster_state);
    ID3D11DeviceContext_IASetInputLayout(s->context, s->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s->context, 0, 1, &s->vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(s->context, s->ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(s->context, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s->context, s->ps, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(s->context, (UINT)ARRAY_SIZE(g_d3d11_indices), 0, 0);
    g_app.present_result = IDXGISwapChain_Present(s->swap_chain, 0, 0);
}

static void release_renderer(void)
{
    release_d3d9();
    release_d3d11();
}

static void switch_renderer(RendererKind renderer)
{
    HRESULT hr;
    release_renderer();
    g_app.renderer = renderer;
    g_app.renderer_ready = 0;
    g_app.init_result = E_FAIL;
    g_app.present_result = E_FAIL;
    g_app.status[0] = '\0';
    if (g_app.width < 1) g_app.width = 1;
    if (g_app.height < 1) g_app.height = 1;

    hr = renderer == RENDERER_D3D11 ? init_d3d11() : init_d3d9();
    g_app.init_result = hr;
    if (FAILED(hr)) {
        if (!g_app.status[0]) {
            set_status_hr(renderer == RENDERER_D3D11 ? "init D3D11" : "init D3D9", hr);
        } else {
            app_log("renderer init final failure: %s; HRESULT 0x%08lx",
                    g_app.status, (unsigned long)hr);
        }
        release_renderer();
    } else {
        g_app.renderer_ready = 1;
        g_app.present_result = S_OK;
    }
    update_title(app_time_seconds(), 1);
}

static void render_frame(void)
{
    const double now = app_time_seconds();
    float angle = g_app.paused ? 0.0f : (float)now;
    if (g_app.paused) {
        angle = 0.75f;
    }
    if (!g_app.paused && g_app.render_sequence &&
        (double)angle + 0.000001 < g_app.last_render_angle) {
        ++g_app.angle_regression_count;
        app_log("CPU angle regression frame=%u current=%.9f previous=%.9f count=%u",
                g_app.render_sequence + 1, (double)angle,
                g_app.last_render_angle, g_app.angle_regression_count);
    }
    g_app.last_render_angle = angle;
    ++g_app.render_sequence;
    if (g_app.renderer == RENDERER_D3D11) {
        if (g_app.d3d11.device) render_d3d11(angle, g_app.render_sequence);
    } else {
        if (g_app.d3d9.device) render_d3d9(angle);
    }
    ++g_app.frame_count;
    if (g_app.renderer_ready) ++g_app.total_frame_count;
    if (g_app.automation && g_app.total_frame_count == 60)
        write_automation_result("started", "fixed-frame");
    update_title(now, 0);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            int width = LOWORD(lparam);
            int height = HIWORD(lparam);
            if (width > 0 && height > 0 &&
                (width != g_app.width || height != g_app.height)) {
                g_app.width = width;
                g_app.height = height;
                g_app.need_recreate = 1;
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            PostQuitMessage(0);
        } else if (wparam == '1' || wparam == VK_F1) {
            switch_renderer(RENDERER_D3D9);
        } else if (wparam == '2' || wparam == VK_F2) {
            switch_renderer(RENDERER_D3D11);
        } else if (wparam == VK_SPACE) {
            g_app.paused = !g_app.paused;
            update_title(app_time_seconds(), 1);
        }
        return 0;
    case WM_CLOSE:
        /* The Harmony automation surface can send a close while the window is
         * still being configured. Keep the bounded smoke alive long enough
         * to publish and capture its fixed frame. */
        if (g_app.automation && (!g_app.duration_ms ||
            GetTickCount64() - g_app.run_start_ms < g_app.duration_ms))
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd)
{
    WNDCLASSEXA wc;
    RECT rect = {0, 0, 960, 640};
    MSG msg;
    RendererKind initial_renderer;
    (void)prev_instance;
    (void)cmd_line;

    memset(&g_app, 0, sizeof(g_app));
    g_app.width = 960;
    g_app.height = 640;
    g_app.running = 1;
    parse_command_line(&initial_renderer);
    g_app.renderer = initial_renderer;
    g_app.run_start_ms = GetTickCount64();
    QueryPerformanceFrequency(&g_app.qpc_freq);
    QueryPerformanceCounter(&g_app.start_qpc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WineD3DSwitchCubeWindow";
    if (!RegisterClassExA(&wc)) {
        if (!g_app.automation)
            MessageBoxA(NULL, "RegisterClassExA failed", "WineD3D switch cube", MB_ICONERROR);
        return 1;
    }

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_app.hwnd = CreateWindowExA(0, wc.lpszClassName, "WineD3D VirGL switch cube",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rect.right - rect.left, rect.bottom - rect.top,
                                 NULL, NULL, instance, NULL);
    if (!g_app.hwnd) {
        if (!g_app.automation)
            MessageBoxA(NULL, "CreateWindowExA failed", "WineD3D switch cube", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_app.hwnd, show_cmd);
    UpdateWindow(g_app.hwnd);
    switch_renderer(initial_renderer);
    /* D3D/Vulkan initialization can consume most of a short automation run.
     * Start both the duration and animation clocks only after the renderer is
     * ready so the visual gate always observes a sustained render sequence. */
    if (g_app.renderer_ready) {
        g_app.run_start_ms = GetTickCount64();
        QueryPerformanceCounter(&g_app.start_qpc);
    }
    /* WM_SIZE is delivered during CreateWindow/ShowWindow. The initial
     * renderer already uses those final dimensions, so do not immediately
     * destroy and recreate its fresh DXGI swapchain. */
    g_app.need_recreate = 0;

    while (g_app.running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_app.running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_app.running) {
            break;
        }
        if (g_app.need_recreate) {
            g_app.need_recreate = 0;
            switch_renderer(g_app.renderer);
        }
        render_frame();
        if (g_app.duration_ms && GetTickCount64() - g_app.run_start_ms >= g_app.duration_ms)
            g_app.running = 0;
        Sleep(1);
    }

    write_automation_result(NULL, NULL);
    release_renderer();
    return g_app.renderer_ready && SUCCEEDED(g_app.present_result) &&
        g_app.total_frame_count >= (g_app.automation ? 60u : 1u) ? 0 : 1;
}

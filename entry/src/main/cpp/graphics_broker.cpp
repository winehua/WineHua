#include "graphics_broker.h"

#include "egl_renderer.h"
#include "wait_utils.h"
#include "wayland_server.h"

#include <EGL/egl.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <signal.h>
#include <thread>

#undef LOG_TAG
#define LOG_TAG "WL_GFX"
#include <hilog/log.h>

namespace winehua {

namespace {

using VirglGlContext = void*;

struct VirglGlCtxParam
{
    int version;
    bool shared;
    int major_ver;
    int minor_ver;
    int compat_ctx;
};

struct VirglRendererCallbacks
{
    int version;
    void (*write_fence)(void* cookie, uint32_t fence);
    VirglGlContext (*create_gl_context)(void* cookie, int scanout_idx, VirglGlCtxParam* param);
    void (*destroy_gl_context)(void* cookie, VirglGlContext ctx);
    int (*make_current)(void* cookie, int scanout_idx, VirglGlContext ctx);
    int (*get_drm_fd)(void* cookie);
    void (*write_context_fence)(void* cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id);
    int (*get_server_fd)(void* cookie, uint32_t version);
    void* (*get_egl_display)(void* cookie);
};

using PFNVirglRendererInit = int (*)(void* cookie, int flags, VirglRendererCallbacks* cb);
using PFNVirglRendererCleanup = void (*)(void* cookie);

constexpr int VIRGL_RENDERER_CALLBACKS_VERSION = 4;
constexpr int VIRGL_RENDERER_USE_GLES = 1 << 4;
constexpr const char* GUEST_GFX_DIRNAME = "guest_gfx";
constexpr const char* GUEST_GFX_ENVFILE = "winehua-guest-gfx.env";

struct ExternalEglContext
{
    EGLContext context = EGL_NO_CONTEXT;
};

bool EnsureDir(const std::string& path)
{
    if (path.empty()) return false;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileExists(const std::string& path)
{
    return !path.empty() && access(path.c_str(), F_OK) == 0;
}

bool DirExists(const std::string& path)
{
    struct stat st = {};

    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool DirHasSharedObjectWithPrefix(const std::string& dir, const std::string& prefix)
{
    DIR* handle = nullptr;
    struct dirent* entry = nullptr;

    if (dir.empty() || prefix.empty()) return false;
    handle = opendir(dir.c_str());
    if (!handle) return false;

    while ((entry = readdir(handle)))
    {
        std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        if (name.find(".so") == std::string::npos) continue;
        closedir(handle);
        return true;
    }

    closedir(handle);
    return false;
}

std::string ToLower(std::string value)
{
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string TrimCopy(const std::string& value)
{
    size_t start = 0;
    size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement)
{
    size_t pos = 0;

    if (needle.empty()) return;

    while ((pos = value.find(needle, pos)) != std::string::npos)
    {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

bool LoadGuestReceiverEnvFile(const std::string& receiverDir,
                              const std::string& envPath,
                              std::vector<std::string>& envLines,
                              std::string& mode)
{
    std::ifstream input(envPath);
    std::string line;

    if (!input.is_open()) return false;

    while (std::getline(input, line))
    {
        size_t equals;
        std::string key;
        std::string value;

        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') continue;
        if (!line.compare(0, 7, "export ")) line = TrimCopy(line.substr(7));

        equals = line.find('=');
        if (equals == std::string::npos) continue;

        key = TrimCopy(line.substr(0, equals));
        value = TrimCopy(line.substr(equals + 1));
        if (key.empty()) continue;

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        ReplaceAll(value, "$ORIGIN", receiverDir);
        if (key == "WINEHUA_GUEST_GFX_MODE") mode = value;

        if (key == "LD_LIBRARY_PATH" || key == "BOX64_LD_LIBRARY_PATH") continue;
        envLines.push_back(key + "=" + value);
    }

    return true;
}

std::string DescribeWaitStatus(int status)
{
    if (WIFEXITED(status))
    {
        return "exited code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status))
    {
        return "signaled signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

void StartChildLogReader(int fd, pid_t pid, const char* tag)
{
    std::thread([fd, pid, tag]() {
        char buf[2048];
        std::string pending;

        while (true)
        {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0)
            {
                pending.append(buf, static_cast<size_t>(n));
                while (true)
                {
                    size_t newline = pending.find('\n');
                    if (newline == std::string::npos) break;

                    std::string line = pending.substr(0, newline);
                    if (!line.empty())
                    {
                        OH_LOG_INFO(LOG_APP, "[%{public}s:%{public}d] %{public}s", tag, pid, line.c_str());
                    }
                    pending.erase(0, newline + 1);
                }
                continue;
            }

            if (n == 0) break;
            if (errno == EINTR) continue;

            OH_LOG_WARN(LOG_APP, "[%{public}s:%{public}d] read failed: %{public}s", tag, pid, strerror(errno));
            break;
        }

        if (!pending.empty())
        {
            OH_LOG_INFO(LOG_APP, "[%{public}s:%{public}d] %{public}s", tag, pid, pending.c_str());
        }
        close(fd);
    }).detach();
}

EGLConfig ChooseVirglEglConfig(EGLDisplay display, bool preferEs3)
{
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    EGLint renderableType = EGL_OPENGL_ES2_BIT;
#ifdef EGL_OPENGL_ES3_BIT_KHR
    if (preferEs3) renderableType = EGL_OPENGL_ES3_BIT_KHR;
#else
    (void)preferEs3;
#endif
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, renderableType,
        EGL_NONE,
    };

    if (!eglChooseConfig(display, attrs, &config, 1, &numConfigs) || numConfigs < 1) return nullptr;
    return config;
}

void VirglWriteFence(void*, uint32_t)
{
}

void VirglWriteContextFence(void*, uint32_t, uint32_t, uint64_t)
{
}

void* VirglGetEglDisplay(void*)
{
    EGLDisplay display = EglRenderer::GetSharedDisplay();
    if (display == EGL_NO_DISPLAY) return nullptr;
    return display;
}

VirglGlContext VirglCreateGlContext(void*, int, VirglGlCtxParam* param)
{
    EGLDisplay display = EglRenderer::GetSharedDisplay();
    ExternalEglContext* external = nullptr;
    EGLContext sharedContext = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;
    EGLint majorVersion = 3;
    EGLint contextAttrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };

    if (display == EGL_NO_DISPLAY) return nullptr;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return nullptr;

    if (param && param->major_ver > 0) majorVersion = param->major_ver;
    if (majorVersion < 2) majorVersion = 2;
    contextAttrs[1] = majorVersion >= 3 ? 3 : 2;

    config = ChooseVirglEglConfig(display, contextAttrs[1] >= 3);
    if (!config && contextAttrs[1] >= 3)
    {
        contextAttrs[1] = 2;
        config = ChooseVirglEglConfig(display, false);
    }
    if (!config) return nullptr;

    if (param && param->shared) sharedContext = eglGetCurrentContext();

    external = new ExternalEglContext();
    external->context = eglCreateContext(display, config, sharedContext, contextAttrs);
    if (external->context == EGL_NO_CONTEXT)
    {
        delete external;
        return nullptr;
    }

    return external;
}

void VirglDestroyGlContext(void*, VirglGlContext ctx)
{
    ExternalEglContext* external = static_cast<ExternalEglContext*>(ctx);
    EGLDisplay display = EglRenderer::GetSharedDisplay();

    if (!external) return;
    if (display != EGL_NO_DISPLAY && external->context != EGL_NO_CONTEXT)
        eglDestroyContext(display, external->context);
    delete external;
}

int VirglMakeCurrent(void*, int, VirglGlContext ctx)
{
    ExternalEglContext* external = static_cast<ExternalEglContext*>(ctx);
    EGLDisplay display = EglRenderer::GetSharedDisplay();
    EGLContext eglContext = external ? external->context : EGL_NO_CONTEXT;

    if (display == EGL_NO_DISPLAY) return -1;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return -1;
    return eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext) ? 0 : -1;
}

} // namespace

GraphicsBroker& GraphicsBroker::GetInstance()
{
    static GraphicsBroker broker;
    return broker;
}

GraphicsBroker::GraphicsBroker()
{
    const char* requested = std::getenv("WINEHUA_GRAPHICS_BACKEND");
    GraphicsBackend backend;

    if (requested && ParseBackendName(requested, &backend)) {
        requestedBackend_ = backend;
    }
}

void GraphicsBroker::SetWineRuntimeBinaryDir(const std::string& wineBinDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    wineRuntimeBinDir_ = wineBinDir;
    virglServerProgramPath_.clear();
    if (!wineRuntimeBinDir_.empty()) virglServerProgramPath_ = wineRuntimeBinDir_ + "/virgl_test_server";
    RefreshGuestReceiverStateLocked();
}

bool GraphicsBroker::EnsureStarted(const std::string& runtimeDir)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!EnsureRuntimeLocked(runtimeDir)) {
        lastError_ = "failed to prepare graphics runtime directory";
        return false;
    }

    RefreshGuestReceiverStateLocked();
    started_ = true;
    if (requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
        ProbeVirglRuntimeSmokeLocked();
    } else {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
    return true;
}

void GraphicsBroker::Stop()
{
    std::string socketPath;
    int serverPid = -1;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        virglServerRunning_.store(false, std::memory_order_release);
        socketPath = virglSocketPath_;
        serverPid = virglServerPid_;
        virglServerPid_ = -1;
        virglSocketReady_ = false;
        activeBackend_ = GraphicsBackend::Shm;
        started_ = false;
        runtimeReady_ = false;
    }

    if (serverPid > 0)
    {
        kill(serverPid, SIGTERM);
        waitpid(serverPid, nullptr, 0);
    }
    if (!socketPath.empty()) unlink(socketPath.c_str());
}

void GraphicsBroker::SetRequestedBackend(GraphicsBackend backend)
{
    std::lock_guard<std::mutex> lock(mutex_);

    requestedBackend_ = backend;
    loggedVirglFallback_ = false;
    if (started_ && requestedBackend_ == GraphicsBackend::Virgl) {
        RefreshVirglStateLocked();
        StartVirglSocketServerLocked();
        ProbeVirglRuntimeSmokeLocked();
    } else if (requestedBackend_ == GraphicsBackend::Shm) {
        lastError_.clear();
    }
    UpdateActiveBackendLocked();
}

GraphicsBackendState GraphicsBroker::GetState() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    GraphicsBackendState state;
    state.requested = requestedBackend_;
    state.active = activeBackend_;
    state.runtimeReady = runtimeReady_;
    state.guestReceiverPresent = guestReceiverPresent_;
    state.guestReceiverRuntimeDir = guestReceiverRuntimeDir_;
    state.guestReceiverMode = guestReceiverMode_;
    state.guestReceiverError = guestReceiverError_;
    state.virglSocketReady = virglSocketReady_;
    state.virglLibraryPresent = virglLibraryPresent_;
    state.virglSmokeAttempted = virglSmokeAttempted_;
    state.virglSmokeSucceeded = virglSmokeSucceeded_;
    state.zeroCopyFramePath = false;
    state.runtimeDir = runtimeDir_;
    state.virglSocketPath = virglSocketPath_;
    state.virglLibraryPath = virglLibraryPath_;
    state.frameTransportMode = "wl_shm+cpu_copy+gl_upload";
    state.virglSmokeError = virglSmokeError_;
    state.lastError = lastError_;
    return state;
}

void GraphicsBroker::AppendWineEnv(std::vector<std::string>& env) const
{
    GraphicsBackendState state = GetState();
    std::vector<std::string> guestEnv;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        guestEnv = guestReceiverEnv_;
    }

    env.push_back("WINEHUA_GRAPHICS_BACKEND=" + std::string(BackendName(state.requested)));
    env.push_back("WINEHUA_GRAPHICS_ACTIVE=" + std::string(BackendName(state.active)));
    env.push_back(std::string("WINEHUA_SHM_FALLBACK=") + (state.active == GraphicsBackend::Shm ? "1" : "0"));
    env.push_back("WINEHUA_FRAME_ZERO_COPY=0");
    env.push_back("WINEHUA_FRAME_TRANSPORT=" + state.frameTransportMode);
    env.push_back(std::string("WINEHUA_GUEST_GFX_READY=") + (state.guestReceiverPresent ? "1" : "0"));
    env.push_back("WINEHUA_GUEST_GFX_MODE=" + (state.guestReceiverMode.empty() ? std::string("stock-egl")
                                                                                : state.guestReceiverMode));
    if (!state.guestReceiverRuntimeDir.empty()) env.push_back("WINEHUA_GUEST_GFX_DIR=" + state.guestReceiverRuntimeDir);
    env.push_back(std::string("WINEHUA_VIRGL_SOCKET_READY=") + (state.virglSocketReady ? "1" : "0"));
    env.push_back(std::string("WINEHUA_VIRGL_LIBRARY_READY=") + (state.virglLibraryPresent ? "1" : "0"));
    if (!state.virglSocketPath.empty()) env.push_back("WINEHUA_VIRGL_SOCKET=" + state.virglSocketPath);
    if (!state.virglLibraryPath.empty()) env.push_back("WINEHUA_VIRGLRENDERER_LIB=" + state.virglLibraryPath);
    if (!state.lastError.empty()) env.push_back("WINEHUA_GRAPHICS_NOTE=" + state.lastError);
    env.push_back(std::string("WINEHUA_VIRGL_READY=") +
                  ((state.active == GraphicsBackend::Virgl) ? "1" : "0"));
    if (state.active == GraphicsBackend::Virgl)
    {
        for (const std::string& extra : guestEnv) env.push_back(extra);
        if (!state.virglSocketPath.empty()) env.push_back("VTEST_SOCKET_NAME=" + state.virglSocketPath);
    }
}

bool GraphicsBroker::TakeFrameForToplevel(uint32_t rendererToplevelId,
                                          std::vector<uint8_t>& outPixels,
                                          int& w,
                                          int& h,
                                          uint32_t* outSourceToplevelId)
{
    WaylandServer* ws = WaylandServer::GetInstance();
    uint32_t sourceToplevelId = rendererToplevelId;

    if (ws->IsDesktopMode()) sourceToplevelId = ws->GetDesktopRootToplevelId();
    if (outSourceToplevelId) *outSourceToplevelId = sourceToplevelId;

    if (sourceToplevelId != 0) return ws->TakeToplevelFrame(sourceToplevelId, outPixels, w, h);
    return ws->TakeFrame(outPixels, w, h);
}

const char* GraphicsBroker::BackendName(GraphicsBackend backend)
{
    switch (backend) {
    case GraphicsBackend::Virgl:
        return "virgl";
    case GraphicsBackend::Shm:
    default:
        return "shm";
    }
}

bool GraphicsBroker::ParseBackendName(const std::string& name, GraphicsBackend* outBackend)
{
    if (!outBackend) return false;

    std::string lower = ToLower(name);
    if (lower == "shm") {
        *outBackend = GraphicsBackend::Shm;
        return true;
    }
    if (lower == "virgl") {
        *outBackend = GraphicsBackend::Virgl;
        return true;
    }
    return false;
}

bool GraphicsBroker::EnsureRuntimeLocked(const std::string& runtimeDir)
{
    if (!EnsureDir(runtimeDir)) return false;

    runtimeDir_ = runtimeDir + "/graphics";
    virglSocketPath_ = runtimeDir_ + "/virgl.sock";
    runtimeReady_ = EnsureDir(runtimeDir_);
    return runtimeReady_;
}

bool GraphicsBroker::IsVirglServerProcessAliveLocked()
{
    int status = 0;
    pid_t waited = 0;

    if (virglServerPid_ <= 0) return false;

    waited = waitpid(virglServerPid_, &status, WNOHANG);
    if (waited == 0) return true;

    if (waited == virglServerPid_)
    {
        std::string statusText = DescribeWaitStatus(status);
        lastError_ = "virgl_test_server terminated: " + statusText;
        OH_LOG_WARN(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d exited before guest connection (%{public}s)",
                    virglServerPid_, statusText.c_str());
    }
    virglServerPid_ = -1;
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    return false;
}

void GraphicsBroker::RefreshVirglStateLocked()
{
    bool loaded = false;
    virglLibraryPath_ = ProbeVirglLibraryLocked(&loaded);
    virglLibraryPresent_ = loaded;
    virglSmokeAttempted_ = false;
    virglSmokeSucceeded_ = false;
    virglSmokeError_.clear();
    if (!virglLibraryPresent_) lastError_ = "virglrenderer library not found; using shm fallback";
}

void GraphicsBroker::RefreshGuestReceiverStateLocked()
{
    std::string receiverDir;
    std::string envPath;
    std::string libDir;
    std::vector<std::string> envLines;
    std::string mode;
    bool hasLibEGL = false;
    bool hasClientApi = false;
    bool hasDriverPayload = false;

    guestReceiverPresent_ = false;
    guestReceiverRuntimeDir_.clear();
    guestReceiverMode_.clear();
    guestReceiverError_.clear();
    guestReceiverEnv_.clear();

    if (wineRuntimeBinDir_.empty()) return;

    receiverDir = wineRuntimeBinDir_ + "/" + GUEST_GFX_DIRNAME;
    envPath = receiverDir + "/" + GUEST_GFX_ENVFILE;
    if (!FileExists(envPath))
    {
        guestReceiverError_ = "guest receiver env missing: " + envPath;
        return;
    }
    if (!LoadGuestReceiverEnvFile(receiverDir, envPath, envLines, mode))
    {
        guestReceiverError_ = "failed to parse guest receiver env: " + envPath;
        return;
    }

    guestReceiverRuntimeDir_ = receiverDir;
    guestReceiverMode_ = mode.empty() ? "external-bundle" : mode;

    libDir = receiverDir + "/lib";
    if (!DirExists(libDir))
    {
        guestReceiverError_ = "guest receiver lib dir missing: " + libDir;
        return;
    }

    hasLibEGL = FileExists(libDir + "/libEGL.so") || FileExists(libDir + "/libEGL.so.1");
    if (!hasLibEGL)
    {
        guestReceiverError_ = "guest receiver is missing libEGL.so* in " + libDir;
        return;
    }

    hasClientApi = FileExists(libDir + "/libGL.so") || FileExists(libDir + "/libGL.so.1") ||
                   FileExists(libDir + "/libOpenGL.so") || FileExists(libDir + "/libOpenGL.so.0") ||
                   FileExists(libDir + "/libGLESv2.so") || FileExists(libDir + "/libGLESv2.so.2") ||
                   FileExists(libDir + "/libGLESv1_CM.so") || FileExists(libDir + "/libGLESv1_CM.so.1");
    if (!hasClientApi)
    {
        guestReceiverError_ = "guest receiver is missing libGL.so* or libGLESv2.so* in " + libDir;
        return;
    }

    hasDriverPayload = DirExists(libDir + "/dri") || DirExists(libDir + "/egl") || DirExists(libDir + "/gallium") ||
                       FileExists(libDir + "/libgallium_dri.so") ||
                       DirHasSharedObjectWithPrefix(libDir, "libgallium-");
    if (!hasDriverPayload)
    {
        guestReceiverError_ = "guest receiver is missing Mesa driver payloads (dri/egl/gallium/libgallium-*.so) in " + libDir;
        return;
    }

    guestReceiverPresent_ = true;
    guestReceiverEnv_ = std::move(envLines);

    OH_LOG_INFO(LOG_APP,
                "[GraphicsBroker] guest 3D receiver bundle detected mode=%{public}s dir=%{public}s",
                guestReceiverMode_.c_str(),
                guestReceiverRuntimeDir_.c_str());
}

void GraphicsBroker::ProbeVirglRuntimeSmokeLocked()
{
    void* handle = nullptr;
    PFNVirglRendererInit virglRendererInit = nullptr;
    PFNVirglRendererCleanup virglRendererCleanup = nullptr;

    virglSmokeAttempted_ = false;
    virglSmokeSucceeded_ = false;
    virglSmokeError_.clear();

    if (!virglLibraryPresent_ || virglLibraryPath_.empty())
    {
        virglSmokeError_ = "virglrenderer library not found";
        return;
    }

    handle = dlopen(virglLibraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        virglSmokeAttempted_ = true;
        virglSmokeError_ = std::string("dlopen failed: ") + dlerror();
        return;
    }

    virglRendererInit = reinterpret_cast<PFNVirglRendererInit>(dlsym(handle, "virgl_renderer_init"));
    virglRendererCleanup = reinterpret_cast<PFNVirglRendererCleanup>(dlsym(handle, "virgl_renderer_cleanup"));
    if (!virglRendererInit || !virglRendererCleanup)
    {
        virglSmokeAttempted_ = true;
        virglSmokeError_ = "required virglrenderer symbols are missing";
        dlclose(handle);
        return;
    }

    /*
     * Do not call virgl_renderer_init() inside the host Ability for now.
     * On OHOS this probe can tear down the shared EGL/Wayland host state and
     * crash the entire app before the Windows smoke exe even starts.
     *
     * For Step 1 we keep the probe intentionally shallow: the bundled library
     * loads and the required entrypoints resolve, but real virgl command
     * transport is still not wired into the guest path yet.
     */
    virglSmokeError_ = "host-side virgl init probe disabled on OHOS; library/symbol check only";
    OH_LOG_WARN(LOG_APP, "[GraphicsBroker] skipping in-process virgl init probe: %{public}s",
                virglSmokeError_.c_str());

    dlclose(handle);
}

void GraphicsBroker::StartVirglSocketServerLocked()
{
    std::string serverPath;
    std::string serverDir;
    std::string ldLibraryPath;
    std::string guestLibDir;
    int logPipe[2] = {-1, -1};
    bool logPipeOk = false;
    bool reuseGuestMesaRuntime = false;
    pid_t pid = -1;

    if (!runtimeReady_ || virglSocketPath_.empty()) return;
    if (virglServerRunning_.load(std::memory_order_acquire) && IsVirglServerProcessAliveLocked()) return;
    if (wineRuntimeBinDir_.empty())
    {
        lastError_ = "wine runtime bin dir is not configured; using shm fallback";
        return;
    }

    serverPath = virglServerProgramPath_.empty() ? (wineRuntimeBinDir_ + "/virgl_test_server") : virglServerProgramPath_;
    if (access(serverPath.c_str(), X_OK) != 0)
    {
        lastError_ = "virgl_test_server is missing from HNP runtime: " + serverPath;
        return;
    }

    serverDir = wineRuntimeBinDir_;
    ldLibraryPath = wineRuntimeBinDir_ + ":" + wineRuntimeBinDir_ + "/x86_64-unix:" + wineRuntimeBinDir_ + "/../lib/x86_64";
    guestLibDir = guestReceiverRuntimeDir_.empty() ? std::string() : (guestReceiverRuntimeDir_ + "/lib");
    if (guestReceiverPresent_ && DirExists(guestLibDir))
    {
        /*
         * Reuse the bundled guest Mesa/EGL runtime for virgl_test_server on
         * OHOS. The host app process does not ship a system-visible libEGL.so.1,
         * so the vtest server must see the guest receiver's libEGL/libGLES
         * payload to bring up a surfaceless GLES context.
         *
         * Do not forward GALLIUM_DRIVER=virpipe here: the host side must stay a
         * renderer, not recurse back into the guest transport.
         */
        ldLibraryPath = guestLibDir + ":" + ldLibraryPath;
        reuseGuestMesaRuntime = true;
    }
    OH_LOG_INFO(LOG_APP,
                "[GraphicsBroker] virgl_test_server runtime: reuseGuestMesa=%{public}d libPath=%{public}s",
                reuseGuestMesaRuntime ? 1 : 0,
                ldLibraryPath.c_str());
    logPipeOk = (pipe(logPipe) == 0);
    if (!logPipeOk)
    {
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] failed to create virgl_test_server log pipe: %{public}s",
                    strerror(errno));
    }

    unlink(virglSocketPath_.c_str());
    pid = fork();
    if (pid < 0)
    {
        if (logPipeOk)
        {
            close(logPipe[0]);
            close(logPipe[1]);
        }
        lastError_ = std::string("failed to fork virgl_test_server: ") + strerror(errno);
        virglServerPid_ = -1;
        virglServerRunning_.store(false, std::memory_order_release);
        virglSocketReady_ = false;
        return;
    }

    if (pid == 0)
    {
        if (logPipeOk)
        {
            close(logPipe[0]);
            dup2(logPipe[1], STDOUT_FILENO);
            dup2(logPipe[1], STDERR_FILENO);
            if (logPipe[1] > 2) close(logPipe[1]);
        }
        setenv("LD_LIBRARY_PATH", ldLibraryPath.c_str(), 1);
        setenv("VTEST_USE_GLES", "1", 1);
        setenv("VTEST_USE_EGL_SURFACELESS", "1", 1);
        setenv("EGL_PLATFORM", "surfaceless", 1);
        if (reuseGuestMesaRuntime)
        {
            setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            setenv("MESA_LOADER_DRIVER_OVERRIDE", "swrast", 1);
            unsetenv("GALLIUM_DRIVER");
        }
        chdir(serverDir.c_str());
        execl(serverPath.c_str(),
              "virgl_test_server",
              "--no-fork",
              "--use-egl-surfaceless",
              "--use-gles",
              "--socket-path",
              virglSocketPath_.c_str(),
              nullptr);
        dprintf(STDERR_FILENO, "virgl_test_server exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (logPipeOk)
    {
        close(logPipe[1]);
        StartChildLogReader(logPipe[0], pid, "virgl-test");
    }

    virglServerPid_ = pid;
    virglServerRunning_.store(true, std::memory_order_release);
    virglSocketReady_ = false;

    if (WaitFor("virgl_test_server socket",
                [this]() { return FileExists(virglSocketPath_) || !IsVirglServerProcessAliveLocked(); },
                4000, 100) &&
        FileExists(virglSocketPath_) &&
        IsVirglServerProcessAliveLocked())
    {
        virglSocketReady_ = true;
        lastError_ = "VirGL vtest server is up; waiting for guest-side 3D receiver";
        OH_LOG_INFO(LOG_APP,
                    "[GraphicsBroker] virgl_test_server pid=%{public}d listening at %{public}s",
                    virglServerPid_, virglSocketPath_.c_str());
        return;
    }

    if (virglServerPid_ > 0)
    {
        kill(virglServerPid_, SIGTERM);
        waitpid(virglServerPid_, nullptr, 0);
    }
    virglServerPid_ = -1;
    virglServerRunning_.store(false, std::memory_order_release);
    virglSocketReady_ = false;
    lastError_ = "timed out waiting for virgl_test_server socket";
}

void GraphicsBroker::UpdateActiveBackendLocked()
{
    if (requestedBackend_ == GraphicsBackend::Shm) {
        activeBackend_ = GraphicsBackend::Shm;
        loggedVirglFallback_ = false;
        return;
    }

    if (runtimeReady_ && virglLibraryPresent_ && virglSocketReady_ && guestReceiverPresent_)
    {
        activeBackend_ = GraphicsBackend::Virgl;
        loggedVirglFallback_ = false;
        if (!virglSmokeAttempted_ && !virglSmokeError_.empty()) {
            lastError_ = "virgl host probe skipped on OHOS; relying on guest receiver bundle for runtime validation";
        } else {
            lastError_.clear();
        }
        return;
    }

    activeBackend_ = GraphicsBackend::Shm;
    if (!runtimeReady_) {
        lastError_ = "graphics runtime is not ready; using shm fallback";
    } else if (!virglLibraryPresent_) {
        lastError_ = "virglrenderer library not found; using shm fallback";
    } else if (!virglSocketReady_) {
        lastError_ = "virgl socket is not ready yet; using shm fallback";
    } else if (!guestReceiverPresent_) {
        if (!guestReceiverError_.empty()) {
            lastError_ = "virgl host is ready, but guest receiver bundle is incomplete: " + guestReceiverError_ +
                         "; Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        } else {
            lastError_ = "virgl host is ready, but no guest 3D receiver bundle was staged "
                         "(guest_gfx/winehua-guest-gfx.env missing); Windows OpenGL/DX still uses stock wayland/EGL; using shm fallback";
        }
    } else if (virglSmokeAttempted_ && !virglSmokeSucceeded_) {
        lastError_ = "virgl external-EGL smoke failed: " + virglSmokeError_ + "; using shm fallback";
    } else {
        lastError_ = "VirGL runtime prerequisites are not satisfied yet; using shm fallback";
    }

    if (!loggedVirglFallback_) {
        OH_LOG_WARN(LOG_APP, "[GraphicsBroker] requested backend=%{public}s active=%{public}s reason=%{public}s",
                    BackendName(requestedBackend_), BackendName(activeBackend_), lastError_.c_str());
        loggedVirglFallback_ = true;
    }
}

std::string GraphicsBroker::ProbeVirglLibraryLocked(bool* outLoaded) const
{
    const char* envLib = std::getenv("WINEHUA_VIRGLRENDERER_LIB");
    const char* candidates[] = {
        envLib && envLib[0] ? envLib : nullptr,
        "libvirglrenderer.so",
        "libvirglrenderer.so.1",
    };

    if (outLoaded) *outLoaded = false;

    for (const char* candidate : candidates) {
        if (!candidate || !candidate[0]) continue;

        void* handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;

        void* symbol = dlsym(handle, "virgl_renderer_init");
        dlclose(handle);
        if (!symbol) continue;

        if (outLoaded) *outLoaded = true;
        return candidate;
    }

    return "";
}

} // namespace winehua

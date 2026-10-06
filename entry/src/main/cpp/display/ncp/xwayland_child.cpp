/*
 * xwayland_child.cpp — Xwayland NCP shim (libxwayland_child.so)
 *
 * OHOS NativeChildProcess 无法 fork/exec 可执行文件 (spec R-SPAWN), Xwayland
 * 以 libxwayland_ohos.so (导出 main) 形态随 HAP 分发。本 shim 的职责:
 *   1. 由父进程经 OH_Ability_StartNativeChildProcess 启动, 收命名 fd 五连:
 *      x_fd0 / x_fd1 / wl_fd / wm_fd / displayfd (fdName ≤ 20 字节, OHOS 限制)
 *   2. 解析 entryParams: "<displayName>|<terminateDelay>|<noTouch>|<xrandr>|<enableWm>"
 *   3. 重建 argv (与 wlroots wlr_xwayland_server_ohos_build_argv 契约一致,
 *      哨兵替换为 NCP 框架下发的实际 fd 号), setenv WAYLAND_SOCKET
 *   4. dlopen libxwayland_ohos.so, 调其导出的 main(argc, argv, envp) —
 *      阻塞直至合成器侧断开 (wl_fd broken pipe), 与上游 fork/exec 语义一致
 *
 * fork/exec 探针: 计划 M0-T7 要求, 启动时探测一次 fork+execve 可用性,
 * 结果写 hilog (标签 XWAYLAND-NCP), 回填 spec 决定 M1 R-xkb (xkbcomp) 形态。
 */
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <dlfcn.h>

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <vector>

#define LOG_TAG "XWAYLAND-NCP"

namespace {

int FdByName(const NativeChildProcess_Args& args, const char* name, int* out)
{
    for (auto* node = args.fdList.head; node; node = node->next)
    {
        if (node->fdName && strcmp(node->fdName, name) == 0)
        {
            *out = node->fd;
            return 0;
        }
    }
    return -1;
}

// fd 数值以文本形式出现在 argv 里, 重建时把哨兵替换为实际值
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

void UnsetCloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args)
{
    const char* params = args.entryParams ? args.entryParams : "";
    OH_LOG_INFO(LOG_APP, "Main enter pid=%{public}d params=%{public}s", getpid(), params);

    // -- 1. 命名 fd 五连 --
    int x_fd0 = -1, x_fd1 = -1, wl_fd = -1, wm_fd = -1, display_fd = -1;
    if (FdByName(args, "x_fd0", &x_fd0) || FdByName(args, "x_fd1", &x_fd1) ||
        FdByName(args, "wl_fd", &wl_fd) || FdByName(args, "displayfd", &display_fd))
    {
        OH_LOG_ERROR(LOG_APP, "missing named fd(s), aborting");
        return;
    }
    bool enable_wm = FdByName(args, "wm_fd", &wm_fd) == 0;
    OH_LOG_INFO(LOG_APP, "fds ready x_fd0=%{public}d x_fd1=%{public}d wl_fd=%{public}d "
                         "wm_fd=%{public}d displayfd=%{public}d",
                x_fd0, x_fd1, wl_fd, enable_wm ? wm_fd : -1, display_fd);

    // -- 2. entryParams: name|terminateDelay|noTouch|xrandr|enableWm --
    std::string entryParams(params);
    std::vector<std::string> f;
    size_t start = 0;
    while (true)
    {
        size_t p = entryParams.find('|', start);
        f.push_back(entryParams.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    if (f.size() < 5)
    {
        OH_LOG_ERROR(LOG_APP, "entryParams parse failed (need 5 fields)");
        return;
    }
    // terminateDelay/noTouch/forceXrandr 曾驱动 shim 侧 argv 副本; argv 改由
    // app 侧唯一来源传递后此处仅按位置跳过 (字段仍在 params 里, 诊断可读)
    enable_wm = enable_wm && atoi(f[4].c_str()) != 0;
    // f[6] = appPid: shim 侧无消费方 (父进程存活由 NCP 框架管理), 仅按位保留
    std::string xdgDir = f.size() > 7 ? f[7] : "";

    // Xwayland stdout/stderr 落盘 (父侧下发的沙箱路径); Xwayland 自身日志走
    // stderr, NCP 子进程默认不可见, 必须重定向才能排障。
    // 已知坑 (D29 实测 r20261007): 本文件可能落子进程私有 mount 视图, 宿主
    // 侧 hdc file recv 读不到 (同型 xclient 日志可见而本文件不可见, 分裂原
    // 因未查)。需要看 Xwayland stderr 时: 临时在 dup2 之后加 pipe→hilog 转
    // 发线程 (D29 诊断用过, 已撤), 或扩 docs/debugging 观测面。
    if (f.size() > 5 && !f[5].empty())
    {
        int logFd = open(f[5].c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0)
        {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }
        else
        {
            OH_LOG_WARN(LOG_APP, "stderr redirect open failed errno=%{public}d path=%{public}s",
                        errno, f[5].c_str());
        }
    }

    // -- 2.5 XDG_RUNTIME_DIR + xkm 缓存 --
    // NCP 子进程不继承 app 环境: Xwayland 的 OutputDirectory (xkb/ddxLoad.c:65)
    // 找不到 XDG_RUNTIME_DIR 会回退编译期宿主路径 (/tmp), xkm 缓存永远 miss →
    // exec xkbcomp 被拒 (NCP exec 禁令, 实测 errno 13) → 键盘激活 FatalError。
    if (!xdgDir.empty())
    {
        setenv("XDG_RUNTIME_DIR", xdgDir.c_str(), 1);
        // 预编译 keymap 缓存 (wine-data/xkm/server-0.xkm, assemble 期由宿主
        // xkbcomp 生成) → XDG_RUNTIME_DIR 根目录 (Xwayland 缓存查找点,
        // OutputDirectory 命中即跳过 xkbcomp)。引擎数据在 xdg 的同级 wine/ 下。
        size_t slash = xdgDir.rfind('/');
        if (slash != std::string::npos)
        {
            std::string src = xdgDir.substr(0, slash) + "/wine/xkm/server-0.xkm";
            std::string dst = xdgDir + "/server-0.xkm";
            int in = open(src.c_str(), O_RDONLY);
            if (in >= 0)
            {
                int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out >= 0)
                {
                    char buf[8192];
                    ssize_t r;
                    while ((r = read(in, buf, sizeof(buf))) > 0)
                        (void)!write(out, buf, r);
                    close(out);
                }
                close(in);
                OH_LOG_INFO(LOG_APP, "xkm cache staged %{public}s → %{public}s",
                            src.c_str(), dst.c_str());
            }
        }
    }

    // -- 4. fd 就绪处理 (同上游 exec_xwayland 的 cloexec 语义) --
    UnsetCloexec(x_fd0);
    UnsetCloexec(x_fd1);
    UnsetCloexec(wl_fd);
    if (enable_wm) UnsetCloexec(wm_fd);
    UnsetCloexec(display_fd);
    char wlStr[16];
    snprintf(wlStr, sizeof(wlStr), "%d", wl_fd);
    setenv("WAYLAND_SOCKET", wlStr, 1);

    // -- 5. 解析 argv (app 侧 wlr_xwayland_server_ohos_build_argv 是唯一来源,
    //       从 entryParams 第 9 字段起逐项传递; fd 哨兵替换为实际值) --
    // 曾有 shim 侧硬编码副本与 app 侧 builder 人肉对齐, 实测漂移 (缺 -xkbdir
    // 的 argc=16 → XKB 键盘激活 FatalError); 按单一来源原则删除副本。
    if (f.size() < 9)
    {
        OH_LOG_ERROR(LOG_APP, "entryParams missing argv fields (%{public}zu)", f.size());
        return;
    }
    std::vector<std::string> argvStore;
    for (size_t i = 8; i < f.size(); ++i)
    {
        const std::string& tok = f[i];
        if (tok == "@XFD0@") argvStore.push_back(std::to_string(x_fd0));
        else if (tok == "@XFD1@") argvStore.push_back(std::to_string(x_fd1));
        else if (tok == "@DISPLAYFD@") argvStore.push_back(std::to_string(display_fd));
        else if (tok == "@WMFD@") argvStore.push_back(std::to_string(wm_fd));
        else argvStore.push_back(tok);
    }
    if (argvStore.empty() || argvStore[0] != "Xwayland")
    {
        OH_LOG_ERROR(LOG_APP, "argv[0] invalid: %{public}s",
                     argvStore.empty() ? "(empty)" : argvStore[0].c_str());
        return;
    }

    std::vector<char*> argv;
    for (auto& a : argvStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    // -- 6. 载入 Xwayland 本体 --
    void* handle = dlopen("libxwayland_ohos.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        OH_LOG_ERROR(LOG_APP, "dlopen libxwayland_ohos.so failed: %{public}s", dlerror());
        return;
    }
    using XwaylandMain = int (*)(int, char**, char**);
    auto xmain = reinterpret_cast<XwaylandMain>(dlsym(handle, "main"));
    if (!xmain)
    {
        OH_LOG_ERROR(LOG_APP, "dlsym main failed: %{public}s", dlerror());
        return;
    }

    OH_LOG_INFO(LOG_APP, "ready, entering Xwayland main argc=%{public}zu", argv.size() - 1);
    int rc = xmain(static_cast<int>(argv.size() - 1), argv.data(), environ);
    // 到这里说明合成器侧已断开 (wl_fd broken pipe) — 与上游 fork/exec 语义一致
    OH_LOG_INFO(LOG_APP, "Xwayland main returned rc=%{public}d", rc);
}

// sandbox_probe.cpp — M0 设备探针：验证显示路线重构（spec 2026-09-27 §5 B 类）依赖的
// 应用沙箱前置条件。仅存在于 feature/sandbox-probe 分支，不进产品线。
//
// 触发：库加载时自动执行一次（constructor）。
// 结果：hilog 标签 SANDBOX-PROBE + 落盘 /data/storage/el2/base/temp/sandbox_probe_result.txt
//       （hilog 缓冲区只留几分钟，落盘是留证主通道，见 .claude/rules/build-and-log.md）
//
// 探针项：
//   P1 shm_open（/dev/shm 可用性）—— wlroots shm 分配器路径
//   P2 memfd_create —— Xwayland/wlroots 匿名内存首选路径
//   P3 XDG_RUNTIME_DIR / 应用 temp 目录 tmpfile —— xwayland-shm.c 回退 + xkm 输出
//   P4 unix socket：路径绑定 + abstract 绑定 —— X11 传输两形态
//   P5 /tmp 与 /tmp/.X11-unix 可写性 —— Xwayland 锁文件/socket 硬编码路径

#include <hilog/log.h>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <dirent.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace {
constexpr const char *TAG = "SANDBOX-PROBE";
constexpr const char *RESULT_PATH = "/data/storage/el2/base/temp/sandbox_probe_result.txt";

std::string g_report;

void emit(const std::string &s)
{
    OH_LOG_INFO(LOG_APP, "%{public}s", s.c_str());
    g_report += s;
    g_report += '\n';
}

std::string errno_name(int e)
{
    switch (e) {
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case EEXIST: return "EEXIST";
        case ENOSYS: return "ENOSYS";
        case EINVAL: return "EINVAL";
        case ENOMEM: return "ENOMEM";
        case EMFILE: return "EMFILE";
        case EOPNOTSUPP: return "EOPNOTSUPP";
        default: return std::string("errno=") + std::to_string(e);
    }
}

// P1: shm_open → /dev/shm
void probe_shm_open()
{
    const char *name = "/winehua_probe";
    emit("P1 shm_open: begin");
    int dir = access("/dev/shm", W_OK);
    emit("P1 /dev/shm access(W_OK): " + std::string(dir == 0 ? "OK" : "FAIL ") +
         (dir == 0 ? "" : errno_name(errno)));
    DIR *d = opendir("/dev/shm");
    emit(std::string("P1 /dev/shm opendir: ") + (d ? "OK" : std::string("FAIL ") + errno_name(errno)));
    if (d) closedir(d);

    errno = 0;
    int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) {
        emit("P1 shm_open(O_CREAT|O_EXCL): OK fd=" + std::to_string(fd));
        close(fd);
        shm_unlink(name);
    } else {
        emit("P1 shm_open: FAIL " + errno_name(errno));
    }
}

// P2: memfd_create（走 syscall，不依赖 libc 包装是否导出）
void probe_memfd()
{
    errno = 0;
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "winehua_probe", MFD_CLOEXEC);
#else
    int fd = -1;
    errno = ENOSYS;
#endif
    if (fd >= 0) {
        // 顺手验证 ftruncate+mmap（wl_shm pool 的实际用法）
        int tr = ftruncate(fd, 4096);
        void *m = tr == 0 ? mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        emit(std::string("P2 memfd_create: OK fd=") + std::to_string(fd) +
             " ftruncate+mmap: " + (m != MAP_FAILED ? "OK" : std::string("FAIL ") + errno_name(errno)));
        if (m != MAP_FAILED) munmap(m, 4096);
        close(fd);
    } else {
        emit("P2 memfd_create: FAIL " + errno_name(errno));
    }
}

// P3: tmpfile 候选目录（XDG_RUNTIME_DIR 与应用已知可写目录）
void probe_tmpfile()
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    emit(std::string("P3 XDG_RUNTIME_DIR env: ") + (xdg ? xdg : "<unset>"));

    std::vector<std::string> dirs;
    if (xdg) dirs.push_back(xdg);
    dirs.push_back("/data/storage/el2/base/temp");
    dirs.push_back("/data/storage/el2/base/cache");
    for (const auto &dir : dirs) {
        std::string tpl = dir + "/probeXXXXXX";
        std::vector<char> buf(tpl.begin(), tpl.end());
        buf.push_back('\0');
        errno = 0;
        int fd = mkostemp(buf.data(), O_CLOEXEC);
        if (fd >= 0) {
            emit("P3 mkostemp(" + dir + "): OK");
            close(fd);
            unlink(buf.data());
        } else {
            emit("P3 mkostemp(" + dir + "): FAIL " + errno_name(errno));
        }
    }
}

// P4: unix socket 路径绑定 + abstract 绑定
void probe_unix_socket()
{
    auto try_path = [](const std::string &path) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            emit("P4 socket(AF_UNIX): FAIL " + errno_name(errno));
            return;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        unlink(path.c_str());
        errno = 0;
        int rc = bind(fd, (sockaddr *)&addr, sizeof(addr));
        emit("P4 path bind " + path + ": " + (rc == 0 ? std::string("OK") : std::string("FAIL ") + errno_name(errno)));
        if (rc == 0) unlink(path.c_str());
        close(fd);
    };
    try_path("/data/storage/el2/base/temp/winehua_probe.sock");
    try_path("/tmp/.winehua_probe.sock");

    // abstract: sun_path[0] = '\0'，名字占剩余字节
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const char *name = "\0winehua-probe-abstract";
        memcpy(addr.sun_path, name, strlen(name + 1) + 1);
        socklen_t len = offsetof(sockaddr_un, sun_path) + strlen(name + 1) + 1;
        errno = 0;
        int rc = bind(fd, (sockaddr *)&addr, len);
        emit("P4 abstract bind: " + std::string(rc == 0 ? "OK" : std::string("FAIL ") + errno_name(errno)));
        close(fd);
    }
}

// P5: /tmp 与 /tmp/.X11-unix（Xwayland 硬编码）
void probe_tmp_dir()
{
    int w = access("/tmp", W_OK);
    emit(std::string("P5 /tmp access(W_OK): ") + (w == 0 ? "OK" : "FAIL " + errno_name(errno)));
    errno = 0;
    int rc = mkdir("/tmp/.X11-unix", 0777);
    if (rc == 0) {
        emit("P5 mkdir /tmp/.X11-unix: OK (此前不存在, 已创建)");
    } else if (errno == EEXIST) {
        emit("P5 mkdir /tmp/.X11-unix: EEXIST(已存在)");
        int w2 = access("/tmp/.X11-unix", W_OK);
        emit(std::string("P5 /tmp/.X11-unix access(W_OK): ") + (w2 == 0 ? "OK" : "FAIL " + errno_name(errno)));
    } else {
        emit("P5 mkdir /tmp/.X11-unix: FAIL " + errno_name(errno));
    }
    int fd = open("/tmp/.winehua_probe_file", O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) {
        emit("P5 create file in /tmp: OK");
        close(fd);
        unlink("/tmp/.winehua_probe_file");
    } else {
        emit("P5 create file in /tmp: FAIL " + errno_name(errno));
    }
}
} // namespace

extern "C" __attribute__((constructor)) void RunSandboxProbe()
{
    char ctx[96];
    snprintf(ctx, sizeof(ctx), "uid=%d pid=%d", (int)getuid(), (int)getpid());
    emit(std::string("=== sandbox probe begin (") + ctx + ") ===");
    probe_shm_open();
    probe_memfd();
    probe_tmpfile();
    probe_unix_socket();
    probe_tmp_dir();
    emit("=== sandbox probe end ===");

    FILE *f = fopen(RESULT_PATH, "w");
    if (f) {
        fwrite(g_report.data(), 1, g_report.size(), f);
        fclose(f);
        OH_LOG_INFO(LOG_APP, "%{public}s result written to %{public}s", TAG, RESULT_PATH);
    } else {
        OH_LOG_WARN(LOG_APP, "%{public}s result file open failed: %{public}s", TAG,
                    errno_name(errno).c_str());
    }
}

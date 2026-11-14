// ============================================================================
// hgs_endpoint_test — socket 端点拆分/客户端环境纯函数对账 (阶段 0 P0.2)
//
// 测试对象: api/hgs_endpoint.{h,cpp} (SplitSocketPath/BuildClientEnvironment)。
// 黄金值 = phase0 §10.2 用例表; 拆分逻辑与 WaylandServer::Start 内部
// find_last_of 同源, 差异 (绝对路径/尾斜杠校验 + 根级 directory="/")
// 在此固化为契约。
//
// 构建: make test (host g++ 直连编译, 零平台依赖 — 纯标准库)。
// ============================================================================
#include "hgs_endpoint.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

static void expect_true(const char* what, bool cond) {
    checks++;
    if (!cond) printf("FAIL %s\n", what), failures++;
}

static void expect_eq(const char* what, const std::string& got,
                      const std::string& want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s\n  got : '%s'\n  want: '%s'\n",
               what, got.c_str(), want.c_str());
    }
}

static void test_valid_paths() {
    // 典型 WineHua 沙箱路径
    hgs::SocketEndpoint ep = hgs::SplitSocketPath("/data/storage/el2/base/.wine/wayland-0");
    expect_true("sandbox valid", ep.valid);
    expect_eq("sandbox dir", ep.directory, "/data/storage/el2/base/.wine");
    expect_eq("sandbox name", ep.displayName, "wayland-0");

    // 根级 socket: directory 规范化为 "/"
    ep = hgs::SplitSocketPath("/wayland-0");
    expect_true("root valid", ep.valid);
    expect_eq("root dir", ep.directory, "/");
    expect_eq("root name", ep.displayName, "wayland-0");

    // 多级目录 + 非 wayland- 命名
    ep = hgs::SplitSocketPath("/tmp/hgs-sock-test.sock");
    expect_true("tmp valid", ep.valid);
    expect_eq("tmp dir", ep.directory, "/tmp");
    expect_eq("tmp name", ep.displayName, "hgs-sock-test.sock");
}

static void test_invalid_paths() {
    const char* cases[] = {
        "",                  // 空串
        "wayland-0",         // 相对路径
        "./wayland-0",       // 相对路径 (点前缀)
        "/data/.wine/",      // 尾斜杠 (basename 为空)
        "/",                 // 纯根
    };
    for (const char* c : cases) {
        hgs::SocketEndpoint ep = hgs::SplitSocketPath(c);
        checks++;
        if (ep.valid) {
            failures++;
            printf("FAIL invalid '%s' accepted (dir='%s' name='%s')\n",
                   c, ep.directory.c_str(), ep.displayName.c_str());
        }
        // 无效时字段必须为空 (契约)
        checks++;
        if (!ep.directory.empty() || !ep.displayName.empty()) {
            failures++;
            printf("FAIL invalid '%s' left dirty fields\n", c);
        }
    }
}

static void test_client_environment() {
    std::vector<std::string> env = hgs::BuildClientEnvironment("/data/.wine/wayland-0");
    checks++;
    if (env.size() != 2) {
        failures++;
        printf("FAIL env size=%zu\n", env.size());
    } else {
        expect_eq("env[0]", env[0], "XDG_RUNTIME_DIR=/data/.wine");
        expect_eq("env[1]", env[1], "WAYLAND_DISPLAY=wayland-0");
    }

    env = hgs::BuildClientEnvironment("/wayland-0");
    expect_true("root env size==2", env.size() == 2);
    if (env.size() == 2) {
        expect_eq("root env[0]", env[0], "XDG_RUNTIME_DIR=/");
        expect_eq("root env[1]", env[1], "WAYLAND_DISPLAY=wayland-0");
    }

    // 无效路径 → 空 vector
    env = hgs::BuildClientEnvironment("relative");
    expect_true("invalid env empty", env.empty());
    env = hgs::BuildClientEnvironment("");
    expect_true("empty env empty", env.empty());
}

int main() {
    test_valid_paths();
    test_invalid_paths();
    test_client_environment();
    printf("hgs_endpoint_test: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}

// ============================================================================
// hgs_window_manager_test — 窗口管理器 core 行为对账 (阶段 2 W1)
//
// 测试对象: api/hgs_window_manager.{h,cpp} + hgs_window_policy.cpp。
// 覆盖: 创建/销毁/标题/几何/状态/可见性/焦点/raise/关闭请求/查询,
// host 与事件 sink 的调用方向 (core→host 操作 vs host→core 回执)。
//
// 构建: make test (host g++ 直连编译, 零平台依赖 — 纯 C++ core)。
// ============================================================================
#include "hgs_window_manager.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hgs;

static int failures = 0;
static int checks = 0;

static void expect_true(const char* what, bool cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void expect_eq_i(const char* what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got=%lld want=%lld\n", what, got, want);
    }
}

struct FakeHost : IWindowHost {
    int created = 0;
    int destroyed = 0;
    int geometry = 0;
    int visible = 0;
    int title = 0;
    int state = 0;
    int move = 0;
    int resize = 0;
    int raise = 0;
    int close = 0;
    WindowId lastId = 0;
    WindowSpec lastSpec;
    WindowGeometry lastGeometry;
    std::string lastTitle;

    void CreateWindow(const WindowSpec& spec) override {
        created++;
        lastSpec = spec;
        lastId = spec.id;
    }
    void DestroyWindow(WindowId id) override { destroyed++; lastId = id; }
    void SetGeometry(WindowId id, const WindowGeometry& g) override {
        geometry++; lastId = id; lastGeometry = g;
    }
    void SetVisible(WindowId id, bool v) override { visible++; lastId = id; (void)v; }
    void SetTitle(WindowId id, const std::string& t) override {
        title++; lastId = id; lastTitle = t;
    }
    void SetState(WindowId id, WindowState s) override { state++; lastId = id; (void)s; }
    void StartMove(WindowId id) override { move++; lastId = id; }
    void StartResize(WindowId id, uint32_t e) override { resize++; lastId = id; (void)e; }
    void RaiseWindow(WindowId id) override { raise++; lastId = id; }
    void RequestClose(WindowId id) override { close++; lastId = id; }
};

struct FakePolicy : IWindowPolicy {
    bool allowClose = true;
    int created = 0;
    int raised = 0;
    int focused = 0;
    WindowGeometry createdGeometry;

    void OnCreated(WindowSpec& spec) override {
        created++;
        createdGeometry = spec.geometry;
        spec.geometry.x = 7;  // 验证 policy 可改初始几何
    }
    void OnRaise(WindowId) override { raised++; }
    void OnFocus(WindowId) override { focused++; }
    void OnStateRequest(WindowId, WindowState) override {}
    void OnGeometryRequest(WindowId, const WindowGeometry&) override {}
    bool OnCloseRequest(WindowId) override { return allowClose; }
};

static WindowSpec MakeSpec(WindowId id, const char* title = "win") {
    WindowSpec s;
    s.id = id;
    s.title = title;
    s.clientPid = 100 + id;
    s.geometry = {10, 20, 640, 480};
    return s;
}

int main() {
    FakeHost host;
    auto policy = std::make_unique<FakePolicy>();
    FakePolicy* policyPtr = policy.get();

    std::vector<WindowEvent> events;
    HgsWindowManager mgr;
    mgr.SetHost(&host);
    mgr.SetPolicy(std::move(policy));
    mgr.SetEventSink([&events](const WindowEvent& e) { events.push_back(e); });

    // ---- 创建 ----
    mgr.OnWindowCreated(MakeSpec(1, "one"));
    expect_eq_i("create host", host.created, 1);
    expect_eq_i("policy created", policyPtr->created, 1);
    expect_eq_i("count", static_cast<long long>(mgr.WindowCount()), 1);
    expect_eq_i("focused after create", mgr.Focused(), 1);
    expect_true("z-order [1]", mgr.ZOrder() == std::vector<WindowId>{1});
    expect_true("event created", events.size() == 1 &&
                                 events[0].type == WindowEventType::Created);
    expect_eq_i("policy geometry x", policyPtr->createdGeometry.x, 10);
    WindowSpec snap;
    expect_true("get window", mgr.GetWindow(1, &snap) && snap.title == "one");

    // ---- 第二个窗口 / raise / focus ----
    mgr.OnWindowCreated(MakeSpec(2, "two"));
    expect_true("z-order [1,2]", mgr.ZOrder() == std::vector<WindowId>{1, 2});
    expect_eq_i("focused after second", mgr.Focused(), 2);
    mgr.Raise(1);
    expect_true("z-order after raise [2,1]", mgr.ZOrder() == std::vector<WindowId>{2, 1});
    expect_eq_i("host raise", host.raise, 1);
    expect_eq_i("policy raised", policyPtr->raised, 1);
    expect_true("event raise requested",
                events.back().type == WindowEventType::RaiseRequested);
    mgr.Focus(1);
    expect_eq_i("focused after Focus", mgr.Focused(), 1);
    expect_eq_i("policy focused", policyPtr->focused, 1);

    // ---- host 焦点回执 ----
    mgr.NotifyHostFocus(2, true);
    expect_eq_i("focused after host focus", mgr.Focused(), 2);
    expect_true("event focus", events.back().type == WindowEventType::FocusChanged &&
                               events.back().focused);
    mgr.NotifyHostFocus(2, false);
    expect_eq_i("focused cleared by host", mgr.Focused(), 0);

    // ---- 标题 ----
    mgr.OnTitleChanged(1, "renamed");
    expect_eq_i("host title", host.title, 1);
    expect_true("host last title", host.lastTitle == "renamed");
    expect_true("event title", events.back().type == WindowEventType::TitleChanged &&
                               events.back().spec.title == "renamed");

    // ---- 客户端几何变化: core -> host.SetGeometry ----
    const int geoBefore = host.geometry;
    mgr.OnClientGeometryChanged(1, {0, 0, 800, 600});
    expect_eq_i("client geom host call", host.geometry, geoBefore + 1);
    expect_true("event geom", events.back().type == WindowEventType::GeometryChanged &&
                              events.back().geometry.width == 800);
    // ---- host 回执几何: 不再回调 host ----
    const int geoAfter = host.geometry;
    mgr.NotifyHostGeometry(1, {5, 6, 810, 610});
    expect_eq_i("host geom no echo", host.geometry, geoAfter);
    expect_true("event host geom", events.back().geometry.x == 5);

    // ---- 状态 ----
    mgr.OnClientStateChanged(1, WindowState::Maximized);
    expect_eq_i("host state", host.state, 1);
    expect_true("event state", events.back().type == WindowEventType::StateChanged &&
                               events.back().state == WindowState::Maximized);
    const int stateAfter = host.state;
    mgr.NotifyHostState(1, WindowState::Normal);
    expect_eq_i("host state no echo", host.state, stateAfter);

    // ---- 可见性 ----
    mgr.SetVisible(2, false);
    expect_eq_i("host visible", host.visible, 1);
    expect_true("event visible", events.back().type == WindowEventType::VisibleChanged);
    expect_true("spec visible=false", mgr.GetWindow(2, &snap) && !snap.visible);

    // ---- 移动/缩放请求 ----
    mgr.RequestMove(1);
    expect_eq_i("host move", host.move, 1);
    expect_true("event move", events.back().type == WindowEventType::MoveRequested);
    mgr.RequestResize(1, 8);
    expect_eq_i("host resize", host.resize, 1);
    expect_true("event resize edges", events.back().edges == 8);

    // ---- 关闭请求 (允许) ----
    policyPtr->allowClose = true;
    mgr.RequestClose(1);
    expect_eq_i("host close allowed", host.close, 1);
    expect_true("event close", events.back().type == WindowEventType::CloseRequested);
    // ---- 关闭请求 (拒绝): host 不收到, 但观察者仍收到 ----
    policyPtr->allowClose = false;
    const int closeBefore = host.close;
    const size_t eventsBefore = events.size();
    mgr.RequestClose(1);
    expect_eq_i("host close denied", host.close, closeBefore);
    expect_true("event close still emitted", events.size() == eventsBefore + 1);

    // ---- host 创建失败回滚 (NotifyHostClosed): 不回调 host.DestroyWindow ----
    const int destroyBefore = host.destroyed;
    mgr.NotifyHostClosed(2);
    expect_eq_i("host destroy not called", host.destroyed, destroyBefore);
    expect_true("count after host closed", mgr.WindowCount() == 1);
    expect_true("event destroyed", events.back().type == WindowEventType::Destroyed);

    // ---- 正常销毁: 调 host.DestroyWindow ----
    mgr.OnWindowDestroyed(1);
    expect_eq_i("host destroy", host.destroyed, 1);
    expect_eq_i("count after destroy", static_cast<long long>(mgr.WindowCount()), 0);
    expect_eq_i("focused cleared", mgr.Focused(), 0);

    // ---- 未知 id 操作: no-op ----
    mgr.OnTitleChanged(999, "x");
    mgr.Raise(999);
    mgr.RequestClose(999);
    expect_eq_i("unknown no host calls", host.title, 1);
    expect_eq_i("count still 0", static_cast<long long>(mgr.WindowCount()), 0);

    // ---- ListWindows 顺序 = z-order ----
    mgr.OnWindowCreated(MakeSpec(10, "a"));
    mgr.OnWindowCreated(MakeSpec(11, "b"));
    std::vector<WindowSpec> list = mgr.ListWindows();
    expect_true("list size", list.size() == 2);
    expect_true("list order", list.size() == 2 && list[0].id == 10 && list[1].id == 11);

    printf("hgs_window_manager_test: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}

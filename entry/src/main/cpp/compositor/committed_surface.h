#pragma once
#include <cstdint>

// CommittedSurface — wl_core.cpp 协议壳 commit 产物的命名快照。
//
// 出处 (PLAN 索引): docs/COMPOSITOR_REFACTOR_PLAN.md §二关键契约 ("commit 产出
// 不可变 CommittedSurface 快照 (role/contentRect/screenPos/parentOffset 命名字段,
// geoX/geoY 三义在此消亡)") + §四阶段5 第 1 条后半 (5A2 步的目标)。
//
// -- geoX/geoY 三义消亡 (PLAN §2.4 "一名多义") --
// SurfaceData::geoX/geoY 曾一身三义, 靠 hasToplevel+模式在运行时分流
// (wl_core.cpp ComputeContentArea, 5A1 步已参数化):
//   1. toplevel 桌面模式: 虚拟桌面屏幕坐标
//   2. toplevel PC 模式:  窗口内容偏移 (首帧位置/argb move, 后续 commit 由
//      OHOS 窗口管理器为权威而忽略)
//   3. subsurface:        buffer 内的内容偏移 (window_geometry 的 x/y)
// 重构后三义各自有命名字段, 消费者按字段名取义, 不再靠外部状态猜义:
//   - screenPos   = 义 1 (toplevel 的虚拟桌面屏幕位置; 义 2 的首帧/移动
//                   也经此字段 — 与 5A1 的 ShmCommitInfo::screenX/Y 同源)
//   - contentRect = 义 3 及几何原值 (buffer 内内容矩形, window_geometry 的
//                   x/y/w/h 原值; toplevel 时 义 1/2 与其 x/y 同值)
//   - role        = 角色判定显式化 (Toplevel/Subsurface/Plain), 取代
//                   hasToplevel 猜义分流
//
// -- 填充纪律 (行为平价) --
// 提交 1 (5A2·1/2): commit 管线在旧字段照旧填的同时并行填充本快照 — 快照值
// 与旧字段同一次计算的两种表达, 不做第二套独立算法; 若某字段快照计算与旧
// 填值不同即实现错误。旧读取路径零改动。
// 提交 2 (5A2·2/2): 消费端全部切换到本快照; sd->geoX/geoY/geoW/geoH 删除,
// 几何写点 (xdg_shell 的 set_window_geometry) 直写 contentRect/hasWindowGeometry,
// role 随协议角色即时同步 — 与旧"写 geo 字段 → 读 geo 字段"值流逐点等价。
//
// -- 线程/锁 --
// 本结构各字段由 Wayland 事件循环线程 (wl 回调) 独占访问 — 与 SurfaceData
// 其余字段的线程域一致, 不新增锁。不是并发共享对象 (compositor 读点经
// SurfaceData* 在已有锁域下访问)。
//
// 零依赖约定: 本文件不 include wayland/hilog, 纯 POD — host_tests 可直连
// 编译 (当前无对应测试: 本结构无独立算法, 内容为既有计算/字段值的拷贝, 由
// 消费点切换的对账保证等价)。

struct CommittedSurface {
    // -- 角色显式化 (旧 hasToplevel/isSubsurface 猜义判定的命名替代) --
    enum class Role : uint8_t { Plain, Subsurface, Toplevel };
    Role role = Role::Plain;

    // -- 几何三义命名替换 --
    //
    // contentRect: window_geometry 内容矩形 (buffer 内"可见内容", 旧
    // geoX/geoY/geoW/geoH 的原值表达; hasWindowGeometry=false 表示未设置,
    // 语义为"全 buffer", 矩形字段无义)。对 toplevel: 义 1/2 的屏幕位置
    // 另见 screenPos, 本矩形的 x/y 保留 window_geometry 原值 (同一值两种义,
    // 与旧字段表现一致); 对 subsurface: 义 3 (buffer 内内容偏移)。
    // 写点: xs_set_window_geometry (提交 2 起直写本字段, 与旧 geo 字段
    // 同一写入点, 即时生效 — 消费端读父 surface 时值与旧实现逐点相同)。
    bool hasWindowGeometry = false;
    struct ContentRect {
        int32_t x = 0, y = 0, w = 0, h = 0;
    };
    ContentRect contentRect;

    // screenPos: toplevel 的虚拟桌面屏幕位置 (义 1; PC 模式首帧/argb_move
    // 同源 = ShmCommitInfo::screenX/Y 的同步表达)。仅 role==Toplevel 且有
    // window_geometry 且尺寸 > 0 时有义 (与 ComputeContentAreaGeometry 的
    // screenX/screenY 赋值条件逐字一致); 其余情况取 0。
    int32_t screenX = 0, screenY = 0;

    // parentOffset: subsurface 相对父 surface 原点的偏移 (义 3 的
    // wl_subsurface.set_position 参数 = 旧 SurfaceData::subsurfaceX/Y 的
    // commit 时点快照)。role==Subsurface 时有义; 只生产暂未消费
    // (消费点在 popup 偏移公式单点化时接入, 见 PLAN §四阶段5 PopupManager)。
    int32_t parentOffsetX = 0, parentOffsetY = 0;

    // -- frame: 本 commit 的帧属性 (commit 产物, 与 SurfaceData 帧字段同值) --
    int32_t w = 0, h = 0;             // content 显示尺寸 (与 SurfaceData::w/h 同值)
    uint64_t shmCommitSerial = 0;     // 本 commit 序列号 (ZC fallback 判定)
    uint32_t shmFormat = 1;           // wl_shm format (0=ARGB8888, 1=XRGB8888)
    // 累积 damage 包围盒 (buffer 坐标, 与 SurfaceData::damage* 同值)
    int32_t damageX = 0, damageY = 0, damageW = 0, damageH = 0;

    // 像素不进快照: 消费端仍经 SurfaceData::pixels 访问 (快照是帧元数据,
    // 像素属可变大数据, 拷贝即行为变化/开销, 见 5A1 的 ShmCommitInfo 同样决策)。
};

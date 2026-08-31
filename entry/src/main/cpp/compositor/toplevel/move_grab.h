#pragma once
#include <wayland-server-core.h>
#include <cstdint>
#include <mutex>

class ToplevelManager;

// 交互式窗口移动 (xdg_toplevel.move)。
// 状态由自身持有，读写 toplevel 位置需通过 ToplevelManager 引用。
class MoveGrabHandler {
public:
    // 开始抓取。serial 是 xdg_toplevel.move 携带的序列号。
    // grabGlobalX/Y: 当前指针在 grab 输入空间的绝对坐标 (desktop = 桌面
    // 逻辑坐标, PC = 局部坐标+窗口位置还原), 由 WaylandServer 从
    // InputManager 获取 — 立即算固定 grab 偏移, 后续 motion 绝对定位
    void StartMoveGrab(ToplevelManager& tmgr, uint32_t toplevelId, uint32_t serial,
                       int32_t grabGlobalX, int32_t grabGlobalY);

    // 结束抓取, 清空 grab 状态 (未在抓取时调用安全, 仅清零)。
    void EndMoveGrab(ToplevelManager& tmgr);

    // 处理移动。返回 true 表示事件被 grab 消费。
    // gx/gy 为桌面全局坐标 (InputManager 在 grab 期间直接注入全局坐标,
    // 不再做 全局→局部→+st->x 还原 的往返 — 那个往返在双线程下基准漂移,
    // 会把窗口位移变成指针位移的累积和, 快速拖动时爆炸)。
    // 每帧绝对定位 (窗口位置 = 全局坐标 - 固定偏移), 无累积。
    bool ProcessMoveGrabMotion(ToplevelManager& tmgr, int32_t gx, int32_t gy);

    bool IsActive() const { return toplevelId_ != 0; }
    uint32_t GetToplevelId() const { return toplevelId_; }
    uint32_t GetSerial() const { return serial_; }

private:
    uint32_t toplevelId_ = 0;
    uint32_t serial_ = 0;
    int32_t grabOffX_ = 0;
    int32_t grabOffY_ = 0;
};

#pragma once
#include <cstdint>

// ============================================================================
// PresentedFrame — 帧交付契约 (重构第 2B 步)
//
// DesktopCompositor::TakeToplevelFrame 的交付物。此前帧以裸 (out, w, h)
// 返回: 有时是 1400x920 桌面合成帧, 有时是 800x600 游戏直传帧, 没有任何
// 字段说明帧的坐标空间/内容尺寸/alpha 语义, 消费方只能按尺寸猜 — 已实证
// 事故: 红警2 直传时渲染 letterbox 锚在游戏帧尺寸上, 输入逆映射二次缩放
// 导致主菜单永远点不中; frameArgb_ 按 root 格式反查, 帧语义与交付物脱钩。
//
// 契约规则: 产出侧 (FramePlanner 各出口 / 窗口帧路径) 必须把每个字段填对;
// 消费方 (egl_renderer 上屏 / input_manager 坐标逆映射) 一律从字段取几何,
// 不再反查 toplevel 格式或重算坐标空间。
// ============================================================================
struct PresentedFrame {
    // 帧来源: Composed = compositor 合成输出 (桌面合成帧 / 无子窗口快进帧 /
    // PC 窗口帧); DirectPass = SHM 全屏直传帧 (游戏层原始像素, 跳过合成)。
    enum class Kind { Composed, DirectPass };
    // 帧作为输出画面所在的坐标空间:
    // - Desktop = 帧即整屏桌面输出 (桌面合成/快进/全屏直传), 输入逆映射
    //   锚定桌面逻辑尺寸 (contentW/H);
    // - Window = 帧是单窗口内容 (PC 窗口帧), 输入锚定窗口内容尺寸。
    enum class BaseSpace { Desktop, Window };

    Kind kind = Kind::Composed;
    BaseSpace baseSpace = BaseSpace::Desktop;
    const uint8_t* pixels = nullptr;  // 帧像素 (XRGB/ARGB 8888), 指向调用方 out 缓冲
    int w = 0, h = 0;                 // buffer 尺寸 (像素)
    // baseSpace 坐标空间的逻辑内容尺寸 (输入逆映射锚): 桌面空间 = root 逻辑
    // 尺寸; 窗口空间 = 窗口内容尺寸。区别于 buffer 尺寸 — 直传帧 buffer 是
    // 游戏内容尺寸 (如 800x600), 显示/输入锚仍是桌面尺寸 (如 1400x920);
    // buffer 含对齐填充或经 viewport 缩放时两者同样不同。
    int contentW = 0, contentH = 0;
    // 帧按不透明呈现: true = alpha 通道无意义/被忽略 (免预乘/免混合优化用)。
    // 产出规则 = 交付帧所属 toplevel 的 wl_shm 格式为 XRGB; 直传帧同理
    // (直传门控要求 root XRGB, 渲染器对直传帧强制不透明呈现)。
    bool opaque = false;
};

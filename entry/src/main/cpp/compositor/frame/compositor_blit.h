#pragma once
#include <cstdint>

// 像素合成原语 (双线性缩放 / 单行混合)。纯函数无 wayland/OHOS 依赖 —
// 独立成文件让宿主机单元测试 (host_tests, make test) 直接编译真实实现。
// 这两支是桌面合成的热路径 (全屏游戏 fit 放大), 性能与逐字节语义
// (与 BlitClipAlpha 混合语义的一致性) 都是测试关注点。

// 双线性缩放 blit (16.16 固定点)。clipX/Y/W/H: 可选目标裁剪矩形 (root 坐标,
// 0/0/0/0 = 不裁剪)。裁剪只约束写入范围, 采样相位仍由 dstX/dstY/dstW/dstH
// 整矩形决定 — 局部合成 (DamageRect) 与整帧合成逐像素一致。
void BlitScaled(uint8_t* dst, int rootW, int rootH,
                const uint8_t* src, int srcStride, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, bool alphaBlend,
                int clipX = 0, int clipY = 0, int clipW = 0, int clipH = 0);

// 像素混合的两种历史语义 (收敛自三份内联行循环, 保留差异 — 见各调用点):
// SrcOnly: 源不乘 alpha 直接叠加到背景, 结果 clamp 到 255, 目标 alpha
//   强制 255 — blitToplevel 普通分支 (窗口帧对半透明底色的处理);
// Normal: 标准 alpha 混合 (src*a + dst*inv)/255, 结果为加权平均不超 255,
//   目标 alpha 不变 — blitSubsurface / blitWindowSubsurface 普通分支。
enum class PixelBlend { SrcOnly, Normal };

// 单行 ARGB 拷贝/混合。dstRow/srcRow 已对齐到裁剪后的行起点, copyW 为
// 裁剪后像素数。alphaBlend=false → 整行不透明 memcpy (含 alpha 通道)。
void BlitClipAlpha(uint8_t* dstRow, const uint8_t* srcRow, int copyW,
                   bool alphaBlend, PixelBlend blend);

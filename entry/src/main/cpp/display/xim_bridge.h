#ifndef WINEHUA_XIM_BRIDGE_H
#define WINEHUA_XIM_BRIDGE_H

/* XIM 桥主进程侧通道 (spec docs/superpowers/specs/2026-10-07-display-route-x11-xim-server-design.md §3 NCP 承载)。
 * XIM server 本体在 NCP 子进程 (xim_server_child.cpp); X1 实测主进程 X
 * 不可达 (提交 1392d60), 本文件只管 socketpair 通道。
 * 线程纪律: send_text 任意线程可调 (内部互斥); channel_init 在 spawn 前
 * (合成器线程) 调一次。 */
#ifdef __cplusplus
extern "C" {
#endif

/* 创建通道对, 返回子端 fd (调用方随 NCP spawn 以 fdName "xim_fd" 传递,
 * 所有权转移); 失败返回 -1。 */
int xim_bridge_channel_init(void);

/* 转投 UTF-8 文本 (smoke ime / 产品 text-input), 子进程未就绪时 -1。 */
int xim_bridge_send_text(const char *utf8);

#ifdef __cplusplus
}
#endif
#endif /* WINEHUA_XIM_BRIDGE_H */

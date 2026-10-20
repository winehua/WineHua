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

/* 上行激活通知 (x11 IME 上行链, 2026-10-17): server 侧 IC 焦点变化
 * (XIM_SET_IC_FOCUS / DESTROY_IC / DISCONNECT) 时回调。active=1 应弹
 * 软键盘 (attach), 0 应收起 (detach)。rect 为光标矩形 (v1 传零, 不影响
 * attach)。回调在桥读线程上下文执行, 实现方须自行转投线程安全队列。 */
typedef void (*xim_bridge_activation_cb)(int active, int x, int y,
                                         int w, int h, void *user);

/* 注册上行激活回调并确保读线程在跑 (幂等)。channel_init 前后均可调;
 * server 侧无通知 (旧构建 / 未聚焦) 时不触发。 */
void xim_bridge_set_activation_callback(xim_bridge_activation_cb cb,
                                        void *user);

#ifdef __cplusplus
}
#endif
#endif /* WINEHUA_XIM_BRIDGE_H */

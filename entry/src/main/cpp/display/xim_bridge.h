#ifndef WINEHUA_XIM_BRIDGE_H
#define WINEHUA_XIM_BRIDGE_H

/* x11 路线 XIM server 桥 (spec docs/superpowers/specs/2026-10-07-display-route-x11-xim-server-design.md)。
 * 链启动后调用; 结果与状态走 hilog (tag: xim-bridge)。
 * 线程纪律: 桥内 Xlib 连接仅在合成器 loop 线程使用, 跨线程请求经
 * display_input 注入队列转交 (同 D19 剪贴板桥纪律)。 */
#ifdef __cplusplus
extern "C" {
#endif

/* X1 探针 (计划 Task 1): 验证 app 主进程 XOpenDisplay(":0") 可行 + root
 * window XIM_SERVERS property 写入成功。每次链启动都跑, 结果只进 hilog
 * (不做标记文件缓存——部署代际无法感知)。返回 0 = 探针已发起。 */
int xim_bridge_probe_start(void);

#ifdef __cplusplus
}
#endif
#endif /* WINEHUA_XIM_BRIDGE_H */

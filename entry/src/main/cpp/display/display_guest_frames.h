/*
 * display_guest_frames.h — M2-T5: guest Vulkan 帧 (Venus 私有 present) 接收侧
 *
 * 生产者是宿主 virgl/vtest 侧: guest 进程 win32u 私有 present 把 Venus 镜像
 * 按 (clientPid << 32) | SURFACE_ID 路由进 SurfaceQueuePresenterManager, 无
 * 目标就丢弃并等目标上线 (virgl_surface_presenter.cpp)。
 *
 * 本模块是接收侧: 把 SURFACE_ID (= X 顶层窗 id, 见 wine fork
 * winex11.drv/vulkan.c) 与合成器在册的 X 窗对齐 —— 每个这样的窗建一个消费者
 * 面 (OH_ConsumerSurface), 生产窗交给 broker 绑定, 之后每帧取一格队列 buffer
 * 挂到该窗的 scene 节点上 (压在同窗 X 面之上, 见 ohos_output.c)。
 *
 * 线程: 全部在 wlroots event loop 线程 (帧时钟内), 无自有线程/锁 —— 消费者面
 * 的取帧走轮询而不是 OnFrameAvailable 回调, 回调线程与 scene 不同源。
 *
 * 销毁即失效: X 窗销毁后同 id 会随复用的新窗重新在册, 这条缝里到达的帧既不
 * 进旧窗 (已摘) 也不进新窗 (无绑定): 宿主侧按「无目标」丢弃, 本模块记
 * destroyed/stale 计数 (hilog 限频), 不解绑就复挂。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 帧时钟调用 (与 scene 提交同线程, 33ms 一拍)。
void display_guest_frames_tick(void);

// 合成器收尾: 先摘帧 (把借来的队列 buffer 还回去), 再解绑, 最后销毁消费者面
// —— 顺序不能反, 面销毁后归还 buffer 是悬垂调用。幂等。
void display_guest_frames_shutdown(void);

#ifdef __cplusplus
}
#endif

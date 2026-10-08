/**
 * display-route 控制面的产品接口 (libdisplay_route_napi.so)。
 * 语义详见 display_route/display_route_napi.cpp 头注释。
 */

/**
 * 启动 x11 出图链。surfaceId 非零 = 同步绑 XComponent → NativeWindow 直推;
 * scriptEnabled = 真机门自动注入脚本 (测试编排专用, 产品恒 false);
 * outW/outH = x11 台架输出尺寸 (缺省 0 ⇒ 800x600);
 * canvasEglPresent = 画布绑定走 EGL swap (语义见 ohos_output.h)。
 * 无参调用 = 不绑 surface 启动链。
 */
export const startDisplayRouteChain: (surfaceId?: bigint, scriptEnabled?: boolean,
  outW?: number, outH?: number, canvasEglPresent?: boolean) => boolean;

/** 停止出图链。 */
export const stopDisplayRouteChain: () => boolean;

/** 路线选择 ("fusion"/"x11")。 */
export const setDisplayRoute: (route: string) => boolean;

/** 进程内路线镜像读取口 (wine_env.cpp 唯一写点): true = x11 路线。
 *  smoke 注入编排按它分叉注入通道。 */
export const displayRouteIsX11: () => boolean;

/** X 屏幕逻辑尺寸 (guest 桌面坐标系; 链未起时 0x0)。
 *  guest 桌面坐标 → injectDisplayRouteMotion 归一化坐标的换算分母。 */
export const displayRouteFrameSize: () => { w: number, h: number };

/** 画布尺寸动态响应 (折叠/旋转后 onSurfaceChanged 的逻辑尺寸)。 */
export const resizeDisplayRouteOutput: (w: number, h: number) => boolean;

/** evdev 键码注入 (KEY_A=30)。 */
export const injectDisplayRouteKey: (keycode: number, press: boolean) => boolean;

/** 指针注入。nx/ny = 帧归一化坐标 0..1 (X 屏幕逻辑尺寸即 guest 桌面坐标系,
 *  桥内换算: 帧 px → topmost_at 命中 → 窗口局部坐标; 虚拟桌面里命中桌面
 *  顶层窗后由 wine 自行路由子窗)。phase: 0=enter 1=motion 2=leave。 */
export const injectDisplayRouteMotion: (nx: number, ny: number, phase: number) => boolean;

/** 按钮注入 (BTN_LEFT 0x110 / BTN_RIGHT 0x111 / BTN_MIDDLE 0x112)。 */
export const injectDisplayRouteButton: (button: number, press: boolean) => boolean;

/** 轴注入 (D15 手势层): which 0=纵向 1=横向; steps=±N discrete 步,
 *  discrete>0 → 滚轮向下 (与 wayland 分支「向上=正」同号)。 */
export const injectDisplayRouteAxis: (which: number, steps: number) => boolean;

/** 文本上屏注入 (XIM Task 4): x11 路线 IME commit —— UTF-8 串经 xim bridge
 *  以 XIM_COMMIT 投给 wine 焦点 IC。Wayland 路线的上屏走 text-input, 不经此。 */
export const injectDisplayRouteText: (text: string) => boolean;

/** D19 CJK 剪贴板桥: UTF-8 串设为 seat selection (xwm 桥接 Xwayland CLIPBOARD),
 *  配合 Ctrl+V 注入粘贴。 */
export const setDisplayRouteClipboard: (text: string) => boolean;

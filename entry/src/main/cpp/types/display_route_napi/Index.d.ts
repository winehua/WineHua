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

/** 画布尺寸动态响应 (折叠/旋转后 onSurfaceChanged 的逻辑尺寸)。 */
export const resizeDisplayRouteOutput: (w: number, h: number) => boolean;

/** evdev 键码注入 (KEY_A=30)。 */
export const injectDisplayRouteKey: (keycode: number, press: boolean) => boolean;

/** 指针注入。nx/ny = client surface 相对坐标 0..1; phase: 0=enter 1=motion 2=leave。 */
export const injectDisplayRouteMotion: (nx: number, ny: number, phase: number) => boolean;

/** 按钮注入 (BTN_LEFT 0x110 / BTN_RIGHT 0x111 / BTN_MIDDLE 0x112)。 */
export const injectDisplayRouteButton: (button: number, press: boolean) => boolean;

/** D19 CJK 剪贴板桥: UTF-8 串设为 seat selection (xwm 桥接 Xwayland CLIPBOARD),
 *  配合 Ctrl+V 注入粘贴。 */
export const setDisplayRouteClipboard: (text: string) => boolean;

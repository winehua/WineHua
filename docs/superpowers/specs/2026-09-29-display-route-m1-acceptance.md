# Display-Route M1 验收报告（2026-09-29）

设计 spec：`2026-09-27-display-route-x11-wlroots-design.md`（§6.3 = M1 实测结论，
本文只记判据裁定与 M2 入口，不重复数据）。分支 `feature/display-route-m0`
（M0/M1 同分支续推，用户裁定）。

## 判据裁定

| # | 判据 | 来源 | 裁定 | 证据 |
|---|---|---|---|---|
| 1 | winex11 交叉接入（guest X11 栈 + 驱动选择 DISPLAY 分支） | T4 | **过** | wine `--with-x` 编入，`WINEHUA_DISPLAY_ROUTE=x11` + 清 `WAYLAND_DISPLAY` 走 winex11；x11app/winedbg 等 X client 真机连接 Xwayland |
| 2 | 窗口语义（surface 列表 + 命中测试 + 焦点随动） | T2 | **过** | scene 化合成，键语义 a/win1 b/win2 c/win1 精确复测；模态对话框几何无关聚焦 |
| 3 | notepad 类输入/窗口正确（M1 出口判据） | T5 | **过** | "hi" 键入进编辑区端到端截图（winex11→Xwayland→xwm→scene→NativeWindow）；retrigger 全链复通；伴随 core 4/4 PASS ×2 |
| 4 | GLX-over-EGL 桥最小原型裁决 | T6 | **过（改判）** | ①桥 no-go（回读 19.2MB/s，800×600 全帧 96.3ms≈10fps）；改判走 wine 原生 winex11 GLX/drisw（6.5ms≈150fps）。自研桥取消，未付出 opengl.c 改造代价 |
| 5 | R-WSI 探针 | T7 | **过（判否）** | venus 不暴露 VK_KHR_xcb_surface/xlib ⇒ X 路线无 Vulkan present，走 M2 重锚 |
| 6 | 合成帧率 ≥25fps | T3 | **未过（按预案降级）** | 18.6-18.7fps 持续（M0 的 2.2 倍）；瓶颈 = NativeWindow BufferQueue copy+flush，零拷贝归 M2 present 重构（brief Step 3 预案允许数据回填+残留后移） |
| 7 | 自动化套件不回退（§8.2） | T8 | **过** | core 4/4 + wine-vulkan 4/4（冷启干净态）；displayroute 会话残留会污染后续套件采集（t5v/T8 两度复现），回归前 force-stop+冷启动为既定流程 |

**M1 总体：达成**（6/7 过；#6 帧率按计划内预案降级，残留归 M2 零拷贝）。

## 挂起项（随分支携带，非阻塞）

- 最后一键间歇不入编辑控件（注入侧日志同形，wine 内部非确定路径）
- 引擎冷启 wineboot 偶发 box64 SIGSEGV 崩溃循环（重试即过，未定性）
- 两 GL 路线 renderer 同串但回读带宽差 16 倍的机制未解释（drisw 本地 shadow vs vtest 往返，嫌疑）
- hdc -b 热更 payload 与 app 挂载视图按文件不一致（t6d-f 实锤；出口 = 完整 deploy；根因未追）

## M2 入口建议（优先级序）

1. **present 重构（零拷贝）**——帧率残留（18.6→≥25fps）的唯一出口，也是 R-ZC ② GL 门的同一战场；T3 的分段遥测已留在代码里作前后对照基线
2. **winex11 GLX/drisw 可见窗 present 链验证**——T6 改判路线的收口；通过则 OpenGL 程序可整切 X 路线
3. **Venus/ZC 重锚**（§4.2，SURFACE_ID ← X window id 经 `wlr_xwayland_surface` 双字段映射）——R-WSI 判否后 X 路线 Vulkan 呈现的唯一路径；与 1 同链路，建议合并实施
4. IME（R-IME）与双窗口形态（PC 模式 surface→output 绑定）随后

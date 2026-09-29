/*
 * ohos_egl_import_probe.h — M2-T2 R-ZC ② 探针 (实现见 .c 头注释)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 合成器 bring-up 时调: 标记文件不存在才真跑 (自缓存), 结论落
 * drive_c/displayroute-egl-import-probe + hilog。线程约束: 合成器
 * 线程 (串行于 renderer 初始化之前, 独占 EGL 默认 display)。 */
void ohos_egl_import_probe_run(void);

#ifdef __cplusplus
}
#endif

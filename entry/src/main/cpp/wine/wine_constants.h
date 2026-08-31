#ifndef WINE_CONSTANTS_H
#define WINE_CONSTANTS_H

// -- WineHua 全局路径常量 --
// OHOS 应用 sandbox 基础路径
#define WINE_FILES_DIR       "/data/storage/el2/base/files"

// Wine prefix (Wine 运行时数据: registry, drive_c, .wineserver socket)
#define WINE_PREFIX          "/data/storage/el2/base/files/.wine"

// Wine 运行时解压目录
#define WINE_RUNTIME_ROOT    "/data/storage/el2/base/files/wine"
#define WINE_RUNTIME_BIN     WINE_RUNTIME_ROOT "/bin"

// Broker Unix socket 路径 (主进程 <-> 子进程通信)
#define WINE_BROKER_SOCKET   "/data/storage/el2/base/files/.wine_broker"

// Wine 临时文件目录 (TMPDIR)
#define WINE_TMPDIR          "/data/storage/el2/base/cache"

// Wine 子进程 stderr 日志目录
#define WINE_LOG_DIR         "/data/storage/el2/base/temp"

// -- Wine 产物架构子目录 (由 wine 架构 WINE_ARCH 决定, 非设备架构) --
// 方案① (x86_64 设备) / 方案② (arm64 设备 + x86_64 wine) → x86_64-*;
// 方案③ (arm64 设备 + arm64 原生 wine) → aarch64-*。
// 判定: 方案②由 CMakeLists.txt 按 entry/.wine_arch 注入 WINEHUA_WINE_ARCH_IS_X86_64;
// 方案① (x86_64 设备) 由 __x86_64__ 编译期兜底 — 不依赖 .wine_arch 文件存在
// (DevEco Studio 直编 / 绕过 package.sh 跑 hvigor 时该文件可能缺失)。
#if defined(__x86_64__) || defined(WINEHUA_WINE_ARCH_IS_X86_64)
#define WINE_UNIX_SUBDIR     "x86_64-unix"
#define WINE_PE_SUBDIR       "x86_64-windows"
#define WINE_WINE_ARCH       "x86_64"
#else
#define WINE_UNIX_SUBDIR     "aarch64-unix"
#define WINE_PE_SUBDIR       "aarch64-windows"
#define WINE_WINE_ARCH       "aarch64"
#endif

#endif // WINE_CONSTANTS_H

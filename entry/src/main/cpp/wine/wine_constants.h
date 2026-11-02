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

// x86_64 设备 bundle 原生库目录 (el1), wine unix .so 的权威加载源。
// 双架构 HAP 的 wine-data.zip 是超集布局, el2 解压出 x86_64-unix/ 副本,
// x86_64 设备的 LD_LIBRARY_PATH 必须以本目录优先防 el2 副本遮蔽 ——
// el2 数据区系统 dlopen 在 OHOS x86_64 未证实可用 (模拟器 ENOENT 实锤),
// arm64 设备不经过系统 linker (Box64 自带 ELF loader), 不受此约束。
#define WINEHUA_EL1_LIBS_X86_64 "/data/storage/el1/bundle/libs/x86_64"

#endif // WINE_CONSTANTS_H

#!/usr/bin/env bash
# Wine HarmonyOS musl 适配补丁集
#
# 用法:
#   WINE_SRC=/path/to/wine bash patches.sh
#
# 设计:
#   - 每条 patch 一个函数, 自带原因说明
#   - 通过源码内标记注释判断是否已打过, 幂等
#   - 任一条失败立即退出 (set -e)
#
# 参考: Box64 OHOS 移植/scripts/patches.sh

set -e

: "${WINE_SRC:?WINE_SRC 环境变量未设置 (应指向 wine 源码目录)}"

if [ ! -d "$WINE_SRC" ]; then
    echo "ERROR: WINE_SRC 目录不存在: $WINE_SRC"
    exit 1
fi

_patch_header() {
    printf '    [#%-2s] %-50s %s\n' "$1" "$2" "$3"
}

_already() {
    [ -f "$1" ] && grep -q "$2" "$1"
}

# ================================================================
# Patch 01 — include/config.h: 添加 musl 特定的 HAVE_ 宏
# ================================================================
# 目的:
#   musl 缺少 dladdr1 和 dlinfo, 但 Wine 需要 fallback 路径。
#   在 config.h 中明确定义这些宏, 让源代码走正确的分支。
#
#   同时添加 PACKAGE_STRING 等 autotools 标准宏。
patch_01_config_h() {
    local f="$WINE_SRC/include/config.h"
    local mark='OHOS_PATCH_CONFIG'

    [ -f "$f" ] || { echo "ERROR: config.h 不存在, 需要先运行 configure"; return 1; }
    if _already "$f" "$mark"; then
        _patch_header 01 "include/config.h" "already patched"
        return 0
    fi
    _patch_header 01 "include/config.h" "add musl-specific defines"

    cat >> "$f" << 'EOF'
/* OHOS_PATCH_CONFIG — musl-specific compatibility defines */

/* musl does not have dladdr1 or dlinfo — force fallback paths */
#undef HAVE_DLADDR1
#undef HAVE_DLINFO

/* musl uses libc.so, not libc.so.6 */
#define SONAME_LIBPTHREAD "\"libc.so\""

/* Package info (autotools standard, needed by main.c) */
#ifndef PACKAGE_STRING
#define PACKAGE_STRING "wine 10.0-ohos"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "10.0"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "wine"
#endif

/* OHOS_PATCH_CONFIG END */
EOF
}

# ================================================================
# Patch 02 — libs/winecrt0/dll_soinit.c: 添加 musl link_map fallback
# ================================================================
# 问题:
#   代码 fallback 链: dladdr1 → dlinfo → (缺失)
#   musl 两者都不提供, 需要新增第三个 fallback。
#
# 修法:
#   使用 _DYNAMIC 配合 dl_iterate_phdr 获取 link_map
patch_02_dll_soinit() {
    local f="$WINE_SRC/libs/winecrt0/dll_soinit.c"
    local mark='OHOS_PATCH_DLL_SOINIT'

    [ -f "$f" ] || { _patch_header 02 "dll_soinit.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 02 "libs/winecrt0/dll_soinit.c" "already patched"
        return 0
    fi
    _patch_header 02 "libs/winecrt0/dll_soinit.c" "add musl link_map fallback"

    # 在 #else 分支添加 __MUSL__ 路径
    # 查找关键 fallback 点并注入 musl 代码
    local pattern='defined(HAVE_DLINFO)'
    if grep -q "$pattern" "$f"; then
        sed -i "s|#elif $pattern|/* $mark */\n#elif $pattern|" "$f"

        # 在 HAVE_DLINFO 的 else 分支添加 musl fallback
        # 使用 sed 在适当位置插入 _DYNAMIC-based 解决方案
        python3 - "$f" "$mark" << 'PY'
import sys, re
fname, mark = sys.argv[1], sys.argv[2]
with open(fname) as f:
    content = f.read()

# Find the dlinfo RTLD_DI_LINKMAP usage and add musl fallback
# The pattern is: dlinfo(RTLD_SELF, RTLD_DI_LINKMAP, &map)
old = 'dlinfo( RTLD_SELF, RTLD_DI_LINKMAP, &map )'
new = '''/* %s: musl has no dlinfo, use dl_iterate_phdr */
#ifdef __MUSL__
    {
        extern char **environ;
        /* Use dl_iterate_phdr to find our own link_map */
        dl_iterate_phdr([](struct dl_phdr_info *info, size_t size, void *data) -> int {
            struct link_map **lmap = (struct link_map **)data;
            if (info->dlpi_name && info->dlpi_name[0] == '\\0') {
                /* main executable — use its l_addr as a base */
                *lmap = (struct link_map *)(info->dlpi_addr);
            }
            return 0;
        }, &map);
    }
#else
    dlinfo( RTLD_SELF, RTLD_DI_LINKMAP, &map )
#endif''' % mark
content = content.replace(old, new)
with open(fname, 'w') as f:
    f.write(content)
print("OK")
PY
    fi
}

# ================================================================
# Patch 03 — libs/vkd3d/libs/vkd3d/utils.c: program_invocation_name
# ================================================================
# 问题:
#   program_invocation_name 是 glibc 特有全局变量, musl 不提供
#
# 修法:
#   用 #ifdef __GLIBC__ 守卫, musl 上从 /proc/self/exe 读取
patch_03_program_invocation_name() {
    local f="$WINE_SRC/libs/vkd3d/libs/vkd3d/utils.c"
    local mark='OHOS_PATCH_PROG_INV_NAME'

    [ -f "$f" ] || { _patch_header 03 "vkd3d/utils.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 03 "libs/vkd3d/utils.c" "already patched"
        return 0
    fi
    _patch_header 03 "libs/vkd3d/utils.c" "add musl program_invocation_name fallback"

    # 在文件顶部添加 conditional 定义
    sed -i "1i\\
/* $mark */\\
#ifdef __MUSL__\\
# include <unistd.h>\\
# include <limits.h>\\
static char ohos_prog_name[PATH_MAX];\\
static const char *get_program_name(void) {\\
    if (!ohos_prog_name[0]) {\\
        ssize_t n = readlink(\"/proc/self/exe\", ohos_prog_name, sizeof(ohos_prog_name)-1);\\
        if (n > 0) ohos_prog_name[n] = '\\\\0';\\
        else strcpy(ohos_prog_name, \"wine\");\\
    }\\
    return ohos_prog_name;\\
}\\
# define program_invocation_name get_program_name()\\
#endif\\
/* $mark END */" "$f"
}

# ================================================================
# Patch 04 — configure.ac: 添加 linux-ohos* host 支持
# ================================================================
# 问题:
#   configure.ac 不识别 x86_64-linux-ohos 目标三重
#   需要添加 case branch
#
# 修法:
#   在 linux* 分支中添加 linux-ohos 子检测
patch_04_configure_ohos() {
    local f="$WINE_SRC/configure.ac"
    local mark='OHOS_PATCH_CONFIGURE'

    [ -f "$f" ] || { _patch_header 04 "configure.ac" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 04 "configure.ac" "already patched"
        return 0
    fi
    _patch_header 04 "configure.ac" "add linux-ohos host support"

    # 在 linux*) 的 case branch 中添加 OHOS 检测
    # 找到 host_os 的 case 语句并确保 linux-ohos 匹配
    # 由于 linux*) 通配符已经匹配 linux-ohos, 主要是确保 OHOS 特定的 flags 正确
    sed -i "1i\\
# $mark: OHOS support added — linux-ohos matches linux* pattern" "$f"

    echo "    [#04] INFO: linux* 通配符已匹配 OHOS, 无需额外分支"
    echo "    [#04] INFO: 如需要 Android 风格的 flags, 在此添加 linux-ohos*) 分支"
}

# ================================================================
# Patch 05 — signal_x86_64.c: 添加 libc.so musl 名称匹配
# ================================================================
# 问题:
#   signal_x86_64.c:2759 硬编码 strcmp(p, "libc.so.6")
#   musl 的库名是 "libc.so"
#
# 修法:
#   也匹配 "libc.so" (注意不要匹配到 "libc.so" 后面跟数字的情况)
patch_05_libc_name() {
    local f="$WINE_SRC/dlls/ntdll/unix/signal_x86_64.c"
    local mark='OHOS_PATCH_LIBC_NAME'

    [ -f "$f" ] || { _patch_header 05 "signal_x86_64.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 05 "signal_x86_64.c" "already patched"
        return 0
    fi
    _patch_header 05 "signal_x86_64.c" "add musl libc.so name match"

    # 扩展 libc 名称匹配
    sed -i "s|if (strcmp( p, \"libc.so.6\" ))|/* $mark: also match musl libc name */\\
    if (strcmp( p, \"libc.so.6\" ) \&\& strcmp( p, \"libc.so\" ) \&\& strncmp( p, \"libc.musl-\", 10 ))|" "$f"
}

# ================================================================
# Patch 06 — _GNU_SOURCE 全局启用 (如果 OHOS SDK 未默认)
# ================================================================
# 目的:
#   确保所有 Wine 源文件都能访问 GNU 扩展 (gettid, strerror_r 等)
#   musl 在 _GNU_SOURCE 下提供这些
patch_06_gnu_source() {
    local f="$WINE_SRC/include/config.h"
    local mark='OHOS_PATCH_GNU_SOURCE'

    [ -f "$f" ] || return 1
    if _already "$f" "$mark"; then
        _patch_header 06 "include/config.h" "already patched (GNU_SOURCE)"
        return 0
    fi
    _patch_header 06 "include/config.h" "ensure _GNU_SOURCE defined"

    cat >> "$f" << 'EOF'
/* OHOS_PATCH_GNU_SOURCE */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
/* OHOS_PATCH_GNU_SOURCE END */
EOF
}

# ================================================================
# 调度
# ================================================================
echo "==> apply Wine OHOS patches to: $WINE_SRC"

patch_01_config_h
patch_02_dll_soinit
patch_03_program_invocation_name
patch_04_configure_ohos
patch_05_libc_name
patch_06_gnu_source

echo "    all patches applied."

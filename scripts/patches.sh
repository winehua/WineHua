#!/usr/bin/env bash
# Wine HarmonyOS musl 閫傞厤琛ヤ竵闆?
#
# 鐢ㄦ硶:
#   WINE_SRC=/path/to/wine bash patches.sh
#
# 璁捐:
#   - 姣忔潯 patch 涓€涓嚱鏁? 鑷甫鍘熷洜璇存槑
#   - 閫氳繃婧愮爜鍐呮爣璁版敞閲婂垽鏂槸鍚﹀凡鎵撹繃, 骞傜瓑
#   - 浠讳竴鏉″け璐ョ珛鍗抽€€鍑?(set -e)
#
# 璁捐鍙傝€?

set -e

: "${WINE_SRC:?WINE_SRC 鐜鍙橀噺鏈缃?(搴旀寚鍚?wine 婧愮爜鐩綍)}"

if [ ! -d "$WINE_SRC" ]; then
    echo "ERROR: WINE_SRC 鐩綍涓嶅瓨鍦? $WINE_SRC"
    exit 1
fi

_patch_header() {
    printf '    [#%-2s] %-50s %s\n' "$1" "$2" "$3"
}

_already() {
    [ -f "$1" ] && grep -q "$2" "$1"
}

# ================================================================
# Patch 01 鈥?include/config.h: 娣诲姞 musl 鐗瑰畾鐨?HAVE_ 瀹?
# ================================================================
# 鐩殑:
#   musl 缂哄皯 dladdr1 鍜?dlinfo, 浣?Wine 闇€瑕?fallback 璺緞銆?
#   鍦?config.h 涓槑纭畾涔夎繖浜涘畯, 璁╂簮浠ｇ爜璧版纭殑鍒嗘敮銆?
#
#   鍚屾椂娣诲姞 PACKAGE_STRING 绛?autotools 鏍囧噯瀹忋€?
patch_01_config_h() {
    local f="$WINE_SRC/include/config.h"
    local mark='OHOS_PATCH_CONFIG'

    [ -f "$f" ] || { echo "ERROR: config.h 涓嶅瓨鍦? 闇€瑕佸厛杩愯 configure"; return 1; }
    if _already "$f" "$mark"; then
        _patch_header 01 "include/config.h" "already patched"
        return 0
    fi
    _patch_header 01 "include/config.h" "add musl-specific defines"

    cat >> "$f" << 'EOF'
/* OHOS_PATCH_CONFIG 鈥?musl-specific compatibility defines */

/* musl does not have dladdr1 or dlinfo 鈥?force fallback paths */
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
# Patch 02 鈥?libs/winecrt0/dll_soinit.c: 娣诲姞 musl link_map fallback
# ================================================================
# 闂:
#   浠ｇ爜 fallback 閾? dladdr1 鈫?dlinfo 鈫?(缂哄け)
#   musl 涓よ€呴兘涓嶆彁渚? 闇€瑕佹柊澧炵涓変釜 fallback銆?
#
# 淇硶:
#   浣跨敤 _DYNAMIC 閰嶅悎 dl_iterate_phdr 鑾峰彇 link_map
patch_02_dll_soinit() {
    local f="$WINE_SRC/libs/winecrt0/dll_soinit.c"
    local mark='OHOS_PATCH_DLL_SOINIT'

    [ -f "$f" ] || { _patch_header 02 "dll_soinit.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 02 "libs/winecrt0/dll_soinit.c" "already patched"
        return 0
    fi
    _patch_header 02 "libs/winecrt0/dll_soinit.c" "add musl link_map fallback"

    # 鍦?#else 鍒嗘敮娣诲姞 __MUSL__ 璺緞
    # 鏌ユ壘鍏抽敭 fallback 鐐瑰苟娉ㄥ叆 musl 浠ｇ爜
    local pattern='defined(HAVE_DLINFO)'
    if grep -q "$pattern" "$f"; then
        sed -i "s|#elif $pattern|/* $mark */\n#elif $pattern|" "$f"

        # 鍦?HAVE_DLINFO 鐨?else 鍒嗘敮娣诲姞 musl fallback
        # 浣跨敤 sed 鍦ㄩ€傚綋浣嶇疆鎻掑叆 _DYNAMIC-based 瑙ｅ喅鏂规
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
                /* main executable 鈥?use its l_addr as a base */
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
# Patch 03 鈥?libs/vkd3d/libs/vkd3d/utils.c: program_invocation_name
# ================================================================
# 闂:
#   program_invocation_name 鏄?glibc 鐗规湁鍏ㄥ眬鍙橀噺, musl 涓嶆彁渚?
#
# 淇硶:
#   鐢?#ifdef __GLIBC__ 瀹堝崼, musl 涓婁粠 /proc/self/exe 璇诲彇
patch_03_program_invocation_name() {
    local f="$WINE_SRC/libs/vkd3d/libs/vkd3d/utils.c"
    local mark='OHOS_PATCH_PROG_INV_NAME'

    [ -f "$f" ] || { _patch_header 03 "vkd3d/utils.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 03 "libs/vkd3d/utils.c" "already patched"
        return 0
    fi
    _patch_header 03 "libs/vkd3d/utils.c" "add musl program_invocation_name fallback"

    # 鍦ㄦ枃浠堕《閮ㄦ坊鍔?conditional 瀹氫箟
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
# Patch 04 鈥?configure.ac: 娣诲姞 linux-ohos* host 鏀寔
# ================================================================
# 闂:
#   configure.ac 涓嶈瘑鍒?x86_64-linux-ohos 鐩爣涓夐噸
#   闇€瑕佹坊鍔?case branch
#
# 淇硶:
#   鍦?linux* 鍒嗘敮涓坊鍔?linux-ohos 瀛愭娴?
patch_04_configure_ohos() {
    local f="$WINE_SRC/configure.ac"
    local mark='OHOS_PATCH_CONFIGURE'

    [ -f "$f" ] || { _patch_header 04 "configure.ac" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 04 "configure.ac" "already patched"
        return 0
    fi
    _patch_header 04 "configure.ac" "add linux-ohos host support"

    # 鍦?linux*) 鐨?case branch 涓坊鍔?OHOS 妫€娴?
    # 鎵惧埌 host_os 鐨?case 璇彞骞剁‘淇?linux-ohos 鍖归厤
    # 鐢变簬 linux*) 閫氶厤绗﹀凡缁忓尮閰?linux-ohos, 涓昏鏄‘淇?OHOS 鐗瑰畾鐨?flags 姝ｇ‘
    sed -i "1i\\
# $mark: OHOS support added 鈥?linux-ohos matches linux* pattern" "$f"

    echo "    [#04] INFO: linux* 閫氶厤绗﹀凡鍖归厤 OHOS, 鏃犻渶棰濆鍒嗘敮"
    echo "    [#04] INFO: 濡傞渶瑕?Android 椋庢牸鐨?flags, 鍦ㄦ娣诲姞 linux-ohos*) 鍒嗘敮"
}

# ================================================================
# Patch 05 鈥?signal_x86_64.c: 娣诲姞 libc.so musl 鍚嶇О鍖归厤
# ================================================================
# 闂:
#   signal_x86_64.c:2759 纭紪鐮?strcmp(p, "libc.so.6")
#   musl 鐨勫簱鍚嶆槸 "libc.so"
#
# 淇硶:
#   涔熷尮閰?"libc.so" (娉ㄦ剰涓嶈鍖归厤鍒?"libc.so" 鍚庨潰璺熸暟瀛楃殑鎯呭喌)
patch_05_libc_name() {
    local f="$WINE_SRC/dlls/ntdll/unix/signal_x86_64.c"
    local mark='OHOS_PATCH_LIBC_NAME'

    [ -f "$f" ] || { _patch_header 05 "signal_x86_64.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 05 "signal_x86_64.c" "already patched"
        return 0
    fi
    _patch_header 05 "signal_x86_64.c" "add musl libc.so name match"

    # 鎵╁睍 libc 鍚嶇О鍖归厤
    sed -i "s|if (strcmp( p, \"libc.so.6\" ))|/* $mark: also match musl libc name */\\
    if (strcmp( p, \"libc.so.6\" ) \&\& strcmp( p, \"libc.so\" ) \&\& strncmp( p, \"libc.musl-\", 10 ))|" "$f"
}

# ================================================================
# Patch 06 鈥?_GNU_SOURCE 鍏ㄥ眬鍚敤 (濡傛灉 OHOS SDK 鏈粯璁?
# ================================================================
# 鐩殑:
#   纭繚鎵€鏈?Wine 婧愭枃浠堕兘鑳借闂?GNU 鎵╁睍 (gettid, strerror_r 绛?
#   musl 鍦?_GNU_SOURCE 涓嬫彁渚涜繖浜?
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
# 璋冨害
# ================================================================
echo "==> apply Wine OHOS patches to: $WINE_SRC"

patch_01_config_h
patch_02_dll_soinit
patch_03_program_invocation_name
patch_04_configure_ohos
patch_05_libc_name
patch_06_gnu_source

echo "    all patches applied."

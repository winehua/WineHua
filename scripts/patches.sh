#!/usr/bin/env bash
# Apply local OHOS and musl compatibility patches to a Wine source tree.
#
# Usage:
#   WINE_SRC=/path/to/wine bash scripts/patches.sh
#
# Notes:
#   - The script edits files in-place.
#   - Each patch uses a marker so reruns stay idempotent.
#   - Keep `set -e` so a failed patch stops the pass.

set -e

: "${WINE_SRC:?WINE_SRC is required and must point at the Wine source tree}"

if [ ! -d "$WINE_SRC" ]; then
    echo "ERROR: WINE_SRC does not exist: $WINE_SRC"
    exit 1
fi

_patch_header() {
    printf '    [#%-2s] %-50s %s\n' "$1" "$2" "$3"
}

_already() {
    [ -f "$1" ] && grep -q "$2" "$1"
}

# ================================================================
# Patch 01: include/config.h
# Add musl compatibility defines and package metadata.
# ================================================================
patch_01_config_h() {
    local f="$WINE_SRC/include/config.h"
    local mark='OHOS_PATCH_CONFIG'

    [ -f "$f" ] || { echo "ERROR: config.h not found; run configure first"; return 1; }
    if _already "$f" "$mark"; then
        _patch_header 01 "include/config.h" "already patched"
        return 0
    fi
    _patch_header 01 "include/config.h" "add musl-specific defines"

    cat >> "$f" << 'EOF'
/* OHOS_PATCH_CONFIG: musl-specific compatibility defines */

/* musl does not have dladdr1 or dlinfo; force Wine fallback paths */
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
# Patch 02: libs/winecrt0/dll_soinit.c
# Add a musl-side fallback around link_map discovery.
# ================================================================
patch_02_dll_soinit() {
    local f="$WINE_SRC/libs/winecrt0/dll_soinit.c"
    local mark='OHOS_PATCH_DLL_SOINIT'
    local pattern='defined(HAVE_DLINFO)'

    [ -f "$f" ] || { _patch_header 02 "dll_soinit.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 02 "libs/winecrt0/dll_soinit.c" "already patched"
        return 0
    fi
    _patch_header 02 "libs/winecrt0/dll_soinit.c" "add musl link_map fallback"

    if grep -q "$pattern" "$f"; then
        sed -i "s|#elif $pattern|/* $mark */\n#elif $pattern|" "$f"

        python3 - "$f" "$mark" << 'PY'
import sys

fname, mark = sys.argv[1], sys.argv[2]
with open(fname) as f:
    content = f.read()

old = 'dlinfo( RTLD_SELF, RTLD_DI_LINKMAP, &map )'
new = '''/* %s: musl has no dlinfo, use dl_iterate_phdr */
#ifdef __MUSL__
    {
        extern char **environ;
        /* Use dl_iterate_phdr to find our own link_map */
        dl_iterate_phdr([](struct dl_phdr_info *info, size_t size, void *data) -> int {
            struct link_map **lmap = (struct link_map **)data;
            if (info->dlpi_name && info->dlpi_name[0] == '\\0') {
                /* Main executable: use its l_addr as a base */
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
# Patch 03: libs/vkd3d/libs/vkd3d/utils.c
# Provide a musl fallback for program_invocation_name.
# ================================================================
patch_03_program_invocation_name() {
    local f="$WINE_SRC/libs/vkd3d/libs/vkd3d/utils.c"
    local mark='OHOS_PATCH_PROG_INV_NAME'

    [ -f "$f" ] || { _patch_header 03 "vkd3d/utils.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 03 "libs/vkd3d/utils.c" "already patched"
        return 0
    fi
    _patch_header 03 "libs/vkd3d/utils.c" "add musl program_invocation_name fallback"

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
# Patch 04: configure.ac
# Document that linux-ohos is already covered by the linux* host match.
# ================================================================
patch_04_configure_ohos() {
    local f="$WINE_SRC/configure.ac"
    local mark='OHOS_PATCH_CONFIGURE'

    [ -f "$f" ] || { _patch_header 04 "configure.ac" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 04 "configure.ac" "already patched"
        return 0
    fi
    _patch_header 04 "configure.ac" "note linux-ohos host handling"

    sed -i "1i\\
# $mark: linux-ohos already matches the existing linux* configure branch" "$f"

    echo "    [#04] INFO: linux-ohos is already covered by the existing linux* host case."
    echo "    [#04] INFO: add a dedicated linux-ohos* branch only if OHOS-specific flags are needed later."
}

# ================================================================
# Patch 05: dlls/ntdll/unix/signal_x86_64.c
# Match musl libc sonames in the libc path probe.
# ================================================================
patch_05_libc_name() {
    local f="$WINE_SRC/dlls/ntdll/unix/signal_x86_64.c"
    local mark='OHOS_PATCH_LIBC_NAME'

    [ -f "$f" ] || { _patch_header 05 "signal_x86_64.c" "not found, skip"; return 0; }
    if _already "$f" "$mark"; then
        _patch_header 05 "signal_x86_64.c" "already patched"
        return 0
    fi
    _patch_header 05 "signal_x86_64.c" "add musl libc.so name match"

    sed -i "s|if (strcmp( p, \"libc.so.6\" ))|/* $mark: also match musl libc names */\\
    if (strcmp( p, \"libc.so.6\" ) \&\& strcmp( p, \"libc.so\" ) \&\& strncmp( p, \"libc.musl-\", 10 ))|" "$f"
}

# ================================================================
# Patch 06: include/config.h
# Ensure _GNU_SOURCE stays enabled for musl-side builds.
# ================================================================
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
# Main
# ================================================================
echo "==> apply Wine OHOS patches to: $WINE_SRC"

patch_01_config_h
patch_02_dll_soinit
patch_03_program_invocation_name
patch_04_configure_ohos
patch_05_libc_name
patch_06_gnu_source

echo "    all patches applied."

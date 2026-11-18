"""Test upstream fat dispatch as C pointer selection, never execute ARM code."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
candidates = [Path(os.environ['NETTLE_SRC'])] if 'NETTLE_SRC' in os.environ else [ROOT / 'build/gnutls_tarballs/nettle', ROOT / 'thirdparty/nettle']
source = next((p for p in candidates if (p / 'fat-arm64.c').is_file()), None)
if source is None:
    raise SystemExit('Nettle 3.10.2 source required: initialize thirdparty/nettle or set NETTLE_SRC to the official release source')


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 3]


with tempfile.TemporaryDirectory(prefix='nettle-arm64-contract-') as folder:
    tree = Path(folder)
    shutil.copy2(source / 'fat-arm64.c', tree / 'fat-arm64.c')
    patch = (ROOT / 'patches/nettle/0001-ohos-arm64-fat-dispatch.patch').read_bytes()
    subprocess.run(['patch', '-p1', '--batch', '--force'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['patch', '-p1', '--batch', '--force', '--dry-run', '-R'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    text = (tree / 'fat-arm64.c').read_text()
    definitions = text[text.index('/* OHOS musl'):text.index('#include "nettle-types.h"')]
    features = text[text.index('/* Defines from arch/arm64'):text.index('DECLARE_FAT_FUNC(nettle_aes128_encrypt')]
    init = function(text, 'static void CONSTRUCTOR\nfat_init (void)')
    assignments = re.findall(r'(\w+_vec) = (\w+);', init)
    vectors = sorted(set(v for v, _ in assignments))
    backends = sorted(set(b for _, b in assignments))
    common = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONSTRUCTOR
#define ENV_VERBOSE "NETTLE_FAT_VERBOSE"
#define ENV_OVERRIDE "NETTLE_FAT_OVERRIDE"
static unsigned long capabilities;
static inline unsigned long fixture_getauxval(unsigned long type) { (void)type; return capabilities; }
#define getauxval fixture_getauxval
'''
    stubs = '\n'.join('static void ' + name + '(void) {}' for name in backends) + '\n'
    stubs += '\n'.join('static void (*' + name + ')(void);' for name in vectors) + '\n'
    checks = ''
    for vector in vectors:
        stem = vector[:-4]
        c = next(b for v, b in assignments if v == vector and b.endswith('_c'))
        arm = next(b for v, b in assignments if v == vector and b.endswith('_arm64'))
        feature = 'AES' if 'aes' in vector else 'PMULL' if 'ghash' in vector else 'SHA1' if 'sha1' in vector else 'SHA2'
        checks += f'assert({vector} == (enabled && (mask & HWCAP_ASIMD) && (mask & HWCAP_{feature}) ? {arm} : {c}));\n'
    for platform in ['ohos', 'ohos-musl']:
        for probe in [0, 1]:
            code = common + definitions + features + stubs + init + r'''
int main(void) {
    const int enabled = VP_NETTLE_HAVE_GETAUXVAL;
    /* An unsafe feature override must not supersede real OHOS capabilities. */
    assert(!setenv(ENV_OVERRIDE,"aes,pmull,sha1,sha2",1));
    for(unsigned long mask=0;mask<128;mask++) {
        capabilities=mask;fat_init();
''' + checks + '\n}\nreturn 0;\n}\n'
            cfile = tree / f'dispatch-{platform}-{probe}.c'
            binary = cfile.with_suffix('')
            cfile.write_text(code)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                            '-D__OHOS__=1', *(['-D__MUSL__=1'] if platform == 'ohos-musl' else []), f'-DVP_NETTLE_HAVE_GETAUXVAL={probe}',
                            str(cfile), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print('PASS upstream ARM64 dispatch: 128 HWCAP masks, probe on/off, OHOS with/without __MUSL__, override ignored')

    build = (ROOT / 'scripts/build_gnutls.sh').read_text()
    helpers = '\n'.join(function(build, name + '() {') for name in ['nettle_asm_select', 'nettle_build_identity', 'nettle_identity_matches'])
    compiler = tree / 'probe-compiler'
    compiler.write_text('#!/bin/sh\necho probe >> "$PROBE_LOG"\nexit "$PROBE_RESULT"\n')
    compiler.chmod(0o755)
    shell = r'''
set -euo pipefail
''' + helpers + r'''
NETTLE_VERSION=3.10.2
NETTLE_PATCH_SHA=original
NETTLE_STAMP="$TEST_DIR/identity"
TARGET=aarch64-linux-ohos
SYSROOT=/unused
for WINE_ARCH in aarch64 x86_64; do
    for VP_NETTLE_ASM in auto off; do
        for PROBE_RESULT in 0 1; do
            export PROBE_RESULT
            : > "$PROBE_LOG"
            nettle_asm_select
            expected=off
            if [ "$WINE_ARCH:$VP_NETTLE_ASM:$PROBE_RESULT" = aarch64:auto:0 ]; then expected=arm64-fat; fi
            [ "$NETTLE_ASM_EFFECTIVE" = "$expected" ]
            if [ "$expected" = arm64-fat ]; then
                [ "${NETTLE_ASM_FLAGS[*]}" = '--enable-assembler --enable-fat' ]
                [ "$NETTLE_ASM_CFLAGS" = '-DVP_NETTLE_HAVE_GETAUXVAL=1' ]
            else
                [ "${NETTLE_ASM_FLAGS[*]}" = '--disable-assembler --disable-fat' ]
                [ -z "$NETTLE_ASM_CFLAGS" ]
            fi
            if [ "$WINE_ARCH:$VP_NETTLE_ASM" = aarch64:auto ]; then [ -s "$PROBE_LOG" ]; else [ ! -s "$PROBE_LOG" ]; fi
            nettle_build_identity > "$NETTLE_STAMP"
            nettle_identity_matches
            NETTLE_PATCH_SHA=changed
            ! nettle_identity_matches
            NETTLE_PATCH_SHA=original
            saved="$VP_NETTLE_ASM"
            if [ "$saved" = auto ]; then VP_NETTLE_ASM=off; else VP_NETTLE_ASM=auto; fi
            ! nettle_identity_matches
            VP_NETTLE_ASM="$saved"
            nettle_identity_matches
            NETTLE_VERSION=other
            ! nettle_identity_matches
            NETTLE_VERSION=3.10.2
        done
    done
done
VP_NETTLE_ASM=unsafe
! nettle_asm_select
'''
    subprocess.run(['bash', '-c', shell], env={**os.environ, 'TEST_DIR': str(tree), 'CLANG': str(compiler),
                                             'PROBE_LOG': str(tree / 'probe.log')}, check=True)
    validation = function(build, 'nettle_validate_configuration() {')
    configured = tree / 'configured'
    configured.mkdir()
    required = ['aes128-encrypt-2', 'aes128-decrypt-2', 'aes192-encrypt-2', 'aes192-decrypt-2',
                'aes256-encrypt-2', 'aes256-decrypt-2', 'ghash-set-key-2', 'ghash-update-2',
                'sha1-compress-2', 'sha256-compress-n-2']
    for name in required:
        shutil.copy2(source / 'arm64/fat' / (name + '.asm'), configured / (name + '.asm'))
    valid_make = 'OPT_NETTLE_SOURCES = fat-arm64.c\nOPT_NETTLE_OBJS = ' + ' '.join(n + '.$(OBJEXT)' for n in required) + '\n'
    def validate(contents, expected):
        (configured / 'Makefile').write_text(contents)
        result = subprocess.run(['bash', '-c', 'set -euo pipefail\n' + validation + '\nnettle_validate_configuration "$TEST_BUILD"'],
                                env={**os.environ, 'NETTLE_ASM_EFFECTIVE': 'arm64-fat',
                                     'NETTLE_SOURCE': str(source), 'TEST_BUILD': str(configured)}, capture_output=True, text=True)
        assert (result.returncode == 0) == expected, result.stdout + result.stderr
    validate(valid_make, True)
    validate(valid_make.replace('fat-arm64.c', 'fat-x86_64.c'), False)
    validate(valid_make.replace('sha256-compress-n-2.$(OBJEXT)', ''), False)
    (configured / 'ghash-update-2.asm').write_text('wrong backend\n')
    validate(valid_make, False)
    print('PASS Nettle configure guard rejects missing fat source, missing object and wrong ASM backend')

    # Run the actual public entry point: invalid mode must fail before env.sh,
    # download, or source setup rather than merely being rejected by a helper.
    invalid = subprocess.run(['bash', str(ROOT / 'scripts/build_gnutls.sh'), '--check-identity'],
                             env={**os.environ, 'VP_NETTLE_ASM': 'unsafe'}, capture_output=True, text=True)
    assert invalid.returncode and 'invalid VP_NETTLE_ASM' in invalid.stderr
    print('PASS Nettle auto/off/architecture/probe matrix, identity invalidation and invalid input')

    # Exercise the real top-level deps recipe's complete skip predicate through
    # GNU make, with all unrelated freshness checks stubbed. Its actual identity
    # command invokes the production helper, not an expected-string assertion.
    make = (ROOT / 'Makefile').read_text()
    start = make.index('\tif [ -f $@ ] && [ -f $(DEPS_SENTINEL) ]')
    end = make.index('\n\tfi', start) + len('\n\tfi')
    recipe = "\t@guest_gfx_ready=1; guest_vulkan_ready=1; mono_ready=1; " + chr(92) + "\n" + make[start:end].replace("\t@if", "\tif", 1)
    scripts = tree / 'scripts'
    scripts.mkdir()
    (scripts / 'build_gnutls.sh').write_text('#!/bin/bash\nset -euo pipefail\n' + helpers + '\n'
        'NETTLE_VERSION=3.10.2\nNETTLE_STAMP="$TEST_DIR/identity"\nNETTLE_ASM_EFFECTIVE="$EFFECTIVE"\n'
        'nettle_identity_matches\n')
    (scripts / 'build_deps.sh').write_text('#!/bin/bash\necho rebuilt >> "$TEST_DIR/rebuilt"\n')
    sentinel = tree / 'sentinel'
    sentinel.touch()
    stamp = tree / 'deps'
    stamp.touch()
    # Real make substitutes variables; fake find supplies no newer unrelated files.
    bin_dir = tree / 'bin'
    bin_dir.mkdir()
    (bin_dir / 'find').write_text('#!/bin/sh\nexit 0\n')
    (bin_dir / 'find').chmod(0o755)
    mf = tree / 'Makefile'
    mf.write_text('ROOT := ' + str(tree) + '\nSCRIPTS := ' + str(scripts) + '\nBUILD_DIR := ' + str(tree) +
                  '\nDEPS_SENTINEL := ' + str(sentinel) + '\n.PHONY: FORCE\n' + str(stamp) + ': FORCE\n' + recipe + '\n')
    env = {**os.environ, 'TEST_DIR': str(tree), 'PATH': str(bin_dir) + ':' + os.environ['PATH'],
           'WINE_ARCH': 'aarch64', 'VP_NETTLE_ASM': 'auto', 'EFFECTIVE': 'arm64-fat', 'NETTLE_PATCH_SHA': 'original',
           'TARGET': 'aarch64-linux-ohos'}
    # New scripts precede stamp so freshness checks do not mask identity checks.
    for item in scripts.iterdir():
        os.utime(item, (stamp.stat().st_mtime - 2, stamp.stat().st_mtime - 2))
    identity = 'schema=1\narch=aarch64\nnettle=3.10.2\nrequested=auto\neffective=arm64-fat\npatch=original\ntarget=aarch64-linux-ohos\n'
    (tree / 'identity').write_text(identity)
    for changes, rebuild in [({}, False), ({'VP_NETTLE_ASM': 'off', 'EFFECTIVE': 'off'}, True),
                             ({'NETTLE_PATCH_SHA': 'new'}, True)]:
        log = tree / 'rebuilt'
        log.unlink(missing_ok=True)
        subprocess.run(['make', '-s', '-f', str(mf), str(stamp)], env={**env, **changes}, check=True)
        assert log.exists() == rebuild
    print('PASS top-level deps identity skip/rebuild for unchanged, auto-to-off, and patch changes')

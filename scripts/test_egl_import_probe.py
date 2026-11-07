"""Run the probe's actual import block against a strict window-buffer driver."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class EglImportProbeTests(unittest.TestCase):
    def test_wrapper_payload_and_failed_imports(self):
        source = (ROOT/'entry/src/main/cpp/display/ohos_egl_import_probe.c').read_text(encoding='utf-8')
        start = source.index('    /* ── 4.')
        end = source.index('    /* ── 5.', start)
        block = source[start:end]
        harness = r'''
#include <cassert>
#include <cstring>
#include <cstddef>
using EGLDisplay = void*;
using EGLContext = void*;
using EGLImageKHR = void*;
using EGLClientBuffer = void*;
using EGLint = int;
using EGLenum = unsigned int;
using PFN_eglCreateImageKHR_ = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
using PFN_eglDestroyImageKHR_ = bool (*)(EGLDisplay, EGLImageKHR);
using PFN_glEGLImageTargetTexture2DOES_ = void (*)(unsigned int, void*);
struct OH_NativeBuffer { int tag; } nativeBuffer;
struct OHNativeWindowBuffer { int tag; } windowBuffer;
constexpr auto EGL_NO_CONTEXT = nullptr;
constexpr auto EGL_NO_IMAGE_KHR = nullptr;
constexpr EGLenum EGL_NATIVE_BUFFER_OHOS = 0x34e1;
int wraps = 0, imports = 0, failures = 0, successes = 0;
bool failWrap = false, failImport = false;
OHNativeWindowBuffer* OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(OH_NativeBuffer* nb) {
    assert(nb == &nativeBuffer);
    ++wraps;
    return failWrap ? nullptr : &windowBuffer;
}
EGLImageKHR createImage(EGLDisplay, EGLContext ctx, EGLenum target, EGLClientBuffer payload, const EGLint*) {
    assert(ctx == EGL_NO_CONTEXT && target == EGL_NATIVE_BUFFER_OHOS);
    // A real driver may crash instead of safely rejecting the wrong type.
    assert(payload == &windowBuffer && payload != &nativeBuffer);
    ++imports;
    return failImport ? EGL_NO_IMAGE_KHR : payload;
}
bool destroyImage(EGLDisplay, EGLImageKHR) { return true; }
void imageTexture(unsigned int, void*) {}
using Proc = void (*)();
Proc eglGetProcAddress(const char* name) {
    if (!std::strcmp(name, "eglCreateImageKHR")) return reinterpret_cast<Proc>(createImage);
    if (!std::strcmp(name, "eglDestroyImageKHR")) return reinterpret_cast<Proc>(destroyImage);
    return reinterpret_cast<Proc>(imageTexture);
}
int eglGetError() { return 0; }
#define PROBE_LOG(...) do {} while (0)
#define FAIL(step) do { ++failures; return; } while (0)
void probeImport() {
    EGLDisplay dpy = nullptr;
    OH_NativeBuffer* nb = &nativeBuffer;
''' + block + r'''
    (void)p_destroy_img;
    assert(img == &windowBuffer);
    assert(!std::strcmp(payload_kind, "pass:payload=OHNativeWindowBuffer"));
    ++successes;
}
int main() {
    probeImport();
    assert(wraps == 1 && imports == 1 && successes == 1 && failures == 0);
    failWrap = true;
    probeImport();
    assert(wraps == 2 && imports == 1 && successes == 1 && failures == 1);
    failWrap = false;
    failImport = true;
    probeImport();
    assert(wraps == 3 && imports == 2 && successes == 1 && failures == 2);
}
'''
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='winehua-egl-probe-') as temp:
            cpp = Path(temp)/'probe.cpp'
            exe = Path(temp)/('probe.exe' if os.name == 'nt' else 'probe')
            cpp.write_text(harness, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

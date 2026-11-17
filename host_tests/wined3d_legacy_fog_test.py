#!/usr/bin/env python3
"""Execute Wine's actual VS epilogue generator, including a failing old-source control."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL;
#define FALSE 0
#define VS_FOG_Z 1
#define VS_FOG_W 2
#define ARB_CLIP_CONTROL 0
#define WINED3D_SHADER_TYPE_PIXEL 1
struct wined3d_gl_info { struct { unsigned user_clip_distances; } limits; int supported[1]; };
struct wined3d_string_buffer { char text[8192]; };
struct wined3d_shader_reg_maps { struct { unsigned major; } shader_version; unsigned output_registers; int fog, point_size; };
struct wined3d_shader { struct wined3d_shader_reg_maps reg_maps; };
struct vs_compile_args { int fog_src, clip_enabled, point_size, next_shader_type; };
static int legacy;
static BOOL needs_legacy_glsl_syntax(const struct wined3d_gl_info *g) { (void)g; return legacy; }
static void shader_addline(struct wined3d_string_buffer *b, const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(b->text + strlen(b->text), sizeof(b->text) - strlen(b->text), format, args); va_end(args);
}
static void shader_glsl_fixup_position(struct wined3d_string_buffer *b, BOOL f) { (void)b; (void)f; }
'''
MAIN = r'''
int main(void) {
    unsigned failures=0, cases=0;
    for (legacy=0; legacy<2; ++legacy) for (unsigned major=1; major<=4; ++major)
    for (int specular=0; specular<2; ++specular) for (int fog=0; fog<2; ++fog)
    for (int src=0; src<3; ++src) {
        struct wined3d_gl_info gl={0};
        struct wined3d_shader shader={{{major},specular ? 1u<<9 : 0,fog,0}};
        struct vs_compile_args args={src,0,0,0};
        struct wined3d_string_buffer buffer={{0}};
        char expected[256]="setup_vs_output(vs_out);\n";
        if (major<3 && (src || !fog)) {
            const char *value=src==VS_FOG_Z ? "gl_Position.z" : src==VS_FOG_W ? "gl_Position.w" :
                    specular ? (legacy ? "gl_FrontSecondaryColor.w" : "ffp_varying_specular.w") : "0.0";
            snprintf(expected+strlen(expected),sizeof(expected)-strlen(expected),"%s = %s;\n",
                    legacy ? "gl_FogFragCoord" : "ffp_varying_fogcoord",value);
        }
        shader_glsl_generate_vs_epilogue(&gl,&buffer,&shader,&args);
        if (strcmp(expected,buffer.text)) ++failures;
        ++cases;
    }
    printf("cases=%u failures=%u\n",cases,failures);
    return failures ? 1 : 0;
}
'''


def execute(source, folder, compiler):
    start = source.index('static void shader_glsl_generate_vs_epilogue(')
    end = source.index('\n/* Context activation is done by the caller. */', start)
    path = folder / 'fog-generator.c'
    path.write_text(PREAMBLE + source[start:end] + MAIN)
    exe = folder / 'fog-generator'
    subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True, help='Wine glsl_shader.c, before or after this overlay')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    original = args.source.read_text()
    patch = ROOT / 'patches/wine/0084-wined3d-legacy-specular-fog.patch'
    with tempfile.TemporaryDirectory(prefix='wined3d-fog-') as tmp:
        folder = Path(tmp)
        source = folder / 'dlls/wined3d/glsl_shader.c'
        source.parent.mkdir(parents=True)
        source.write_text(original)
        reverse = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=folder, capture_output=True)
        if reverse.returncode == 0:
            subprocess.run(['git', 'apply', '--reverse', str(patch)], cwd=folder, check=True)
        before = execute(source.read_text(), folder, args.cc)
        assert before.returncode == 1 and 'cases=96 failures=2' in before.stdout, before.stdout
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=folder, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=folder, check=True)
        after = execute(source.read_text(), folder, args.cc)
        assert after.returncode == 0 and 'cases=96 failures=0' in after.stdout, after.stdout
        print('Old generator negative control:', before.stdout.strip())
        print('Patched generator:', after.stdout.strip())


if __name__ == '__main__':
    main()

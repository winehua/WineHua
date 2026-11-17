#!/usr/bin/env python3
"""Compile Wine's production argv parser; exercise resolved-image handoff."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (Path(os.environ.get('WINE_SRC', root / 'build/runtime-sources/wine')) / 'dlls/ntdll/unix/process.c').read_text()

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
typedef uint_least16_t WCHAR;
typedef struct {unsigned short Length; const WCHAR *Buffer;} UNICODE_STRING;
typedef struct {UNICODE_STRING ImagePathName, CommandLine;} RTL_USER_PROCESS_PARAMETERS;
#define FALSE 0
#define TRACE(...) ((void)0)
#define debugstr_a(s) (s)
static unsigned ntdll_wcstoumbs(const WCHAR *in, int length, char *out, int cap, int ignored)
{
    (void)ignored; unsigned n=0;
    for(int i=0;i<length;i++) {
        unsigned c=in[i];
        assert(c<0xd800 || c>0xdfff); /* Fixtures are BMP, no surrogate conversion claim. */
        if(c<128) out[n++]=c;
        else if(c<2048) {out[n++]=0xc0|(c>>6);out[n++]=0x80|(c&63);}
        else {out[n++]=0xe0|(c>>12);out[n++]=0x80|((c>>6)&63);out[n++]=0x80|(c&63);}
        assert(n<=(unsigned)cap);
    }
    return n;
}
static UNICODE_STRING string(const WCHAR *s)
{unsigned n=0;while(s[n])n++;return (UNICODE_STRING){n*2,s};}
'''
tests = r'''
static unsigned failures;
static void check(const WCHAR *image,const WCHAR *cmd,const char *expected,const char *arg)
{
    RTL_USER_PROCESS_PARAMETERS params={string(image),string(cmd)};
    char **v=build_broker_argv(&params);assert(v);
    int ok=!strcmp(v[0],expected) && (arg ? v[1]&&!strcmp(v[1],arg)&&!v[2] : !v[1]);
    if(!ok) {fprintf(stderr,"wrong executable: %s expected: %s\n",v[0],expected);failures++;}
    free(v);
}
int main(void)
{
    check(u"\\??\\Z:\\games\\[th18] 东方虹龙洞\\th18.exe",u"th18.exe",
          "Z:\\games\\[th18] 东方虹龙洞\\th18.exe",NULL);
    check(u"Z:\\games\\Old Game\\game.exe",u"game.exe -window",
          "Z:\\games\\Old Game\\game.exe","-window");
    check(u"C:\\Game\\game.exe",u"display-name --option","C:\\Game\\game.exe","--option");
    check(u"C:\\Game\\game.exe",u"","C:\\Game\\game.exe",NULL);
    check(u"C:\\Program Files\\Steam\\steam.exe",u"C:\\Program Files\\Steam\\steam.exe -silent",
          "C:\\Program Files\\Steam\\steam.exe","-silent");
    check(u"\\??\\C:\\Program Files\\Steam\\steam.exe",u"\"C:\\Program Files\\Steam\\steam.exe\" -silent",
          "C:\\Program Files\\Steam\\steam.exe","-silent");
    check(u"C:\\Program Files\\Steam\\steam.exe",u"C:\\Program Files\\Steam\\steam -silent",
          "C:\\Program Files\\Steam\\steam.exe","-silent");
    check(u"C:\\Program Files\\Steam\\steamerrorreporter.exe",u"C:\\Program Files\\Steam\\steam",
          "C:\\Program Files\\Steam\\steamerrorreporter.exe",NULL);
    check(u"\\DosDevices\\C:\\Game\\game.exe",u"C:\\Game\\game.exe -test",
          "C:\\Game\\game.exe","-test");
    check(u"C:\\Game\\game.exe",u"game.exe \"two words\"","C:\\Game\\game.exe","two words");
    return failures?1:0;
}
'''
fixed = function('static char **build_broker_argv(')
begin = fixed.index('        /* lpApplicationName may resolve')
end = fixed.index('        consumed = argc ? 1 : 0;', begin) + len('        consumed = argc ? 1 : 0;')
broken = fixed[:begin] + '        free( image );\n        return argv;' + fixed[end:]
with tempfile.TemporaryDirectory(prefix='vp-broker-argv-') as tmp:
    for name, body, expected in [('fixed', fixed, 0), ('previous', broken, 1)]:
        path = Path(tmp) / (name + '.c')
        path.write_text(prefix + function('static char **build_argv(') + body + tests)
        exe = path.with_suffix('')
        subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True)
        assert run.returncode == expected, run.stderr.decode()
print('PASS: relative/alternate argv0, Unicode, empty command line, Steam paths and arguments; previous failure reproduced')

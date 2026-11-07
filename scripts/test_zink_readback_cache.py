#!/usr/bin/env python3
"""Exercise Zink's actual bounded staging selector with retained/busy buffers."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#define PIPE_MAP_READ 1
#define PIPE_MAP_WRITE 2
#define ZINK_RESOURCE_ACCESS_RW 3
#define p_atomic_read(p) (*(p))
struct pipe_resource { struct { int count; } reference; unsigned width0; int format; bool gpu_busy; };
struct context { struct pipe_resource *winehua_readback_staging; };
struct transfer { struct pipe_resource *staging_res; };
static bool enabled = true;
static unsigned live, peak;
static bool debug_get_bool_option(const char *s, bool fallback) { return enabled; }
static struct pipe_resource *zink_resource(struct pipe_resource *p) { return p; }
static bool zink_resource_usage_check_completion(void *s, struct pipe_resource *r, int a) { return !r->gpu_busy; }
static struct pipe_resource *zink_resource_create(void *s, struct pipe_resource *t) {
 struct pipe_resource *p=malloc(sizeof *p);assert(p);*p=*t;p->reference.count=1;
 if(++live>peak)peak=live;return p;
}
static void pipe_resource_reference(struct pipe_resource **dst,struct pipe_resource *src) {
 if(*dst==src)return;if(src)src->reference.count++;
 if(*dst && !--(*dst)->reference.count){free(*dst);live--;}*dst=src;
}
static struct pipe_resource *select_buffer(struct context *ctx,unsigned size,int format,unsigned usage) {
 void *screen=NULL;struct {void *screen;} pipe={0},*pctx=&pipe;
 struct pipe_resource templ={.width0=size,.format=format};struct transfer t={0},*trans=&t;
SELECTOR
 return trans->staging_res;
fail:return NULL;
}
int main(void) {
 struct context ctx={0};struct pipe_resource *a=select_buffer(&ctx,4096,1,PIPE_MAP_READ);
 assert(a && a==ctx.winehua_readback_staging && live==1);
 struct pipe_resource *b=select_buffer(&ctx,4096,1,PIPE_MAP_READ);
 assert(b!=a && ctx.winehua_readback_staging==a);pipe_resource_reference(&b,NULL);
 pipe_resource_reference(&a,NULL);
 for(int i=0;i<2048;i++) {
  b=select_buffer(&ctx,2048,1,PIPE_MAP_READ);assert(b==ctx.winehua_readback_staging);
  pipe_resource_reference(&b,NULL);assert(live==1);
 }
 ctx.winehua_readback_staging->gpu_busy=true;
 b=select_buffer(&ctx,2048,1,PIPE_MAP_READ);assert(b!=ctx.winehua_readback_staging);
 pipe_resource_reference(&b,NULL);ctx.winehua_readback_staging->gpu_busy=false;
 b=select_buffer(&ctx,8192,1,PIPE_MAP_READ);assert(b==ctx.winehua_readback_staging && b->width0==8192);
 pipe_resource_reference(&b,NULL);b=select_buffer(&ctx,4096,2,PIPE_MAP_READ);
 assert(b==ctx.winehua_readback_staging && b->format==2);pipe_resource_reference(&b,NULL);
 b=select_buffer(&ctx,17*1024*1024,2,PIPE_MAP_READ);assert(b!=ctx.winehua_readback_staging);
 pipe_resource_reference(&b,NULL);
 b=select_buffer(&ctx,4096,2,PIPE_MAP_READ|PIPE_MAP_WRITE);assert(b!=ctx.winehua_readback_staging);
 pipe_resource_reference(&b,NULL);enabled=false;
 b=select_buffer(&ctx,4096,2,PIPE_MAP_READ);assert(b!=ctx.winehua_readback_staging);
 pipe_resource_reference(&b,NULL);pipe_resource_reference(&ctx.winehua_readback_staging,NULL);
 assert(!live && peak<=2);
}
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux cc')
class ReadbackCacheTests(unittest.TestCase):
    def test_actual_selector_lifetime(self):
        source = (ROOT / 'thirdparty/mesa/src/gallium/drivers/zink/zink_resource.c').read_text()
        start = source.index('      const bool cache_readback =')
        end = source.index('      struct zink_resource *staging_res =', start)
        with tempfile.TemporaryDirectory(prefix='winehua-staging-') as directory:
            src = Path(directory) / 'test.c'
            exe = Path(directory) / 'test'
            src.write_text(HARNESS.replace('SELECTOR', source[start:end]))
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                            str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

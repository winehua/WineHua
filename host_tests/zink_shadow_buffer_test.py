#!/usr/bin/env python3
"""Compile production buffer synchronization and failure cleanup with shadow BOs."""
import argparse
from pathlib import Path
import resource
import tempfile
from zink_shadow_test_support import ROOT, compile_run, function, replay

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mesa-git', type=Path, default=ROOT / 'thirdparty/mesa')
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t VkDeviceSize;
enum { PIPE_MAP_READ=1, PIPE_MAP_WRITE=2, VK_SUCCESS=0 };
struct bo { unsigned refs, maps; uint8_t host[1024], guest[1024]; };
struct zink_resource_object { bool coherent; VkDeviceSize offset, size; struct bo *bo; };
struct pipe_resource { unsigned refs; };
struct zink_resource { struct pipe_resource base; struct zink_resource_object *obj; };
struct pipe_box { unsigned x, width; };
struct zink_transfer { struct { struct { struct pipe_resource *resource; } b; } base;
   struct pipe_resource *staging_res; unsigned offset; };
struct zink_context { int unused; };
struct zink_screen { int dev; struct zink_context *copy_context; };
typedef struct { struct bo *memory; VkDeviceSize offset, size; } VkMappedMemoryRange;
static const char *option;
static bool sync_fail;
static unsigned calls, unmapped, destroyed, unlocked;
static VkMappedMemoryRange emitted;
static const char *os_get_option(const char *key) { (void)key; return option; }
#define mesa_loge(...) ((void)0)
#define VKSCR(name) name
static VkMappedMemoryRange zink_resource_init_mem_range(struct zink_screen *s,
   struct zink_resource_object *obj, VkDeviceSize off, VkDeviceSize len) {
   (void)s; assert(off + len <= obj->size); return (VkMappedMemoryRange){obj->bo, off, len};
}
static int InvalidateMappedMemoryRanges(int dev, unsigned n, const VkMappedMemoryRange *r) {
   (void)dev; assert(n == 1); calls++; emitted=*r;
   if (sync_fail) return -1;
   memcpy(r->memory->guest+r->offset,r->memory->host+r->offset,r->size); return VK_SUCCESS;
}
static void zink_bo_unmap(struct zink_screen *s,struct bo *bo) {
   (void)s; assert(bo->maps); bo->maps--; unmapped++;
}
static void zink_screen_unlock_context(struct zink_screen *s) { (void)s; unlocked++; }
static void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src) {
   assert(!src); if (*dst) { assert((*dst)->refs); (*dst)->refs--; } *dst=src;
}
static void destroy_transfer(struct zink_context *c, struct zink_transfer *t) {
   (void)c; (void)t; destroyed++;
}
'''

MAIN = r'''
static void test(bool staging, unsigned bo_offset, unsigned x, unsigned usage,
                 const char *enabled, bool coherent, bool fail) {
   struct bo bo={.maps=1}; memset(bo.host,0xa5,sizeof bo.host);
   memset(bo.guest,0x17,sizeof bo.guest);
   struct zink_resource_object obj={coherent,bo_offset,1024,&bo};
   struct zink_resource res={.base={1},.obj=&obj};
   struct pipe_resource original={1}, temporary={1};
   struct zink_transfer trans={.base.b.resource=&original,
      .staging_res=staging?&temporary:NULL,.offset=staging?20:0};
   struct zink_context normal={0}, copy={0};
   struct zink_screen screen={.copy_context=&copy};
   struct pipe_box box={x,8};
   unsigned actual=staging?trans.offset:x;
   option=enabled; sync_fail=fail; calls=unmapped=destroyed=unlocked=0;
   bool remote=(usage&PIPE_MAP_READ)&&enabled&&!strcmp(enabled,"1");
   bool sync=!coherent||remote;
   bool ok=sync_buffer(&screen,staging?&copy:&normal,&res,&trans,usage,&box,actual);
   assert(ok==!(sync&&fail));
   if (sync) {
      assert(calls==1);
      assert(emitted.offset==bo_offset+(remote?actual:trans.offset)&&emitted.size==8);
   } else assert(calls==0);
   if (sync&&fail) {
      assert(unmapped==1&&bo.maps==0&&destroyed==1&&original.refs==0);
      assert(!trans.staging_res&&!trans.base.b.resource);
      assert(temporary.refs==(staging?0:1)&&unlocked==(unsigned)staging);
      for(unsigned i=0;i<1024;i++)assert(bo.guest[i]==0x17);
   } else {
      assert(!destroyed&&!unmapped&&original.refs==1&&temporary.refs==1);
      for(unsigned i=0;i<1024;i++) {
         bool copied=sync&&i>=emitted.offset&&i<emitted.offset+emitted.size;
         assert(bo.guest[i]==(copied?0xa5:0x17));
      }
      if(remote)assert(!memcmp(bo.guest+bo_offset+actual,bo.host+bo_offset+actual,8));
   }
   for(unsigned i=0;i<1024;i++)assert(bo.host[i]==0xa5);
}
int main(void) {
   test(false,128,64,PIPE_MAP_READ,"1",true,false);
   const char *options[]={NULL,"0","1","10"};
   unsigned count=0;
   for(unsigned st=0;st<2;st++)for(unsigned bo=0;bo<2;bo++)for(unsigned x=0;x<2;x++)
   for(unsigned usage=1;usage<=3;usage++)for(unsigned opt=0;opt<4;opt++)
   for(unsigned coherent=0;coherent<2;coherent++)for(unsigned fail=0;fail<2;fail++) {
      test(st,bo?128:0,x?64:0,usage,options[opt],coherent,fail); count++;
   }
   printf("%u direct/staging, offsets, READ/READ_WRITE/WRITE, opt-out and failure cases PASS\n",count);
}
'''


def code(text):
    body = function(text, 'static void *\nzink_buffer_map(')
    begin = body.index('   /* WineHua Venus uses separate Host and Guest mappings.')
    end = body.index('   trans->base.b.usage = usage;', begin)
    cleanup = body[body.index('fail:\n'):body.rindex('}')]
    cleanup = cleanup.replace('return NULL;', 'return false;')
    return PREFIX + '''
static bool sync_buffer(struct zink_screen *screen,struct zink_context *ctx,
   struct zink_resource *res,struct zink_transfer *trans,unsigned usage,
   const struct pipe_box *box,unsigned map_offset) {
   (void)map_offset;
   (void)pipe_resource_reference;
''' + body[begin:end] + '\nreturn true;\n' + cleanup + '}\n' + MAIN


with tempfile.TemporaryDirectory(prefix='zink-shadow-buffer-') as directory:
    tmp = Path(directory)
    old = replay(tmp / 'old', a.mesa_git, stop_before='0026-')
    current = replay(tmp / 'current', a.mesa_git)
    compile_run(tmp, code(old), 'old-offset', expect_failure='emitted.offset==')
    compile_run(tmp, code(current), 'buffer')
    compile_run(tmp, code(current), 'buffer-sanitized', sanitized=True)

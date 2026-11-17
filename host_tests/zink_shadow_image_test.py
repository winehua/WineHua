#!/usr/bin/env python3
"""Run real image map/flush/unmap code against separate Host/Guest memories."""
import argparse
from pathlib import Path
import resource
import subprocess
import tempfile
from zink_shadow_test_support import ROOT, compile_run, function, replay

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mesa-git', type=Path, default=ROOT / 'thirdparty/mesa')
p.add_argument('--reference-patch', type=Path, help='Optional externally supplied negative control')
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Mesa mixes signed pipe_box coordinates with unsigned transfer offsets. */
#pragma GCC diagnostic ignored "-Wsign-compare"
typedef uint64_t VkDeviceSize;
typedef int VkResult;
enum pipe_format { RGBA8, BC1, PIPE_FORMAT_S8_UINT };
enum { VK_SUCCESS=0, PIPE_BUFFER, PIPE_TEXTURE_2D, PIPE_TEXTURE_3D,
       PIPE_TEXTURE_2D_ARRAY, PIPE_TEXTURE_1D_ARRAY, PIPE_TEXTURE_CUBE_ARRAY };
enum { PIPE_MAP_READ=1, PIPE_MAP_WRITE=2, PIPE_MAP_FLUSH_EXPLICIT=4, PIPE_MAP_COHERENT=8,
       PIPE_MAP_UNSYNCHRONIZED=16, PIPE_MAP_DEPTH_ONLY=32, PIPE_MAP_STENCIL_ONLY=64,
       TC_TRANSFER_MAP_THREADED_UNSYNC=128, PIPE_MAP_THREAD_SAFE=256, PIPE_MAP_ONCE=512,
       PIPE_MAP_PERSISTENT=1024, PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE=2048,
       PIPE_RESOURCE_FLAG_DRV_PRIV=4096, ZINK_RESOURCE_FLAG_SHADOW_STAGING=4096,
       ZINK_ALLOC_NO_SUBALLOC=1, PIPE_USAGE_STAGING=1, PIPE_USAGE_STREAM=2,
       PIPE_BIND_LINEAR=1, ZINK_MAP_TEMPORARY=8192,
       VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE=1, ZINK_RESOURCE_ACCESS_WRITE=1 };
#define ASSERTED
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define VKSCR(n) n
#define mesa_loge(...) ((void)0)
struct memory { uint8_t host[16384],guest[16384]; unsigned maps; };
struct zink_resource_object { bool coherent,host_visible,is_buffer,exportable,transfer_dst;
   VkDeviceSize offset,size; struct memory *bo; int image; unsigned modifier_aspect; };
struct pipe_box { int x,y,z,width,height,depth; };
struct pipe_resource { unsigned target; enum pipe_format format; unsigned usage,bind,width0,
   height0,depth0,last_level,array_size,flags,refs; struct pipe_resource *next; };
struct zink_resource { struct { struct pipe_resource b; } base; struct zink_resource_object *obj;
   bool linear,valid; unsigned fb_bind_count,modifiers,aspect; };
struct pipe_transfer { struct pipe_resource *resource; unsigned usage,level,stride,layer_stride;
   struct pipe_box box; };
struct zink_transfer { struct { struct pipe_transfer b; } base;
   struct pipe_resource *staging_res; unsigned offset,depthPitch; };
struct zink_screen { int dev; bool device_lost;
   struct { struct { struct { VkDeviceSize nonCoherentAtomSize; } limits; } props; } info; };
struct pipe_context { struct zink_screen *screen; };
struct zink_context { struct pipe_context base; bool rp_loadop_changed; };
typedef struct { unsigned aspectMask,mipLevel,arrayLayer; } VkImageSubresource;
typedef struct { VkDeviceSize offset,size,rowPitch,arrayPitch,depthPitch; } VkSubresourceLayout;
typedef struct { int sType; void *pNext; struct memory *memory; VkDeviceSize offset,size; } VkMappedMemoryRange;
struct util_format_description { struct { unsigned width,height,bits; } block; };
static struct memory image_mem,stage_mem;
static struct zink_resource_object stage_obj;
static struct zink_resource stage_res;
static struct zink_transfer transfer;
static VkSubresourceLayout layout;
static bool enabled,alloc_fail,map_fail,sync_fail,noncoherent;
static unsigned invalidates,flushes,copies,fences,waits,destroyed,unmaps,allocations;
static const char *os_get_option(const char *name) { (void)name; return enabled?"1":"0"; }
static bool __attribute__((unused)) debug_get_bool_option(const char *name,bool fallback) {
   (void)name;(void)fallback;return enabled;
}
static struct zink_screen *zink_screen(struct zink_screen *p) { return p; }
static struct zink_context *zink_context(struct pipe_context *p) { return (struct zink_context*)p; }
static struct zink_resource *zink_resource(struct pipe_resource *p) { return (struct zink_resource*)p; }
static unsigned util_format_get_blockheight(enum pipe_format f) { return f==BC1?4:1; }
static enum pipe_format util_format_get_depth_only(enum pipe_format f) { return f; }
static unsigned util_format_get_blockwidth(enum pipe_format f) { return f==BC1?4:1; }
static unsigned util_format_get_blocksize(enum pipe_format f) { return f==BC1?8:4; }
static unsigned util_format_get_nblocksy(enum pipe_format f,unsigned h) {
   unsigned b=util_format_get_blockheight(f); return (h+b-1)/b;
}
static unsigned util_format_get_stride(enum pipe_format f,unsigned w) {
   unsigned b=util_format_get_blockwidth(f); return (w+b-1)/b*util_format_get_blocksize(f);
}
static unsigned util_format_get_2d_size(enum pipe_format f,unsigned s,unsigned h) {
   return util_format_get_nblocksy(f,h)*s;
}
static const struct util_format_description *util_format_description(enum pipe_format f) {
   static const struct util_format_description desc[]={{{1,1,32}},{{4,4,64}}}; return &desc[f];
}
static struct memory *zink_bo_get_mem(struct memory *m) { return m; }
static int InvalidateMappedMemoryRanges(int dev,unsigned n,const VkMappedMemoryRange *r) {
   (void)dev; assert(n==1);invalidates++;assert(fences||waits);if(sync_fail)return -1;
   assert(r->offset+r->size<=sizeof r->memory->host);
   memcpy(r->memory->guest+r->offset,r->memory->host+r->offset,r->size);return VK_SUCCESS;
}
static int FlushMappedMemoryRanges(int dev,unsigned n,const VkMappedMemoryRange *r) {
   (void)dev; assert(n==1);flushes++;if(sync_fail)return -1;
   assert(r->offset+r->size<=sizeof r->memory->host);
   memcpy(r->memory->host+r->offset,r->memory->guest+r->offset,r->size);return VK_SUCCESS;
}
static bool zink_is_swapchain(struct zink_resource *r) { (void)r; return false; }
static void zink_kopper_acquire(struct zink_context *c,struct zink_resource *r,unsigned x) { (void)c;(void)r;(void)x; }
static int zink_rect_from_box(const struct pipe_box *b) { (void)b;return 0; }
static void zink_fb_clears_apply_or_discard(struct zink_context *c,struct pipe_resource *r,int b,bool x) { (void)c;(void)r;(void)b;(void)x; }
static void zink_fb_clears_apply_region(struct zink_context *c,struct pipe_resource *r,int b) { (void)c;(void)r;(void)b; }
static bool zink_resource_usage_is_unflushed_write(struct zink_resource *r) { (void)r;return false; }
static bool zink_resource_has_usage(struct zink_resource *r) { (void)r;return true; }
static void zink_resource_usage_wait(struct zink_context *c,struct zink_resource *r,int x) { (void)c;(void)r;(void)x;waits++; }
static void zink_fence_wait(struct pipe_context *p) { (void)p;fences++; }
static struct zink_transfer *create_transfer(struct zink_context *c,struct pipe_resource *r,unsigned u,const struct pipe_box *b) {
   (void)c;assert(!transfer.base.b.resource);memset(&transfer,0,sizeof transfer);
   r->refs++;transfer.base.b=(struct pipe_transfer){.resource=r,.usage=u,.box=*b};return &transfer;
}
static void destroy_transfer(struct zink_context *c,struct zink_transfer *t) { (void)c;(void)t;destroyed++; }
static void pipe_resource_reference(struct pipe_resource **dst,struct pipe_resource *src) {
   assert(!src);if(*dst){assert((*dst)->refs);(*dst)->refs--;}*dst=src;
}
static void *map_resource(struct zink_screen *s,struct zink_resource *r) {
   (void)s;if(map_fail)return NULL;r->obj->bo->maps++;return r->obj->bo->guest+r->obj->offset;
}
static void zink_bo_unmap(struct zink_screen *s,struct memory *m) { (void)s;assert(m->maps);m->maps--;unmaps++; }
static void GetImageSubresourceLayout(int dev,int img,const VkImageSubresource *s,VkSubresourceLayout *l) {
   (void)dev;(void)img;(void)s;*l=layout;
}
static bool zink_winehua_queue_mapped_flush(struct zink_context *c,struct zink_resource_object *o,const VkMappedMemoryRange *r) { (void)c;(void)o;(void)r;return false; }
static void zink_copy_buffer(struct zink_context *c,struct zink_resource *d,struct zink_resource *s,VkDeviceSize x,VkDeviceSize y,VkDeviceSize n) { (void)c;(void)d;(void)s;(void)x;(void)y;(void)n;assert(false); }
static unsigned allocation_flags(const struct pipe_resource *templ,bool extended);
static struct pipe_resource *zink_resource_create(struct zink_screen *s,const struct pipe_resource *t) {
   (void)s;allocations++;if(alloc_fail)return NULL;
   assert(allocation_flags(t,true)==ZINK_ALLOC_NO_SUBALLOC);
   bool isolated=allocation_flags(t,false)&ZINK_ALLOC_NO_SUBALLOC;
   stage_obj=(struct zink_resource_object){.host_visible=true,.is_buffer=true,.transfer_dst=true,
      .coherent=!noncoherent,.offset=isolated?0:256,.size=isolated?4096:16384,.bo=&stage_mem};
   stage_res=(struct zink_resource){.base.b=*t,.obj=&stage_obj};stage_res.base.b.refs=1;
   return &stage_res.base.b;
}
/* The GPU oracle uses the image's actual Vulkan pitches, not transfer strides. */
static unsigned image_offset(struct zink_resource *r,unsigned x,unsigned y,unsigned z) {
   enum pipe_format f=r->base.b.format;
   return r->obj->offset+layout.offset+z*(r->base.b.target==PIPE_TEXTURE_3D?layout.depthPitch:layout.arrayPitch)+
      (y/util_format_get_blockheight(f))*layout.rowPitch+(x/util_format_get_blockwidth(f))*util_format_get_blocksize(f);
}
static void zink_copy_image_buffer(struct zink_context *c,struct zink_resource *dst,
   struct zink_resource *src,unsigned dl,unsigned dx,unsigned dy,unsigned dz,
   unsigned sl,const struct pipe_box *box,unsigned usage) {
   (void)c;(void)dl;(void)sl;(void)usage;copies++;
   bool upload=src->base.b.target==PIPE_BUFFER;
   struct zink_resource *img=upload?dst:src,*buf=upload?src:dst;
   unsigned width=util_format_get_stride(img->base.b.format,box->width);
   unsigned rows=util_format_get_nblocksy(img->base.b.format,box->height);
   for(int z=0;z<box->depth;z++)for(unsigned y=0;y<rows;y++) {
      unsigned io=image_offset(img,upload?dx:(unsigned)box->x,
         (upload?dy:(unsigned)box->y)+y*util_format_get_blockheight(img->base.b.format),
         (upload?dz:(unsigned)box->z)+z);
      unsigned bo=buf->obj->offset+(upload?(unsigned)box->x:dx)+(z*rows+y)*width;
      assert(io+width<sizeof image_mem.host&&bo+width<sizeof stage_mem.host);
      if(upload)memcpy(img->obj->bo->host+io,buf->obj->bo->host+bo,width);
      else memcpy(buf->obj->bo->host+bo,img->obj->bo->host+io,width);
   }
}
'''

MAIN = r'''
static uint8_t pattern(unsigned i) { return (uint8_t)(i*7+i/64+3); }
static void reset(void) {
   memset(&transfer,0,sizeof transfer);
   for(unsigned i=0;i<sizeof image_mem.host;i++)image_mem.host[i]=pattern(i);
   memset(image_mem.guest,0x17,sizeof image_mem.guest);
   memset(stage_mem.host,0xc3,sizeof stage_mem.host);memset(stage_mem.guest,0x29,sizeof stage_mem.guest);
   stage_mem.maps=image_mem.maps=0;
   invalidates=flushes=copies=fences=waits=destroyed=unmaps=allocations=0;
   alloc_fail=map_fail=sync_fail=noncoherent=false;
   layout=(VkSubresourceLayout){.offset=16,.rowPitch=64,.depthPitch=512,.arrayPitch=1024};
}
static void test(unsigned target,enum pipe_format format,unsigned usage,unsigned atom,unsigned partial,bool coherent) {
   reset();enabled=true;noncoherent=!coherent;
   struct zink_screen screen={.info.props.limits.nonCoherentAtomSize=atom};
   struct zink_context ctx={.base.screen=&screen};
   struct zink_resource_object obj={.coherent=coherent,.host_visible=true,.transfer_dst=true,.offset=128,.size=16384,.bo=&image_mem};
   struct zink_resource res={.base.b={.target=target,.format=format,.refs=1},.obj=&obj,.linear=true};
   unsigned bw=util_format_get_blockwidth(format),bh=util_format_get_blockheight(format);
   struct pipe_box box={bw,2*bh,1,3*bw,3*bh,target==PIPE_TEXTURE_2D?1:2};
   if(format==BC1)box.width--,box.height--; /* edge blocks need ceil division */
   if(target==PIPE_TEXTURE_1D_ARRAY)box.y=0,box.height=1;
   struct pipe_transfer *out=NULL;
   uint8_t *map=zink_image_map(&ctx.base,&res.base.b,2,usage|(partial?PIPE_MAP_FLUSH_EXPLICIT:0),&box,&out);
   assert(map&&out&&out->resource==&res.base.b);
   /* Check data before implementation details so both old controls fail on behavior. */
   if(usage&PIPE_MAP_READ)for(int z=0;z<box.depth;z++)
   for(unsigned y=0;y<util_format_get_nblocksy(format,box.height);y++)
   for(unsigned i=0;i<util_format_get_stride(format,box.width);i++) {
      unsigned io=image_offset(&res,box.x,box.y+y*bh,box.z+z);
      assert(map[z*out->layer_stride+y*out->stride+i]==pattern(io+i));
   }
   assert(allocations==1&&transfer.staging_res&&stage_obj.offset==0);
   unsigned stride=util_format_get_stride(format,box.width),rows=util_format_get_nblocksy(format,box.height);
   assert(out->stride==stride&&out->layer_stride==stride*rows);
   assert(!flushes); /* No WRITE-only publication before the caller writes. */
   assert(invalidates==((usage&PIPE_MAP_READ)?1u:0u));
   struct pipe_box region={0,0,0,box.width,box.height,box.depth};
   if(partial) {
      region.x=bw;region.width=bw;
      if(partial==1) {
         region.z=box.depth-1;region.depth=1;
         if(box.height>1)region.y=bh,region.height=bh;
      }
   }
   if(usage&PIPE_MAP_WRITE) {
      /* Deliberately dirty the whole mapping; FLUSH_EXPLICIT must copy only region. */
      memset(map,0x5a,out->layer_stride*box.depth);
      if(partial)zink_transfer_flush_region(&ctx.base,out,&region);
   }
   transfer_unmap(&ctx.base,out);
   assert(destroyed==1&&res.base.b.refs==1&&!transfer.staging_res&&!transfer.base.b.resource);
   assert(stage_res.base.b.refs==0);
   uint8_t expected[16384];for(unsigned i=0;i<sizeof expected;i++)expected[i]=pattern(i);
   if(usage&PIPE_MAP_WRITE)for(int z=0;z<region.depth;z++)
   for(unsigned y=0;y<util_format_get_nblocksy(format,region.height);y++) {
      unsigned off=image_offset(&res,box.x+region.x,box.y+region.y+y*bh,box.z+region.z+z);
      memset(expected+off,0x5a,util_format_get_stride(format,region.width));
   }
   assert(!memcmp(expected,image_mem.host,sizeof expected));
   for(unsigned i=stage_res.base.b.width0;i<sizeof stage_mem.host;i++) {
      /* Atom expansion can touch private padding, never the separate sibling sentinel. */
      if(i>=stage_obj.size)assert(stage_mem.host[i]==0xc3);
   }
   assert(!screen.device_lost);
}
static void failures(void) {
   for(unsigned kind=0;kind<4;kind++) {
      reset();enabled=true;alloc_fail=kind==0;map_fail=kind==1;sync_fail=kind==2;
      struct zink_screen screen={.info.props.limits.nonCoherentAtomSize=256};
      struct zink_context ctx={.base.screen=&screen};
      struct zink_resource_object obj={.coherent=true,.host_visible=true,.transfer_dst=true,.size=16384,.bo=&image_mem};
      struct zink_resource res={.base.b={.target=PIPE_TEXTURE_2D,.format=RGBA8,.refs=1},.obj=&obj,.linear=true};
      struct pipe_box box={1,1,0,2,2,1};struct pipe_transfer *out=NULL;
      void *map=zink_image_map(&ctx.base,&res.base.b,0,kind==3?PIPE_MAP_WRITE:PIPE_MAP_READ,&box,&out);
      if(kind<3) {
         assert(!map&&!out&&res.base.b.refs==1&&destroyed==1);
         assert(!transfer.staging_res&&!transfer.base.b.resource);
         if(kind>0)assert(stage_res.base.b.refs==0);
         assert(stage_mem.maps==0&&unmaps==(kind==2?1u:0u));
      } else {
         assert(map);sync_fail=true;transfer_unmap(&ctx.base,out);
         assert(screen.device_lost&&!copies&&res.base.b.refs==1);
         for(unsigned i=0;i<sizeof image_mem.host;i++)assert(image_mem.host[i]==pattern(i));
      }
   }
}
static void opt_out(void) {
   reset();enabled=false;
   struct zink_screen screen={.info.props.limits.nonCoherentAtomSize=64};
   struct zink_context ctx={.base.screen=&screen};
   struct zink_resource_object obj={.coherent=true,.host_visible=true,.offset=128,.size=16384,.bo=&image_mem};
   struct zink_resource res={.base.b={.target=PIPE_TEXTURE_3D,.format=RGBA8,.refs=1},.obj=&obj,.linear=true};
   struct pipe_box box={1,1,1,2,2,1};struct pipe_transfer *out=NULL;
   uint8_t *map=zink_image_map(&ctx.base,&res.base.b,0,PIPE_MAP_READ,&box,&out);
   assert(map==image_mem.guest+image_offset(&res,1,1,1));
   assert(!allocations&&!invalidates&&!flushes&&!transfer.staging_res);
   transfer_unmap(&ctx.base,out);assert(res.base.b.refs==1);
}
int main(void) {
   (void)zink_winehua_queue_mapped_flush;
   (void)os_get_option;
   /* Fails on old main and on the supplied contiguous-range reference patch. */
   test(PIPE_TEXTURE_3D,RGBA8,PIPE_MAP_READ,1,false,true);
   unsigned targets[]={PIPE_TEXTURE_2D,PIPE_TEXTURE_3D,PIPE_TEXTURE_2D_ARRAY,
      PIPE_TEXTURE_1D_ARRAY,PIPE_TEXTURE_CUBE_ARRAY};
   unsigned atoms[]={1,64,256,1024};unsigned count=0;
   for(unsigned t=0;t<5;t++)for(unsigned f=0;f<2;f++)for(unsigned u=1;u<=3;u++)
   for(unsigned at=0;at<4;at++)for(unsigned partial=0;partial<3;partial++)
   for(unsigned coherent=0;coherent<2;coherent++) {
      if(targets[t]==PIPE_TEXTURE_1D_ARRAY&&f==BC1)continue;
      test(targets[t],f,u,atoms[at],partial,coherent);count++;
   }
   failures();opt_out();
   printf("%u linear/array/3D/block-format, explicit/automatic, atom/padding cases + failures/opt-out PASS\n",count);
}
'''


def code(text):
    image = function(text, 'static void *\nzink_image_map(')
    flush = function(text, 'static void\nzink_transfer_flush_region(')
    unmap = function(text, 'static void\ntransfer_unmap(')
    alloc = 'return extended ? ZINK_ALLOC_NO_SUBALLOC : 0;'
    helper = ''
    if 'zink_winehua_flush_image_staging(' in text:
        helper = function(text, 'static void\nzink_winehua_flush_image_staging(')
        alloc = text.split('   const unsigned bo_flags = ', 1)[1].split(';', 1)[0]
        alloc = 'return ' + alloc.replace('mai.pNext', 'extended') + ';'
    # All stubs exist for both variants so the unchanged production path compiles.
    prefix = PREFIX + '''
static unsigned allocation_flags(const struct pipe_resource *templ,bool extended) {
   (void)templ; ''' + alloc + '''
}
'''
    functions = '\n'.join(function(text, marker) for marker in [
        'ALWAYS_INLINE static void\nalign_offset_size(',
        'VkMappedMemoryRange\nzink_resource_init_mem_range(',
        'static void\nzink_transfer_copy_bufimage('])
    functions = functions.replace('ALWAYS_INLINE ', '')
    return prefix + functions + '\n' + image + '\n' + helper + '\n' + flush + '\n' + unmap + '\n' + MAIN


with tempfile.TemporaryDirectory(prefix='zink-shadow-image-') as directory:
    tmp = Path(directory)
    old = replay(tmp / 'old', a.mesa_git, stop_before='0027-')
    current = replay(tmp / 'current', a.mesa_git)
    assertion = 'map[z*out->layer_stride+y*out->stride+i]==pattern(io+i)'
    compile_run(tmp, code(old), 'old-linear', expect_failure=assertion)
    if a.reference_patch:
        reference_dir = tmp / 'reference'
        replay(reference_dir, a.mesa_git, stop_before='0015-')
        source = reference_dir / 'src/gallium/drivers/zink/zink_resource.c'
        text = source.read_text()
        for before in ['      ptr = map_resource(screen, staging_res);\n   } else {',
                       '         assert(src_offset + size <= res->obj->size);\n      }']:
            assert text.count(before) == 1
            text = text.replace(before, before.replace('\n   }', '\n\n   }').replace('\n      }', '\n\n      }'))
        source.write_text(text)
        subprocess.run(['git', '-C', str(reference_dir), 'apply', str(a.reference_patch)], check=True)
        reference = code(source.read_text()).replace(
            'test(PIPE_TEXTURE_3D,RGBA8,PIPE_MAP_READ,1,false,true);',
            'test(PIPE_TEXTURE_3D,RGBA8,PIPE_MAP_READ,1,false,false);')
        compile_run(tmp, reference, 'reference-linear', expect_failure=assertion)
    compile_run(tmp, code(current), 'image')
    compile_run(tmp, code(current), 'image-sanitized', sanitized=True)

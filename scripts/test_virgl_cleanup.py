#!/usr/bin/env python3
"""Run the real renderer cleanup helpers under ASan with both teardown orders."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENUS = ROOT / 'thirdparty/virglrenderer/src/venus'


def function(text, name):
    start = text.index(name + '(')
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux cc/ASan')
class CleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='winehua-cleanup-')
        cls.addClassCleanup(cls.temp.cleanup)
        buffer = (VENUS / 'vkr_buffer.c').read_text()
        memory = (VENUS / 'vkr_device_memory.c').read_text()
        # Find definitions rather than forward declarations.
        memory = memory[memory.index('\nvkr_device_memory_release('):]
        text = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <threads.h>
#include "util/list.h"
#define __OHOS__ 1
typedef uint64_t VkDeviceSize;
struct vkr_context { mtx_t object_mutex; };
struct vkr_device_memory;
struct vkr_buffer {
    struct vkr_device_memory *bound_memory;
    VkDeviceSize bound_memory_offset;
    struct list_head memory_head;
    bool memory_listed;
    void *winehua_ubo_watches;
    atomic_uint winehua_ubo_watch_count;
};
struct vn_device_proc_table { void (*UnmapMemory)(void *,void *); };
struct vkr_device { struct vn_device_proc_table proc_table; struct {struct {void *device;} handle;} base; };
struct vkr_device_memory {
    struct vkr_context *context;
    struct vkr_device *device;
    struct {struct {void *device_memory;} handle;} base;
    struct list_head bound_buffers;
    struct list_head shadow_dirty_head;
    bool shadow_dirty_listed;
    void *host_map, *shadow_map;
    size_t shadow_size;
    int shadow_fd, udmabuf_fd;
    void *shadow_dirty_ranges, *shadow_coverage_ranges, *shadow_upload_snapshot;
    bool shadow_host_copy_deferred, shadow_coverage_valid;
    uint32_t shadow_dirty_range_count, shadow_dirty_range_capacity;
    uint32_t shadow_coverage_range_count, shadow_coverage_range_capacity;
    void *mtl_shm, *gbm_bo;
};
static unsigned unmaps;
static void unmap_stub(void *device, void *memory) { (void)device;(void)memory;unmaps++; }
static void vkr_device_memory_invalidate_shadow_coverage(struct vkr_device_memory *mem) { mem->shadow_coverage_valid=false; }
static void vkr_mtl_shm_free(void *ptr) { free(ptr); }
static void vkr_gbm_bo_destroy(void *ptr) { free(ptr); }
'''
        text += '\nstatic void\n' + function(buffer, 'vkr_winehua_set_buffer_memory')
        text += '\nstatic void\n' + function((VENUS / 'vkr_device_memory.c').read_text(), 'vkr_ohos_shadow_dirty_list_remove')
        text += '\nvoid\n' + function(buffer, 'vkr_buffer_release')
        text += '\nvoid\n' + function(memory, 'vkr_device_memory_release')
        text += r'''
static void check(unsigned order) {
    struct vkr_context ctx;
    assert(mtx_init(&ctx.object_mutex,mtx_plain)==thrd_success);
    struct vkr_device dev={.proc_table={.UnmapMemory=unmap_stub}};
    struct vkr_device_memory *mem=calloc(1,sizeof(*mem));
    mem->context=&ctx;mem->device=&dev;mem->shadow_fd=-1;mem->udmabuf_fd=-1;
    list_inithead(&mem->bound_buffers);list_inithead(&mem->shadow_dirty_head);
    mem->host_map=(void*)1;
    mem->shadow_size=4096;
    mem->shadow_map=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mem->shadow_map!=MAP_FAILED);
    mem->shadow_dirty_ranges=malloc(32);mem->shadow_coverage_ranges=malloc(32);mem->shadow_upload_snapshot=malloc(64);
    struct vkr_buffer *buffers[64];
    for(unsigned i=0;i<64;i++) {
        buffers[i]=calloc(1,sizeof(*buffers[i]));
        list_inithead(&buffers[i]->memory_head);
        buffers[i]->winehua_ubo_watches=malloc(32);
        atomic_init(&buffers[i]->winehua_ubo_watch_count,1);
        vkr_winehua_set_buffer_memory(&ctx,buffers[i],mem,i*16);
    }
    if(order==1) vkr_device_memory_release(mem);
    for(unsigned i=0;i<64;i++) {
        if(order!=2) {
            vkr_buffer_release(&ctx,buffers[i]);
            assert(!buffers[i]->bound_memory&&!buffers[i]->memory_listed);
            vkr_buffer_release(&ctx,buffers[i]);
        }
        free(buffers[i]);
    }
    vkr_device_memory_release(mem);
    assert(list_is_empty(&mem->bound_buffers));
    assert(!mem->host_map&&!mem->shadow_map&&!mem->shadow_upload_snapshot);
    free(mem);mtx_destroy(&ctx.object_mutex);
}
int main(int argc,char **argv) {
    (void)argv;
    if(argc>1) {check(2);return 0;}
    for(unsigned i=0;i<100;i++){check(0);check(1);}
    assert(unmaps==200);
    return 0;
}
'''
        directory = Path(cls.temp.name)
        source = directory / 'cleanup.c'
        source.write_text(text)
        cls.exe = directory / 'cleanup'
        subprocess.run(['cc', '-g', '-O1', '-std=gnu11', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-Wall', '-Wextra', '-Werror',
                        '-I', str(VENUS.parent / 'mesa'), str(source), '-pthread', '-o', str(cls.exe)], check=True)

    def test_both_teardown_orders_and_repeat_release(self):
        subprocess.run([str(self.exe)], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}, check=True)

    def test_original_implicit_buffer_free_reproduces_use_after_free(self):
        result = subprocess.run([str(self.exe), '--original-order'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('heap-use-after-free', result.stderr)

    def test_implicit_device_cleanup_uses_helpers_before_driver_free(self):
        body = function((VENUS / 'vkr_device.c').read_text(), 'vkr_device_object_destroy')
        self.assertLess(body.index('vkr_buffer_release('), body.index('vk->DestroyBuffer('))
        self.assertLess(body.index('vkr_device_memory_release('), body.index('vk->FreeMemory('))
        self.assertEqual(body.count('vkr_device_memory_release('), 1)


if __name__ == '__main__':
    unittest.main()

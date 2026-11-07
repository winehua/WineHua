"""Run the renderer's actual implicit readback branch against pending writes."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/virgl-shadow-explicit-readback.patch'
RELATIVE = Path('src/venus/vkr_device_memory.c')

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <inttypes.h>
struct vkr_device_memory;
struct VkMappedMemoryRange { vkr_device_memory *memory; size_t offset,size; };
using VkResult = int;
static constexpr int VK_SUCCESS = 0;
static constexpr int VK_ERROR_MEMORY_MAP_FAILED = -5;
struct vkr_shadow_sync_stats { unsigned bytes=0,copies=0,cache_ops=0; };
struct Memory {
    unsigned shadow_guest_write_depth=0;
    bool shadow_remote_active=false,shadow_host_dirty=false;
    unsigned shadow_dirty_range_count=0;
    unsigned shadow_remote_invalidate_count=0;
    bool shadow_dirty_range_overflow=false;
    uint8_t host[16]{},shadow[16]{},snapshot[16]{};
    uint8_t *host_map=host,*shadow_map=shadow,*shadow_upload_snapshot=snapshot;
};
static unsigned invalidates,removes;
struct Proc {
    int InvalidateMappedMemoryRanges(int,unsigned,const VkMappedMemoryRange*) {
        invalidates++; return VK_SUCCESS;
    }
};
struct Device { struct { struct {int device=0;} handle; } base; };
struct vkr_device_memory:Memory { Device *device; };
using Allocation=vkr_device_memory;
struct vkr_context { int object_mutex=0; };
struct vn_dispatch_context { vkr_context *data; };
struct vn_command_vkInvalidateMappedMemoryRanges {
    VkResult ret;
    unsigned memoryRangeCount;
    const VkMappedMemoryRange *pMemoryRanges;
};
static const char *host_mode;
static unsigned explicit_reads;
static const char *os_get_option(const char*) { return host_mode; }
static void mtx_lock(int*) {}
static void mtx_unlock(int*) {}
static Allocation *vkr_device_memory_from_handle(Allocation *mem) { return mem; }
static bool vkr_ohos_shadow_trace_enabled() { return false; }
static int vkr_device_memory_invalidate_shadow_range(Allocation *mem,size_t offset,size_t size) {
    assert(offset+size<=16);
    explicit_reads++;
    memcpy(mem->shadow_map+offset,mem->host_map+offset,size);
    return VK_SUCCESS;
}
template<typename... T> static void vkr_ohos_gate_c_trace_memory(T...) {}
template<typename... T> static void vkr_log(T...) {}
static void vkr_ohos_shadow_dirty_list_remove(Allocation*) { removes++; }
static vkr_shadow_sync_stats readback(Allocation *mem) {
    vkr_shadow_sync_stats stats;
    Proc proc,*vk=&proc;
    VkMappedMemoryRange range;
    const bool coherent=true;
    const size_t copy_size=16;
    BRANCH
    return stats;
}
static void check(bool remote,bool writing,bool dirty) {
    Device dev;
    Allocation mem;
    mem.device=&dev;
    mem.shadow_remote_active=remote;
    mem.shadow_guest_write_depth=writing;
    mem.shadow_host_dirty=dirty;
    mem.shadow_dirty_range_count=dirty ? 3 : 0;
    mem.shadow_dirty_range_overflow=dirty;
    memset(mem.host_map,0x11,16);
    memset(mem.shadow_map,0x22,16);
    memset(mem.shadow_upload_snapshot,0x33,16);
    invalidates=removes=0;
    const auto stats=readback(&mem);
    if(remote || writing) {
        assert(invalidates==0 && removes==0 && stats.copies==0);
        for(unsigned i=0;i<16;i++) {
            assert(mem.shadow_map[i]==0x22);
            assert(mem.shadow_upload_snapshot[i]==0x33);
        }
        assert(mem.shadow_host_dirty==dirty);
        assert(mem.shadow_dirty_range_count==(dirty ? 3u : 0u));
        assert(mem.shadow_dirty_range_overflow==dirty);
    } else {
        assert(invalidates==1 && stats.copies==1 && stats.bytes==16);
        for(unsigned i=0;i<16;i++) {
            assert(mem.shadow_map[i]==0x11);
            assert(mem.shadow_upload_snapshot[i]==0x11);
        }
    }
}
#define __OHOS__ 1
DISPATCHER
static void check_dispatch(const char *mode,bool legacy) {
    Device dev;
    Allocation mem;
    mem.device=&dev;
    memset(mem.host_map,0x44,16);
    memset(mem.shadow_map,0x22,16);
    vkr_context ctx;
    vn_dispatch_context dispatch{&ctx};
    const VkMappedMemoryRange range{&mem,8,4};
    vn_command_vkInvalidateMappedMemoryRanges args{-1,1,&range};
    host_mode=mode;
    explicit_reads=0;
    vkr_dispatch_vkInvalidateMappedMemoryRanges(&dispatch,&args);
    assert(args.ret==VK_SUCCESS);
    assert(explicit_reads==(legacy ? 0u : 1u));
    assert(mem.shadow_guest_write_depth==(legacy ? 1u : 0u));
    for(unsigned i=0;i<16;i++)
        assert(mem.shadow_map[i]==(!legacy && i>=8 && i<12 ? 0x44 : 0x22));
}
int main() {
    for(int remote=0;remote<2;remote++)
        for(int writing=0;writing<2;writing++)
            for(int dirty=0;dirty<2;dirty++) check(remote,writing,dirty);
    const char *modes[]={nullptr,"full","precise","none"};
    for(const char *mode:modes) check_dispatch(mode,false);
    check_dispatch("legacy-write-begin",true);
}
'''


class ExplicitReadbackTests(unittest.TestCase):
    def test_unrelated_fence_preserves_explicit_writes(self):
        source = Path(os.environ.get('WINEHUA_VIRGL_MEMORY_SOURCE',
            str(ROOT / 'thirdparty/virglrenderer' / RELATIVE)))
        if source.exists():
            text = source.read_text(encoding='utf-8')
        else:
            commit = subprocess.check_output(['git','rev-parse','HEAD:thirdparty/virglrenderer'],
                                            cwd=ROOT,text=True).strip()
            text = subprocess.check_output(['git','-C',str(ROOT/'thirdparty/virglrenderer'),
                'show',commit+':'+RELATIVE.as_posix()],text=True,encoding='utf-8')
        compiler=os.environ.get('CXX') or shutil.which('g++') or shutil.which('c++')
        self.assertIsNotNone(compiler,'C++17 host compiler required')
        with tempfile.TemporaryDirectory(prefix='winehua-explicit-readback-') as temp:
            work=Path(temp)
            path=work/RELATIVE
            path.parent.mkdir(parents=True)
            path.write_text(text,encoding='utf-8')
            reverse=subprocess.run(['git','apply','--reverse','--check',str(PATCH)],
                                   cwd=work,capture_output=True)
            if reverse.returncode:
                subprocess.run(['git','apply','--check',str(PATCH)],cwd=work,check=True)
                subprocess.run(['git','apply',str(PATCH)],cwd=work,check=True)
                text=path.read_text(encoding='utf-8')
            start=text.index('\nvkr_device_memory_sync_shadow(')
            end=text.index('\nstatic VkResult\nvkr_device_memory_flush_shadow_range(',start)
            function=text[start:end]
            block=function.rindex('   } else {')+len('   } else {')
            branch=function[block:function.index('\n   }\n#else',block)]
            ds=text.index('\nvkr_dispatch_vkInvalidateMappedMemoryRanges(')
            de=text.index('\nstatic void\nvkr_dispatch_vkGetMemoryResourcePropertiesMESA(',ds)
            dispatcher='static void'+text[ds:de]
            for variant in ('fixed','old-broadcast','old-invalidate'):
                selected=branch.replace(' || mem->shadow_remote_active','') if variant=='old-broadcast' else branch
                dispatch=dispatcher.replace('!mode || strcmp(mode, "legacy-write-begin")',
                    'mode && !strcmp(mode, "precise")') if variant=='old-invalidate' else dispatcher
                cpp=work/(variant+'.cpp')
                exe=cpp.with_suffix('.exe' if sys.platform=='win32' else '')
                cpp.write_text(HARNESS.replace('BRANCH',selected).replace('DISPATCHER',dispatch),encoding='utf-8')
                flags=['-std=c++17','-O1','-Wall','-Wextra','-Werror']
                flags+=['-static'] if sys.platform=='win32' else ['-fsanitize=address,undefined']
                subprocess.run([compiler,*flags,str(cpp),'-o',str(exe)],check=True)
                result=subprocess.run([str(exe)],capture_output=True,timeout=10)
                if variant!='fixed':
                    self.assertNotEqual(result.returncode,0,'Old contract must fail: '+variant)
                else:
                    self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))


if __name__=='__main__':
    unittest.main()

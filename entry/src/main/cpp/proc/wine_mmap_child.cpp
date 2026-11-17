/**
 * wine_mmap_child.cpp — 32-bit x86 mmap 探针 (NCP 子进程)
 *
 * 在 NCP 子进程中测试 Box64/Wine 所需 mmap 特性:
 * MAP_FIXED_NOREPLACE, MAP_32BIT, low-4GB 分配, RWX 等。
 * 入口 MmapTestMain() 由 wine_child 库导出。
 */
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WineChild"

static void MMAP_L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void MMAP_L(const char* fmt, ...) {
    char buf[300];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        OH_LOG_INFO(LOG_APP, "[MMAP-NCP] %{public}s", buf);
        fprintf(stderr, "[MMAP-NCP] %s\n", buf);  // also to stderr pipe
    }
}

static const char* PN(int prot) {
    static char b[8];
    if (prot == 0) return "NONE";
    snprintf(b, sizeof(b), "%s%s%s",
             (prot & PROT_READ) ? "R" : "",
             (prot & PROT_WRITE) ? "W" : "",
             (prot & PROT_EXEC) ? "X" : "");
    return b;
}

extern "C" void MmapTestMain(NativeChildProcess_Args args)
{
    MMAP_L("===== NCP mmap 32-bit probe (pid=%d) =====", getpid());
    const size_t pg = 4096;

    // ── 1. mmap_min_addr ──
    {
        FILE* f = fopen("/proc/sys/vm/mmap_min_addr", "r");
        if (f) { char buf[32]={0}; fread(buf,1,sizeof(buf)-1,f); fclose(f);
                 MMAP_L("1. mmap_min_addr = %s", buf); }
        else  { MMAP_L("1. /proc/sys/vm/mmap_min_addr: NOT READABLE (err=%d)", errno); }
    }

    // ── 2. MAP_FIXED_NOREPLACE ──
    #ifndef MAP_FIXED_NOREPLACE
    #define MAP_FIXED_NOREPLACE 0x100000
    #endif
    {
        void* a = mmap(NULL, pg*4, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (a != MAP_FAILED) {
            void* c = mmap(a, pg, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            MMAP_L("2a. NOREPLACE conflict -> %s (err=%d,%s)",
                  c==MAP_FAILED?"MAP_FAILED(expected)":"SUCCESS(unexpected)", errno, strerror(errno));
            munmap(a, pg*4);
            void* s = mmap(NULL, pg*4, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (s != MAP_FAILED) {
                munmap(s, pg*4);
                void* r = mmap(s, pg*2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
                MMAP_L("2b. NOREPLACE free -> %s (err=%d,%s)",
                      r==s?"OK_at_expected":"FAIL_or_wrong", errno, strerror(errno));
                if (r != MAP_FAILED) munmap(r, pg*2);
            }
        } else { MMAP_L("2. SKIP (anchor failed)"); }
    }

    // ── 3. Low hint ──
    {
        uintptr_t hints[] = {0x100000,0x1000000,0x10000000,0x20000000,0x40000000,0x80000000,0xF0000000};
        const char* names[] = {"1MB","16MB","256MB","512MB","1GB","2GB","3.75GB"};
        int cnt = 0;
        for (int i=0;i<7;i++) {
            void* m = mmap((void*)hints[i], pg*64, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (m != MAP_FAILED) {
                bool b4=(uintptr_t)m<0x100000000ULL, atH=(uintptr_t)m==hints[i];
                MMAP_L("3. hint=%-6s -> @%p %s %s", names[i], m, b4?"BELOW_4G":"ABOVE", atH?"at_hint":"relocated");
                munmap(m, pg*64);
                if(b4)cnt++;
            } else { MMAP_L("3. hint=%-6s -> FAIL (err=%d)", names[i], errno); }
        }
        MMAP_L("3. %d/7 below 4GB", cnt);
    }

    // ── 4. MAP_FIXED low ──
    {
        uintptr_t tgt[]={0x01000000,0x10000000,0x40000000};
        for (int i=0;i<3;i++) {
            FILE* f=fopen("/proc/self/maps","r"); bool taken=false;
            if(f){char ln[256]; while(fgets(ln,sizeof(ln),f)){uintptr_t s,e;
                   if(sscanf(ln,"%lx-%lx",&s,&e)==2&&tgt[i]>=s&&tgt[i]<e){taken=true;break;}} fclose(f);}
            if(taken){MMAP_L("4. %08lx: SKIP occupied",tgt[i]);continue;}
            void* m=mmap((void*)tgt[i],pg*16,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
            MMAP_L("4. FIXED %08lx -> %s",tgt[i],m==(void*)tgt[i]?"OK":"FAIL");
            if(m!=MAP_FAILED)munmap(m,pg*16);
        }
    }

    // ── 5. 256MB ──
    {
        size_t heap=0x10000000;
        void* m=mmap((void*)0x10000000,heap,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
        if(m==(void*)0x10000000) { MMAP_L("5. FIXED 256MB@0x10000000 -> OK"); munmap(m,heap); }
        else { MMAP_L("5. FIXED 256MB@0x10000000 -> FAIL");
               m=mmap((void*)0x10000000,heap,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
               bool b4=m!=MAP_FAILED&&(uintptr_t)m<0x100000000ULL;
               MMAP_L("5. hint 256MB -> @%p %s",m,b4?"BELOW_4G":"ABOVE");
               if(m!=MAP_FAILED)munmap(m,heap); }
    }

    // ── 6. MAP_32BIT ──
    {
        size_t sz[]={0x1000,0x10000,0x100000,0x1000000};
        for(int i=0;i<4;i++){
            void* m=mmap(NULL,sz[i],PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|0x40,-1,0);
            if(m!=MAP_FAILED){bool b4=(uintptr_t)m<0x100000000ULL;
                MMAP_L("6. MAP_32BIT sz=0x%zx -> @%p %s",sz[i],m,b4?"BELOW_4G":"above");munmap(m,sz[i]);}
            else{MMAP_L("6. MAP_32BIT sz=0x%zx -> FAIL (err=%d,%s)",sz[i],errno,strerror(errno));}
        }
    }

    // ── 7. /proc/self/maps ──
    {
        FILE* f=fopen("/proc/self/maps","r");
        if(f){char ln[512];int n=0,ark=0;uintptr_t lo=-1UL,hi=0;
              while(fgets(ln,sizeof(ln),f)){uintptr_t s,e;
                  if(sscanf(ln,"%lx-%lx",&s,&e)==2&&s<0x100000000ULL){n++;if(s<lo)lo=s;if(e>hi)hi=e;
                      if(n<=15){char*nl=strchr(ln,'\n');if(nl)*nl=0;MMAP_L("7. %s",ln);}
                      if(strstr(ln,"[anon:ark"))ark++;}}
              fclose(f);
              MMAP_L("7. low-4GB: %d regions [%08lx,%08lx) ARK=%d",n,lo,hi,ark);}
        else { MMAP_L("7. cannot open /proc/self/maps"); }
    }

    // ── 8. NOREPLACE scan ──
    {
        int hits=0,fails=0;
        for(uintptr_t cur=0x40000000UL;cur<0xF0000000UL;cur+=0x1000000UL){
            void* m=mmap((void*)cur,0x10000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
            if(m==(void*)cur){hits++;munmap(m,0x10000);}else{fails++;if(m!=MAP_FAILED)munmap(m,0x10000);}
            if(hits>=3)break;
        }
        MMAP_L("8. NOREPLACE scan: %d free, %d occupied (%s)", hits, fails,
              fails>0&&hits==0?"NOREPLACE not working!":"");
    }

    // ── 9. Hard search (no NOREPLACE) ──
    {
        size_t need=256UL*1024*1024;
        bool found=false;
        for(uintptr_t h=0x10000000UL;h<0x80000000UL;h+=0x10000000UL){
            void* m=mmap((void*)h,need,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(m!=MAP_FAILED&&(uintptr_t)m+need<=0x100000000ULL){
                MMAP_L("9. hard search: 256MB@%08lx -> OK @%p",(uintptr_t)h,m);
                munmap(m,need);found=true;break;
            }
            if(m!=MAP_FAILED)munmap(m,need);
        }
        if(!found)MMAP_L("9. hard search: 256MB <4GB NOT FOUND");
    }

    // ── 10. Dynarec: RW + mprotect RX ──
    {
        void* m=mmap(NULL,pg,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(m!=MAP_FAILED){
            ((uint32_t*)m)[0]=0xD65F03C0;
            int r=mprotect(m,pg,PROT_READ|PROT_EXEC);
            MMAP_L("10. RW+mprotect RX -> %s (err=%d)",r==0?"OK":"FAIL",errno);
            munmap(m,pg);
        }
    }

    // ── 11. RWX ──
    {
        void* m=mmap(NULL,pg,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        MMAP_L("11. ANON RWX -> %s (err=%d)",m==MAP_FAILED?"FAIL":"OK",errno);
        if(m!=MAP_FAILED)munmap(m,pg);
    }

    MMAP_L("===== NCP mmap probe DONE =====");
    fflush(stderr);
}

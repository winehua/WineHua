import json
from pathlib import Path
import subprocess

root = Path('/data/src/winehua')
src = (root / 'entry/src/main/cpp/proc/wine_child.cpp').read_text()
begin = src.index('#define WINEHUA_STALL_MAX_SAMPLES')
end = src.index('extern "C" void Main(', begin)
header = '''
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <pthread.h>
'''
main = '''
static int previousCalls;
static int stopping;
static void PreviousHandler(int, siginfo_t*, void*) {
    __atomic_fetch_add(&previousCalls, 1, __ATOMIC_RELAXED);
}
static void* Worker(void*) {
    while (!__atomic_load_n(&stopping, __ATOMIC_RELAXED)) usleep(1000);
    return nullptr;
}
int main() {
    WineHuaStallPrepareBacktrace();
    struct sigaction previous = {};
    previous.sa_sigaction = PreviousHandler;
    previous.sa_flags = SA_SIGINFO;
    sigemptyset(&previous.sa_mask);
    assert(sigaction(SIGPROF, &previous, nullptr) == 0);
    pthread_t threads[4];
    for (auto& thread : threads) assert(pthread_create(&thread, nullptr, Worker, nullptr) == 0);
    for (int iteration = 0; iteration < 3; iteration++) {
        int before = __atomic_load_n(&previousCalls, __ATOMIC_RELAXED);
        WineHuaStallSampleAllThreads();
        assert(g_stallPrevSa.sa_sigaction == PreviousHandler);
        assert(__atomic_load_n(&previousCalls, __ATOMIC_RELAXED) > before);
        assert(g_stallSampleCount >= 5 && g_stallSampleCount <= WINEHUA_STALL_MAX_SAMPLES);
        int count = __atomic_load_n(&g_stallSampleCount, __ATOMIC_RELAXED);
        for (int i = 0; i < count; i++) assert(__atomic_load_n(&g_stallSamples[i].ready, __ATOMIC_ACQUIRE));
    }
    int before = __atomic_load_n(&previousCalls, __ATOMIC_RELAXED);
    raise(SIGPROF);
    assert(__atomic_load_n(&previousCalls, __ATOMIC_RELAXED) == before + 1);
    __atomic_store_n(&stopping, 1, __ATOMIC_RELAXED);
    for (auto thread : threads) assert(pthread_join(thread, nullptr) == 0);
    puts("PASS: 3 rounds, concurrent threads, original handler retained and chained outside sampling");
}
'''
build = root / 'build/direct-stall-host-regression'
build.mkdir(parents=True, exist_ok=True)
(build / 'sampler.cpp').write_text(header + src[begin:end] + main)
subprocess.run(['g++', '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', str(build / 'sampler.cpp'), '-o', str(build / 'sampler')], check=True)
r = subprocess.run([str(build / 'sampler')], capture_output=True, text=True, timeout=15)
(build / 'result.json').write_text(json.dumps({'exit': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr, 'source': 'Exact sampler functions extracted from production wine_child.cpp; Linux x86_64 host signals, not OHOS/FEX.'}, indent=2) + '\n')
print(r.stdout.strip())
raise SystemExit(r.returncode)

/**
 * child_demo.cpp - 子进程入口 (libchild_demo.so)
 *
 * 通过 OH_Ability_StartNativeChildProcess 启动，入口函数为 Main。
 * 参考 .temp/native-process 验证示例。
 */
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <unistd.h>
#include <cstdio>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "CHILD-DEMO"

extern "C" void Main(NativeChildProcess_Args args)
{
    pid_t pid = getpid();
    OH_LOG_INFO(LOG_APP, "[CHILD-DEMO] Main() ENTER pid=%{public}d entryParams=%{public}s",
                pid, args.entryParams ? args.entryParams : "(null)");

    FILE* f = fopen("/data/storage/el2/base/files/child_demo.log", "w");
    if (f) {
        fprintf(f, "CHILD-DEMO OK pid=%d entryParams=%s\n", pid,
                args.entryParams ? args.entryParams : "(null)");
        int n = 0;
        for (auto* node = args.fdList.head; node; node = node->next, n++)
            fprintf(f, "  fd[%d] name=%s fd=%d\n", n,
                    node->fdName ? node->fdName : "(null)", node->fd);
        fclose(f);
    }

    OH_LOG_INFO(LOG_APP, "[CHILD-DEMO] Main() EXIT");
}

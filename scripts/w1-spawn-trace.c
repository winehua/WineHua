/*
 * w1-spawn-trace.c — W1 调试用：拦截 posix_spawnp / execvp，打印真正要执行的程序。
 *
 * 背景：Wine 的 tools/tools.h:strarray_spawn() 用 posix_spawnp，失败时只返回 -1，
 * 而 tools/winebuild/utils.c 的 fatal_perror("winebuild") 打出来的 "winebuild"
 * 是固定文案、errno 也可能是陈旧的 —— 看不出到底缺哪个程序。用它来看。
 *
 * 用法：
 *   gcc -shared -fPIC -o /tmp/spawntrace.so scripts/w1-spawn-trace.c -ldl
 *   LD_PRELOAD=/tmp/spawntrace.so <原来的命令>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <spawn.h>
#include <dlfcn.h>
#include <unistd.h>

typedef int (*spawnp_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                         const posix_spawnattr_t *, char *const[], char *const[]);
typedef int (*execvp_fn)(const char *, char *const[]);

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *sa, char *const argv[], char *const envp[])
{
    static spawnp_fn real;
    int rc;

    if (!real) real = (spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    rc = real(pid, file, fa, sa, argv, envp);
    fprintf(stderr, "[spawntrace] posix_spawnp(\"%s\") -> %d%s\n",
            file ? file : "(null)", rc, rc ? "   <== FAILED" : "");
    return rc;
}

int execvp(const char *file, char *const argv[])
{
    static execvp_fn real;
    int rc;

    if (!real) real = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    fprintf(stderr, "[spawntrace] execvp(\"%s\")\n", file ? file : "(null)");
    rc = real(file, argv);
    return rc;
}

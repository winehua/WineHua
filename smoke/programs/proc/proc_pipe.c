/* winehua_t_proc_pipe — 管道（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.7。
 * 失败特征：传输错/阻塞 = fd 继承/IPC 断。
 * 匿名管道：父写 64KB → 子进程读校验（退出码判结果）。
 * 命名管道：同进程双线程（server/client）双向各 4KB。
 * 协议：--t-child <readEnd句柄值> 子模式。
 */
#include "../common/winehua_t_check.h"
#include <process.h>

#define PAYLOAD_SIZE (64 * 1024)

static DWORD WINAPI named_server(LPVOID arg)
{
    char name_pipe[] = "\\\\.\\pipe\\winehua_t_pipe_srv";
    char buffer[4096];
    DWORD written = 0, read = 0, total = 0;
    HANDLE pipe = CreateNamedPipeA(name_pipe, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 4096, 4096, 0, NULL);
    (void)arg;
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        CloseHandle(pipe);
        return 2;
    }
    /* 双向：先读客户端 4KB，再回写 4KB */
    while (total < 4096)
    {
        if (!ReadFile(pipe, buffer + total, 4096 - total, &read, NULL) || !read)
            break;
        total += read;
    }
    if (total != 4096)
    {
        CloseHandle(pipe);
        return 3;
    }
    for (read = 0; read < 4096; read++)
        buffer[read] = (unsigned char)(buffer[read] ^ 0x5A);
    if (!WriteFile(pipe, buffer, 4096, &written, NULL) || written != 4096)
    {
        CloseHandle(pipe);
        return 4;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

int main(int argc, char **argv)
{
    t_begin("winehua_t_proc_pipe", argc, argv);

    /* 子模式：读匿名管道，校验图案，退出码 0/5 */
    if (argc > 2 && !lstrcmpA(argv[1], "--t-child"))
    {
        HANDLE read_end = (HANDLE)(UINT_PTR)atoi(argv[2]);
        unsigned char *buffer = (unsigned char *)malloc(PAYLOAD_SIZE);
        DWORD read = 0, total = 0;
        int i, ok;
        if (!buffer)
            return 6;
        while (total < PAYLOAD_SIZE)
        {
            if (!ReadFile(read_end, buffer + total, PAYLOAD_SIZE - total, &read, NULL) || !read)
                break;
            total += read;
        }
        ok = total == PAYLOAD_SIZE;
        for (i = 0; ok && i < PAYLOAD_SIZE; ++i)
            if (buffer[i] != (unsigned char)(i * 31 + 7))
                ok = 0;
        free(buffer);
        CloseHandle(read_end);
        return ok ? 0 : 5;
    }

    /* 匿名管道：父→子 64KB */
    {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
        HANDLE read_end = NULL, write_end = NULL;
        HANDLE child = NULL;
        unsigned char *buffer;
        DWORD written = 0;
        DWORD exit_code = 99;
        char cmdline[MAX_PATH + 48];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        int i;

        t_check("anon-create", CreatePipe(&read_end, &write_end, &sa, 0),
                "err=%lu", GetLastError());
        buffer = (unsigned char *)malloc(PAYLOAD_SIZE);
        if (buffer)
        {
            for (i = 0; i < PAYLOAD_SIZE; ++i)
                buffer[i] = (unsigned char)(i * 31 + 7);
        }
        /* 子进程继承句柄：用命令行传句柄值 */
        {
            char self[MAX_PATH];
            GetModuleFileNameA(NULL, self, sizeof(self));
            snprintf(cmdline, sizeof(cmdline), "\"%s\" --t-child %lu",
                     self, (unsigned long)(UINT_PTR)read_end);
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            memset(&pi, 0, sizeof(pi));
            t_check("anon-spawn",
                    CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi),
                    "err=%lu", GetLastError());
            if (pi.hProcess)
            {
                CloseHandle(pi.hThread);
                /* 写满 64KB（管道缓冲有限，子进程边读边收） */
                {
                    DWORD off = 0;
                    while (off < PAYLOAD_SIZE)
                    {
                        if (!WriteFile(write_end, buffer + off, PAYLOAD_SIZE - off, &written, NULL))
                            break;
                        off += written;
                    }
                    t_check("anon-write-all", off == PAYLOAD_SIZE,
                            "wrote %lu of %d", off, PAYLOAD_SIZE);
                }
                CloseHandle(write_end); /* 关写端 = 子进程读到 EOF */
                WaitForSingleObject(pi.hProcess, 15000);
                GetExitCodeProcess(pi.hProcess, &exit_code);
                t_check("anon-child-verify", exit_code == 0,
                        "child exit=%lu (0=ok 5=mismatch)", exit_code);
                CloseHandle(pi.hProcess);
            }
            free(buffer);
        }
        CloseHandle(read_end);
    }

    /* 命名管道：双线程双向 4KB */
    {
        HANDLE thread;
        HANDLE client;
        char name_pipe[] = "\\\\.\\pipe\\winehua_t_pipe_srv";
        unsigned char buffer[4096];
        DWORD written = 0, read = 0, total = 0;
        int i, ok = 1;
        DWORD wait_rc;

        thread = (HANDLE)_beginthreadex(NULL, 0, named_server, NULL, 0, NULL);
        t_check("named-server-thread", thread != NULL, "beginthreadex");
        Sleep(100); /* 等 server 监听 */

        client = CreateFileA(name_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, 0, NULL);
        t_check("named-connect", client != INVALID_HANDLE_VALUE,
                "err=%lu", client == INVALID_HANDLE_VALUE ? GetLastError() : 0);
        if (client != INVALID_HANDLE_VALUE)
        {
            for (i = 0; i < 4096; ++i)
                buffer[i] = (unsigned char)(i * 13 + 1);
            t_check("named-write", WriteFile(client, buffer, 4096, &written, NULL) &&
                                   written == 4096, "written=%lu", written);
            total = 0;
            while (total < 4096)
            {
                if (!ReadFile(client, buffer + total, 4096 - total, &read, NULL) || !read)
                    break;
                total += read;
            }
            t_check("named-read-all", total == 4096, "read %lu", total);
            for (i = 0; i < 4096; ++i)
                if (buffer[i] != (unsigned char)((unsigned char)(i * 13 + 1) ^ 0x5A))
                {
                    ok = 0;
                    break;
                }
            t_check("named-roundtrip", ok, "XOR pattern mismatch at %d", i);
            CloseHandle(client);
        }
        wait_rc = WaitForSingleObject(thread, 5000);
        t_check("named-server-clean", wait_rc == WAIT_OBJECT_0, "wait rc=%lu", wait_rc);
        CloseHandle(thread);
    }
    return t_finish();
}

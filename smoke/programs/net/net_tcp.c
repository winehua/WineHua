/* winehua_t_net_tcp — 回环 TCP（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.13。
 * 失败特征：connect 拒绝 = socket 链断；传输错 = ws2_32 层断。
 * 同进程 listener 线程 + 127.0.0.1 自连 echo，不依赖外网。
 */
#include "../common/winehua_t_check.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>

#define PAYLOAD 4096
#define PORT 0 /* 系统分配 */

static char g_payload[PAYLOAD];

static DWORD WINAPI echo_server(LPVOID arg)
{
    SOCKET listener, accepted;
    struct sockaddr_in addr;
    int len = sizeof(addr);
    char buffer[1024];
    int ret;
    (void)arg;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return 1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    ret = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    if (ret)
    {
        closesocket(listener);
        return 2;
    }
    if (listen(listener, 1))
    {
        closesocket(listener);
        return 3;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &len))
    {
        closesocket(listener);
        return 4;
    }
    *(volatile int *)arg = (int)ntohs(addr.sin_port); /* 交给主线程 */

    accepted = accept(listener, NULL, NULL);
    if (accepted == INVALID_SOCKET)
    {
        closesocket(listener);
        return 5;
    }
    /* echo 到对端关闭 */
    for (;;)
    {
        ret = recv(accepted, buffer, sizeof(buffer), 0);
        if (ret <= 0)
            break;
        send(accepted, buffer, ret, 0);
    }
    closesocket(accepted);
    closesocket(listener);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    HANDLE thread;
    volatile int server_port = 0;
    SOCKET sock;
    struct sockaddr_in addr;
    int i, ret, ok;
    char buffer[PAYLOAD];
    DWORD wait_rc;

    t_begin("winehua_t_net_tcp", argc, argv);

    ret = WSAStartup(MAKEWORD(2, 2), &wsa);
    t_check("wsastartup", ret == 0, "rc=%d wsaerr=%d", ret, WSAGetLastError());
    if (ret)
        return t_finish();

    for (i = 0; i < PAYLOAD; ++i)
        g_payload[i] = (char)(i * 37 + 11);

    thread = CreateThread(NULL, 0, echo_server, (void *)&server_port, 0, NULL);
    t_check("spawn-server", thread != NULL, "err");
    /* 等 server 分配端口 */
    for (i = 0; i < 100 && !server_port; ++i)
        Sleep(10);
    t_check("server-port", server_port > 0, "port=%d", server_port);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    t_check("socket", sock != INVALID_SOCKET, "err=%d", WSAGetLastError());
    if (sock != INVALID_SOCKET && server_port > 0)
    {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((unsigned short)server_port);
        ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        t_check("connect", ret == 0, "rc=%d err=%d", ret, WSAGetLastError());
        if (ret == 0)
        {
            ret = send(sock, g_payload, PAYLOAD, 0);
            t_check("send-4k", ret == PAYLOAD, "sent %d", ret);
            ok = 1;
            i = 0;
            while (i < PAYLOAD)
            {
                ret = recv(sock, buffer + i, PAYLOAD - i, 0);
                if (ret <= 0)
                    break;
                i += ret;
            }
            t_check("recv-4k", i == PAYLOAD, "recv %d", i);
            for (ret = 0; ret < PAYLOAD; ++ret)
                if (buffer[ret] != g_payload[ret])
                {
                    ok = 0;
                    break;
                }
            t_check("echo-bytes", ok, "mismatch at %d", ret);
            closesocket(sock);
        }
    }
    /* 关闭客户端后 server 的 recv 返回 0，线程自然退出 */
    wait_rc = WaitForSingleObject(thread, 5000);
    t_check("server-clean-exit", wait_rc == WAIT_OBJECT_0, "wait rc=%lu", wait_rc);
    CloseHandle(thread);
    WSACleanup();
    return t_finish();
}

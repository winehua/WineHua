/* winehua_t_net_https — HTTPS 协议链（P6，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.13。
 * 失败分层：DNS/TCP 80 预检不通=环境无网（UNSUPPORTED，合法答案，不算
 * 失败）；预检通而 TLS 挂（ERROR_INTERNET_SECURITY_CHANNEL_ERROR /
 * 证书类错误）=schannel→gnutls 链断（wine-https 回归哨兵）；HTTP 层错=
 * wininet 语义断。
 * 目标=baidu.com（高稳定服务，可复现）；超时 15s。
 */
#include "../common/winehua_t_check.h"
#include <wininet.h>
#include <winsock2.h>

#define HOST "www.baidu.com"
#define TIMEOUT_MS 15000

static int tcp_probe(void)
{
    WSADATA wsa;
    struct hostent *he;
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;
    int ok = 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    he = gethostbyname(HOST);
    if (!he || he->h_addr_list[0] == NULL)
        goto done; /* DNS 断 = 环境无网/解析链断 */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        goto done;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    ok = connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0;
done:
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    return ok;
}

int main(int argc, char **argv)
{
    HINTERNET inet = NULL, conn = NULL, req = NULL;
    char buf[512];
    DWORD bytes = 0, err, idx = 0, code = 0, code_len = sizeof(code);
    int net_up;

    t_begin("winehua_t_net_https", argc, argv);

    /* 分层第 1 步：DNS+TCP 80 预检。不通 = 环境无网（UNSUPPORTED） */
    net_up = tcp_probe();
    if (!net_up)
    {
        t_check("https-protocol-chain", 1,
                "UNSUPPORTED: DNS/TCP 80 预检不通（环境无网），无判据");
        t_metric("network-up", "0");
        return t_finish();
    }
    t_metric("network-up", "1");
    t_check("tcp-probe", 1, "dns+tcp80 ok");

    inet = InternetOpenA("winehua-t-net-https", INTERNET_OPEN_TYPE_PRECONFIG,
                         NULL, NULL, 0);
    t_check("internet-open", inet != NULL, "err=%lu", GetLastError());
    if (!inet)
        return t_finish();
    InternetSetOptionA(inet, INTERNET_OPTION_CONNECT_TIMEOUT,
                       &(DWORD){TIMEOUT_MS}, sizeof(DWORD));
    InternetSetOptionA(inet, INTERNET_OPTION_SEND_TIMEOUT,
                       &(DWORD){TIMEOUT_MS}, sizeof(DWORD));
    InternetSetOptionA(inet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &(DWORD){TIMEOUT_MS}, sizeof(DWORD));

    conn = InternetConnectA(inet, HOST, INTERNET_DEFAULT_HTTPS_PORT,
                            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    t_check("internet-connect", conn != NULL, "err=%lu", GetLastError());
    if (!conn)
        goto done;

    req = HttpOpenRequestA(conn, "GET", "/", HTTP_VERSION, NULL, NULL,
                           INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE
                           | INTERNET_FLAG_NO_UI, 0);
    t_check("http-open-request", req != NULL, "err=%lu", GetLastError());
    if (!req)
        goto done;

    SetLastError(0);
    if (!HttpSendRequestA(req, NULL, 0, NULL, 0))
    {
        err = GetLastError();
        /* 分层第 2 步：TLS 层 vs 其它 */
        t_check("http-send-tls", err != ERROR_INTERNET_SECURITY_CHANNEL_ERROR
                                 && err != ERROR_INTERNET_INVALID_CA
                                 && err != ERROR_INTERNET_SEC_CERT_ERRORS,
                "TLS 层断 err=%lu (schannel→gnutls 链回归哨兵)",
                (unsigned long)err);
        t_check("https-protocol-chain", 0, "send failed err=%lu (%s)",
                (unsigned long)err,
                err == ERROR_INTERNET_NAME_NOT_RESOLVED ? "DNS"
                : err == ERROR_INTERNET_CANNOT_CONNECT ? "TCP"
                : err == ERROR_INTERNET_TIMEOUT ? "TIMEOUT" : "OTHER");
        goto done;
    }
    t_check("http-send-tls", 1, "TLS 握手+请求发送成功");

    if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &code, &code_len, &idx))
        t_check("http-status-2xx", code >= 200 && code < 300,
                "status=%lu", (unsigned long)code);
    else
        t_check("http-status-2xx", 0, "query err=%lu", GetLastError());

    if (InternetReadFile(req, buf, sizeof(buf) - 1, &bytes) && bytes > 0)
    {
        buf[bytes < sizeof(buf) - 1 ? bytes : sizeof(buf) - 1] = 0;
        t_check("http-body-read", bytes > 0, "first=%lu bytes",
                (unsigned long)bytes);
    }
    else
        t_check("http-body-read", 0, "read err=%lu bytes=%lu",
                GetLastError(), (unsigned long)bytes);
    t_check("https-protocol-chain", 1, "HTTPS 全链通");
    t_metric("https-status", "%lu", (unsigned long)code);

done:
    if (req) InternetCloseHandle(req);
    if (conn) InternetCloseHandle(conn);
    if (inet) InternetCloseHandle(inet);
    return t_finish();
}

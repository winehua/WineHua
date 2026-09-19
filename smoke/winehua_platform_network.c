/* Steam platform network gate: adapters, DNS, TCP, TLS, WinHTTP, and WinInet. */

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windns.h>
#include <winhttp.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#include "../thirdparty/wine/programs/winehua_smoke_protocol.h"

#define STORE_HOST_A "store.steampowered.com"
#define STORE_HOST_W L"store.steampowered.com"
#define UPDATE_HOST_A "client-update.steamstatic.com"
#define UPDATE_HOST_W L"client-update.steamstatic.com"
#define UPDATE_PATH_W L"/steam_client_win32"
#define UPDATE_URL_W L"https://client-update.steamstatic.com/steam_client_win32"
#define SELF_SIGNED_HOST_W L"self-signed.badssl.com"
#define WRONG_HOST_W L"wrong.host.badssl.com"
#define MAX_HTTP_BODY (16 * 1024 * 1024)

enum http_stage
{
    HTTP_STAGE_NONE,
    HTTP_STAGE_SESSION,
    HTTP_STAGE_CONNECTION,
    HTTP_STAGE_REQUEST,
    HTTP_STAGE_SENT,
    HTTP_STAGE_RESPONSE,
    HTTP_STAGE_HEADERS,
    HTTP_STAGE_BODY,
    HTTP_STAGE_COMPLETE
};

struct adapter_probe
{
    ULONG flags;
    ULONG size_result;
    ULONG query_result;
    ULONG adapter_count;
    ULONG ipv4_count;
};

struct http_probe
{
    DWORD stage;
    DWORD error;
    DWORD status;
    DWORD bytes;
    DWORD cert_query_error;
    DWORD cert_chain_error;
};

struct wininet_probe
{
    DWORD stage;
    DWORD error;
    DWORD status;
    DWORD bytes;
};

BOOL winehua_probe_wininet_update(DWORD *stage, DWORD *error, DWORD *status, DWORD *bytes);

static BOOL probe_adapters(struct adapter_probe *probe)
{
    IP_ADAPTER_ADDRESSES *buffer, *adapter;
    ULONG size = 0;

    probe->size_result = GetAdaptersAddresses(AF_UNSPEC, probe->flags, NULL, NULL, &size);
    if (probe->size_result != ERROR_BUFFER_OVERFLOW || !size) return FALSE;

    buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!buffer) return FALSE;
    probe->query_result = GetAdaptersAddresses(AF_UNSPEC, probe->flags, NULL, buffer, &size);
    if (!probe->query_result)
    {
        for (adapter = buffer; adapter; adapter = adapter->Next)
        {
            IP_ADAPTER_UNICAST_ADDRESS *address;
            ++probe->adapter_count;
            for (address = adapter->FirstUnicastAddress; address; address = address->Next)
                if (address->Address.lpSockaddr && address->Address.lpSockaddr->sa_family == AF_INET)
                    ++probe->ipv4_count;
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return probe->query_result == ERROR_SUCCESS && probe->adapter_count > 0;
}

static DWORD probe_dns(const char *host, char *ip, size_t ip_size)
{
    DNS_RECORD *records = NULL, *record;
    DNS_STATUS status = DnsQuery_A(host, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &records, NULL);

    ip[0] = 0;
    if (!status)
    {
        for (record = records; record; record = record->pNext)
        {
            if (record->wType == DNS_TYPE_A)
            {
                const BYTE *bytes = (const BYTE *)&record->Data.A.IpAddress;
                snprintf(ip, ip_size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
                break;
            }
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    return status;
}

static DWORD probe_tcp(const char *host)
{
    ADDRINFOA hints, *addresses = NULL, *address;
    DWORD result = WSAHOST_NOT_FOUND;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ret = GetAddrInfoA(host, "443", &hints, &addresses);
    if (ret) return (DWORD)ret;

    for (address = addresses; address; address = address->ai_next)
    {
        SOCKET socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        u_long nonblocking = 1;
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);
        fd_set write_set, error_set;
        struct timeval timeout;

        if (socket_handle == INVALID_SOCKET)
        {
            result = WSAGetLastError();
            continue;
        }
        ioctlsocket(socket_handle, FIONBIO, &nonblocking);
        ret = connect(socket_handle, address->ai_addr, (int)address->ai_addrlen);
        if (!ret)
        {
            closesocket(socket_handle);
            result = ERROR_SUCCESS;
            break;
        }
        result = WSAGetLastError();
        if (result == WSAEWOULDBLOCK || result == WSAEINPROGRESS)
        {
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(socket_handle, &write_set);
            FD_SET(socket_handle, &error_set);
            timeout.tv_sec = 10;
            timeout.tv_usec = 0;
            ret = select(0, NULL, &write_set, &error_set, &timeout);
            if (ret > 0 && !getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                                      (char *)&socket_error, &socket_error_size))
                result = socket_error;
            else if (!ret)
                result = WSAETIMEDOUT;
            else if (ret < 0)
                result = WSAGetLastError();
        }
        closesocket(socket_handle);
        if (!result) break;
    }
    FreeAddrInfoA(addresses);
    return result;
}

static DWORD verify_certificate_chain(PCCERT_CONTEXT certificate, const WCHAR *host)
{
    CERT_CHAIN_PARA chain_parameters;
    CERT_CHAIN_POLICY_PARA policy_parameters;
    CERT_CHAIN_POLICY_STATUS policy_status;
    HTTPSPolicyCallbackData ssl_parameters;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    DWORD result;

    memset(&chain_parameters, 0, sizeof(chain_parameters));
    chain_parameters.cbSize = sizeof(chain_parameters);
    if (!CertGetCertificateChain(NULL, certificate, NULL, certificate->hCertStore,
                                 &chain_parameters, 0, NULL, &chain))
        return GetLastError();

    memset(&ssl_parameters, 0, sizeof(ssl_parameters));
    ssl_parameters.cbStruct = sizeof(ssl_parameters);
    ssl_parameters.dwAuthType = AUTHTYPE_SERVER;
    ssl_parameters.pwszServerName = (WCHAR *)host;
    memset(&policy_parameters, 0, sizeof(policy_parameters));
    policy_parameters.cbSize = sizeof(policy_parameters);
    policy_parameters.pvExtraPolicyPara = &ssl_parameters;
    memset(&policy_status, 0, sizeof(policy_status));
    policy_status.cbSize = sizeof(policy_status);

    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                          &policy_parameters, &policy_status))
        result = GetLastError();
    else
        result = policy_status.dwError;
    CertFreeCertificateChain(chain);
    return result;
}

static BOOL probe_winhttp(const WCHAR *host, const WCHAR *path, struct http_probe *probe)
{
    HINTERNET session = NULL, connection = NULL, request = NULL;
    PCCERT_CONTEXT certificate = NULL;
    DWORD size, read;
    BYTE buffer[4096];
    BOOL ok = FALSE;

    memset(probe, 0, sizeof(*probe));
    session = WinHttpOpen(L"WineHua Steam network gate/2.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto failed;
    probe->stage = HTTP_STAGE_SESSION;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 20000);

    connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) goto failed;
    probe->stage = HTTP_STAGE_CONNECTION;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) goto failed;
    probe->stage = HTTP_STAGE_REQUEST;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto failed;
    probe->stage = HTTP_STAGE_SENT;
    if (!WinHttpReceiveResponse(request, NULL)) goto failed;
    probe->stage = HTTP_STAGE_RESPONSE;

    size = sizeof(probe->status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &probe->status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        goto failed;
    probe->stage = HTTP_STAGE_HEADERS;

    size = sizeof(certificate);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size))
        probe->cert_query_error = GetLastError();
    else
    {
        probe->cert_chain_error = verify_certificate_chain(certificate, host);
        CertFreeCertificateContext(certificate);
        certificate = NULL;
    }

    for (;;)
    {
        read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) goto failed;
        if (!read) break;
        if (probe->bytes > MAX_HTTP_BODY - read)
        {
            SetLastError(ERROR_FILE_TOO_LARGE);
            goto failed;
        }
        probe->bytes += read;
        probe->stage = HTTP_STAGE_BODY;
    }
    probe->stage = HTTP_STAGE_COMPLETE;
    ok = probe->status >= 200 && probe->status < 400 && probe->bytes > 0 &&
         !probe->cert_query_error && !probe->cert_chain_error;
    goto done;

failed:
    probe->error = GetLastError();
done:
    if (certificate) CertFreeCertificateContext(certificate);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

static SECURITY_STATUS probe_schannel(void)
{
    SCHANNEL_CRED credentials;
    CredHandle handle;
    TimeStamp expiry;
    SECURITY_STATUS status;

    memset(&credentials, 0, sizeof(credentials));
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    status = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
                                       NULL, &credentials, NULL, NULL, &handle, &expiry);
    if (status == SEC_E_OK) FreeCredentialsHandle(&handle);
    return status;
}

static DWORD count_root_certificates(DWORD *error)
{
    HCERTSTORE store;
    PCCERT_CONTEXT certificate = NULL;
    DWORD count = 0, enumeration_error;

    *error = ERROR_SUCCESS;
    store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store)
    {
        *error = GetLastError();
        return 0;
    }
    while ((certificate = CertEnumCertificatesInStore(store, certificate))) ++count;
    enumeration_error = GetLastError();
    if (enumeration_error != (DWORD)CRYPT_E_NOT_FOUND) *error = enumeration_error;
    CertCloseStore(store, 0);
    return count;
}

int main(int argc, char **argv)
{
    static const ULONG flags[] = {0x00, 0x0f, 0x27, 0x2e};
    struct winehua_smoke_options options;
    struct adapter_probe adapters[4];
    struct http_probe store_http, update_http, self_signed_http, wrong_host_http;
    struct wininet_probe update_wininet;
    WSADATA winsock_data;
    SECURITY_STATUS schannel_status;
    char metrics[4096], message[512], store_ip[32], update_ip[32];
    DWORD store_dns, update_dns, tcp_error, root_error, root_count;
    BOOL ok = TRUE, store_ok, update_ok, wininet_ok;
    int winsock_status;
    unsigned int i;

    if (!winehua_smoke_parse_options(&options, argc, argv, 15)) return 2;
    winsock_status = WSAStartup(MAKEWORD(2, 2), &winsock_data);
    if (winsock_status) ok = FALSE;

    memset(adapters, 0, sizeof(adapters));
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
    {
        adapters[i].flags = flags[i];
        if (!probe_adapters(&adapters[i])) ok = FALSE;
    }

    store_dns = probe_dns(STORE_HOST_A, store_ip, sizeof(store_ip));
    update_dns = probe_dns(UPDATE_HOST_A, update_ip, sizeof(update_ip));
    tcp_error = winsock_status ? (DWORD)winsock_status : probe_tcp(UPDATE_HOST_A);
    store_ok = probe_winhttp(STORE_HOST_W, L"/", &store_http);
    update_ok = probe_winhttp(UPDATE_HOST_W, UPDATE_PATH_W, &update_http);
    wininet_ok = winehua_probe_wininet_update(&update_wininet.stage, &update_wininet.error,
                                              &update_wininet.status, &update_wininet.bytes);
    probe_winhttp(SELF_SIGNED_HOST_W, L"/", &self_signed_http);
    probe_winhttp(WRONG_HOST_W, L"/", &wrong_host_http);
    schannel_status = probe_schannel();
    root_count = count_root_certificates(&root_error);

    if (store_dns || !store_ip[0] || update_dns || !update_ip[0] || tcp_error ||
        !store_ok || !update_ok || !wininet_ok || schannel_status != SEC_E_OK ||
        root_error || !root_count ||
        self_signed_http.error != ERROR_WINHTTP_SECURE_FAILURE ||
        wrong_host_http.error != ERROR_WINHTTP_SECURE_FAILURE)
        ok = FALSE;

    snprintf(metrics, sizeof(metrics),
             "{\"gaa\":["
             "{\"flags\":0,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":15,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":39,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu},"
             "{\"flags\":46,\"size\":%lu,\"query\":%lu,\"adapters\":%lu,\"ipv4\":%lu}],"
             "\"storeDnsError\":%lu,\"storeDnsIp\":\"%s\","
             "\"updateDnsError\":%lu,\"updateDnsIp\":\"%s\",\"updateTcpError\":%lu,"
             "\"storeWinHttp\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu,\"certQueryError\":%lu,\"certChainError\":%lu},"
             "\"updateWinHttp\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu,\"certQueryError\":%lu,\"certChainError\":%lu},"
             "\"updateWinInet\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu,\"bytes\":%lu},"
             "\"schannelStatus\":%ld,\"rootCertificates\":%lu,\"rootError\":%lu,"
             "\"selfSigned\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu},"
             "\"wrongHost\":{\"stage\":%lu,\"error\":%lu,\"status\":%lu}}",
             adapters[0].size_result, adapters[0].query_result, adapters[0].adapter_count, adapters[0].ipv4_count,
             adapters[1].size_result, adapters[1].query_result, adapters[1].adapter_count, adapters[1].ipv4_count,
             adapters[2].size_result, adapters[2].query_result, adapters[2].adapter_count, adapters[2].ipv4_count,
             adapters[3].size_result, adapters[3].query_result, adapters[3].adapter_count, adapters[3].ipv4_count,
             store_dns, store_ip, update_dns, update_ip, tcp_error,
             store_http.stage, store_http.error, store_http.status, store_http.bytes,
             store_http.cert_query_error, store_http.cert_chain_error,
             update_http.stage, update_http.error, update_http.status, update_http.bytes,
             update_http.cert_query_error, update_http.cert_chain_error,
             update_wininet.stage, update_wininet.error, update_wininet.status, update_wininet.bytes,
             (long)schannel_status, root_count, root_error,
             self_signed_http.stage, self_signed_http.error, self_signed_http.status,
             wrong_host_http.stage, wrong_host_http.error, wrong_host_http.status);
    snprintf(message, sizeof(message),
             "Steam update DNS %s rc=%lu TCP=%lu; WinHTTP stage=%lu err=%lu status=%lu bytes=%lu cert=%lu; WinInet stage=%lu err=%lu status=%lu bytes=%lu",
             update_ip[0] ? update_ip : "failed", update_dns, tcp_error,
             update_http.stage, update_http.error, update_http.status, update_http.bytes,
             update_http.cert_chain_error, update_wininet.stage, update_wininet.error,
             update_wininet.status, update_wininet.bytes);

    if (!winsock_status) WSACleanup();
    if (!winehua_smoke_write_result(&options, ok ? "PASS" : "FAIL",
                                    ok ? "complete" : "platform-network",
                                    message, metrics))
        return 3;
    return ok ? 0 : 1;
}

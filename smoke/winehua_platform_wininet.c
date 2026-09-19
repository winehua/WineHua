/* Kept separate because MinGW's winhttp.h and wininet.h conflict. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#define UPDATE_URL_W L"https://client-update.steamstatic.com/steam_client_win32"
#define MAX_HTTP_BODY (16 * 1024 * 1024)
#define HTTP_STAGE_SESSION 1
#define HTTP_STAGE_RESPONSE 5
#define HTTP_STAGE_HEADERS 6
#define HTTP_STAGE_BODY 7
#define HTTP_STAGE_COMPLETE 8

BOOL winehua_probe_wininet_update(DWORD *stage, DWORD *error, DWORD *status, DWORD *bytes)
{
    HINTERNET session = NULL, request = NULL;
    BYTE buffer[4096];
    DWORD size, read;
    BOOL ok = FALSE;

    *stage = *error = *status = *bytes = 0;
    session = InternetOpenW(L"WineHua Steam network gate/2.0", INTERNET_OPEN_TYPE_PRECONFIG,
                            NULL, NULL, 0);
    if (!session) goto failed;
    *stage = HTTP_STAGE_SESSION;
    request = InternetOpenUrlW(session, UPDATE_URL_W, NULL, 0,
                               INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                               INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!request) goto failed;
    *stage = HTTP_STAGE_RESPONSE;
    size = sizeof(*status);
    if (!HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        status, &size, NULL))
        goto failed;
    *stage = HTTP_STAGE_HEADERS;

    for (;;)
    {
        read = 0;
        if (!InternetReadFile(request, buffer, sizeof(buffer), &read)) goto failed;
        if (!read) break;
        if (*bytes > MAX_HTTP_BODY - read)
        {
            SetLastError(ERROR_FILE_TOO_LARGE);
            goto failed;
        }
        *bytes += read;
        *stage = HTTP_STAGE_BODY;
    }
    *stage = HTTP_STAGE_COMPLETE;
    ok = *status >= 200 && *status < 400 && *bytes > 0;
    goto done;

failed:
    *error = GetLastError();
done:
    if (request) InternetCloseHandle(request);
    if (session) InternetCloseHandle(session);
    return ok;
}

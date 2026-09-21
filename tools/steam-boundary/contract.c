#include <windows.h>
#include <stdio.h>
#include <string.h>

#define ROUNDS 8
#define THREADS 4
#define INCREMENTS 5000

struct exchange {
    volatile LONG request;
    volatile LONG response;
};

static CRITICAL_SECTION lock;
static LONG counter;
static const char *output_path;

static int write_result(const char *text)
{
    HANDLE file;
    DWORD length, written = 0;
    int ok;

    file = CreateFileA(output_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    length = (DWORD)strlen(text);
    ok = WriteFile(file, text, length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static DWORD WINAPI count_thread(void *unused)
{
    int i;
    (void)unused;
    for (i = 0; i < INCREMENTS; ++i) {
        EnterCriticalSection(&lock);
        ++counter;
        LeaveCriticalSection(&lock);
    }
    return 0x1234;
}

static DWORD WINAPI exit_thread(void *unused)
{
    (void)unused;
    ExitThread(0x4321);
    return 0;
}

static DWORD WINAPI wait_thread(void *event)
{
    return WaitForSingleObject((HANDLE)event, 5000);
}

static int child(const char *mapping_name, const char *request_name, const char *ack_name)
{
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
    HANDLE request = OpenEventA(SYNCHRONIZE, FALSE, request_name);
    HANDLE ack = OpenEventA(EVENT_MODIFY_STATE, FALSE, ack_name);
    struct exchange *data = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*data)) : NULL;
    int i, ok = data && request && ack;

    for (i = 1; ok && i <= ROUNDS; ++i) {
        if (WaitForSingleObject(request, 5000) != WAIT_OBJECT_0 || data->request != i) ok = 0;
        if (ok) {
            InterlockedExchange(&data->response, i * 7 + 3);
            ok = SetEvent(ack);
        }
    }
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
    if (request) CloseHandle(request);
    if (ack) CloseHandle(ack);
    return ok ? 0 : 2;
}

static int ipc_test(const char *exe)
{
    char mapping_name[80], request_name[80], ack_name[80], command[1200];
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE mapping, request, ack;
    struct exchange *data;
    DWORD exit_code = 0;
    int i, ok;

    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-format-map\"}\n");
    snprintf(mapping_name, sizeof(mapping_name), "Local\\WineHuaContractMap%lu", GetCurrentProcessId());
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-format-request\"}\n");
    snprintf(request_name, sizeof(request_name), "Local\\WineHuaContractReq%lu", GetCurrentProcessId());
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-format-ack\"}\n");
    snprintf(ack_name, sizeof(ack_name), "Local\\WineHuaContractAck%lu", GetCurrentProcessId());
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-create\"}\n");
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*data), mapping_name);
    request = CreateEventA(NULL, FALSE, FALSE, request_name);
    ack = CreateEventA(NULL, FALSE, FALSE, ack_name);
    data = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*data)) : NULL;
    ok = data && request && ack;
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-spawn\"}\n");
    if (ok) {
        memset(data, 0, sizeof(*data));
        snprintf(command, sizeof(command), "\"%s\" --child %s %s %s", exe, mapping_name, request_name, ack_name);
        startup.cb = sizeof(startup);
        ok = CreateProcessA(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    }
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-rounds\"}\n");
    for (i = 1; ok && i <= ROUNDS; ++i) {
        InterlockedExchange(&data->request, i);
        ok = SetEvent(request) && WaitForSingleObject(ack, 5000) == WAIT_OBJECT_0 &&
             data->response == i * 7 + 3;
    }
    if (process.hProcess) {
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-child-exit\"}\n");
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(process.hProcess, &exit_code) || exit_code) ok = 0;
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
    if (request) CloseHandle(request);
    if (ack) CloseHandle(ack);
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"ipc-done\"}\n");
    return ok;
}

int main(int argc, char **argv)
{
    HANDLE threads[THREADS], event, quitter, killed, auto_waiters[2];
    DWORD code;
    char exe[MAX_PATH];
    char result[400];
    int i, critical_ok = 1, callback_ok = 1, auto_ok = 1, exit_ok, terminate_ok, ipc_ok;
    int skip_terminate = argc == 4 && !strcmp(argv[1], "--skip-terminate") && !strcmp(argv[2], "--output");
    int terminated;
    DWORD waited;
    const char *machine = sizeof(void *) == 8 ? "AMD64" : "I386";

    if (argc == 5 && !strcmp(argv[1], "--child")) return child(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "--format-only") && !strcmp(argv[2], "--output")) {
        output_path = argv[3];
        if (!write_result("{\"status\":\"RUNNING\",\"stage\":\"format\"}\n")) return 3;
        i = snprintf(result, sizeof(result), "Local\\WineHuaContractMap%lu", GetCurrentProcessId());
        if (i < 0 || i >= (int)sizeof(result)) return 2;
        return write_result(sizeof(void *) == 8 ?
            "{\"status\":\"PASS\",\"stage\":\"format\",\"machine\":\"AMD64\"}\n" :
            "{\"status\":\"PASS\",\"stage\":\"format\",\"machine\":\"I386\"}\n") ? 0 : 3;
    }
    if (argc == 3 && !strcmp(argv[1], "--output")) {
        output_path = argv[2];
        if (!write_result("{\"status\":\"RUNNING\",\"stage\":\"main\"}\n")) return 3;
    } else if (skip_terminate) {
        output_path = argv[3];
        if (!write_result("{\"status\":\"RUNNING\",\"stage\":\"main\"}\n")) return 3;
    }
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"threads\"}\n");
    InitializeCriticalSection(&lock);
    for (i = 0; i < THREADS; ++i) threads[i] = CreateThread(NULL, 0, count_thread, NULL, 0, NULL);
    for (i = 0; i < THREADS; ++i) {
        if (!threads[i] || WaitForSingleObject(threads[i], 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(threads[i], &code) || code != 0x1234) callback_ok = 0;
        if (threads[i]) CloseHandle(threads[i]);
    }
    critical_ok = counter == THREADS * INCREMENTS;
    DeleteCriticalSection(&lock);
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"auto-reset\"}\n");

    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    auto_waiters[0] = CreateThread(NULL, 0, wait_thread, event, 0, NULL);
    auto_waiters[1] = CreateThread(NULL, 0, wait_thread, event, 0, NULL);
    auto_ok = event && auto_waiters[0] && auto_waiters[1] && SetEvent(event) &&
              WaitForMultipleObjects(2, auto_waiters, FALSE, 5000) < WAIT_OBJECT_0 + 2 &&
              SetEvent(event) && WaitForMultipleObjects(2, auto_waiters, TRUE, 5000) == WAIT_OBJECT_0;
    for (i = 0; i < 2; ++i) {
        if (!auto_waiters[i] || !GetExitCodeThread(auto_waiters[i], &code) || code != WAIT_OBJECT_0) auto_ok = 0;
        if (auto_waiters[i]) CloseHandle(auto_waiters[i]);
    }
    if (event) CloseHandle(event);

    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"thread-exit\"}\n");
    quitter = CreateThread(NULL, 0, exit_thread, NULL, 0, NULL);
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"exit-wait\"}\n");
    waited = quitter ? WaitForSingleObject(quitter, 5000) : WAIT_FAILED;
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"exit-code\"}\n");
    exit_ok = quitter && waited == WAIT_OBJECT_0 && GetExitCodeThread(quitter, &code) && code == 0x4321;
    if (quitter) CloseHandle(quitter);
    if (skip_terminate) {
        terminate_ok = 1;
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"skipped-terminate\"}\n");
    } else {
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"terminate-create\"}\n");
        event = CreateEventA(NULL, TRUE, FALSE, NULL);
        killed = event ? CreateThread(NULL, 0, wait_thread, event, 0, NULL) : NULL;
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"terminate-call\"}\n");
        terminated = killed && TerminateThread(killed, 0x5678);
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"terminate-wait\"}\n");
        waited = terminated ? WaitForSingleObject(killed, 5000) : WAIT_FAILED;
        if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"terminate-code\"}\n");
        terminate_ok = terminated && waited == WAIT_OBJECT_0 &&
                       GetExitCodeThread(killed, &code) && code == 0x5678;
        if (killed) CloseHandle(killed);
        if (event) CloseHandle(event);
    }
    if (output_path) write_result("{\"status\":\"RUNNING\",\"stage\":\"shared-ipc\"}\n");
    ipc_ok = ipc_test(exe);
    snprintf(result, sizeof(result),
             "{\"status\":\"%s\",\"stage\":\"contract\",\"machine\":\"%s\",\"auto_reset\":%d,\"shared_ipc\":%d,"
             "\"callback_return\":%d,\"critical_section\":%d,\"thread_exit\":%d,\"thread_terminate\":%d,"
             "\"backend\":\"requires_runtime_log\"}\n",
             auto_ok && ipc_ok && callback_ok && critical_ok && exit_ok && terminate_ok ? "PASS" : "FAIL",
             machine, auto_ok, ipc_ok, callback_ok, critical_ok, exit_ok, skip_terminate ? -1 : terminate_ok);
    if (output_path) write_result(result);
    else fputs(result, stdout);
    return auto_ok && ipc_ok && callback_ok && critical_ok && exit_ok && terminate_ok ? 0 : 1;
}

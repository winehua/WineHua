/* Read-only, bounded snapshots for local Wine memory-map diagnosis. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    PROCESSENTRY32 entry = {0};
    HANDLE snapshot, process = NULL;
    FILE *out;
    DWORD pid = 0;
    SIZE_T address = 0, next, size;
    MEMORY_BASIC_INFORMATION info;
    unsigned int count;
    int i;
    if (argc < 2) return 2;
    entry.dwSize = sizeof(entry);
    out = fopen("C:\\windows\\temp\\winehua-process-memory.txt", "wb");
    if (!out) return 3;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        if (Process32First(snapshot, &entry)) do
        {
            if (!_stricmp(entry.szExeFile, argv[1])) { pid = entry.th32ProcessID; break; }
        } while (Process32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    if (pid) process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    fprintf(out, "target=%s pid=%lu handle=%p error=%lu tick=%llu\n",
            argv[1], pid, process, GetLastError(), GetTickCount64());
    if (!process) { fclose(out); return 4; }
    for (count = 0; address < 0x100000000ULL && count < 65536; ++count)
    {
        if (!VirtualQueryEx(process, (void *)address, &info, sizeof(info))) break;
        next = (SIZE_T)info.BaseAddress + info.RegionSize;
        if (info.State != MEM_FREE)
            fprintf(out, "map=%p-%p alloc=%p state=%lx protect=%lx type=%lx\n",
                    info.BaseAddress, (void *)next, info.AllocationBase, info.State, info.Protect, info.Type);
        if (next <= address) break;
        address = next;
    }
    for (i = 2; i < argc; ++i)
    {
        unsigned char data[256];
        BOOL ok;
        address = (SIZE_T)strtoull(argv[i], NULL, 0);
        size = 0;
        ok = ReadProcessMemory(process, (void *)address, data, sizeof(data), &size);
        fprintf(out, "read=%p ok=%d size=%llu error=%lu\n", (void *)address, ok,
                (unsigned long long)size, GetLastError());
        for (count = 0; count < size; ++count)
        {
            if (!(count % 16)) fprintf(out, "%llx:", (unsigned long long)address + count);
            fprintf(out, " %02x", data[count]);
            if (count % 16 == 15 || count + 1 == size) fputc('\n', out);
        }
    }
    CloseHandle(process);
    fclose(out);
    return 0;
}

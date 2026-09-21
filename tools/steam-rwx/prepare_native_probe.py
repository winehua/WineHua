"""Stage a temporary, bounded Wine native-fault/TEB probe for an isolated HAP.

Keep the original sources in artifacts/fex-rwx-probe/v15-source-before.
This probe is not applied by the product build script.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
unix = root / "thirdparty/wine-valve/dlls/ntdll/unix"
path = unix / "ohos_virtual.c"
source = path.read_text()
begin = source.index("    /* 2026-09-20 (ROUND3 §9.10):")
end = source.index("    dynarec =", begin)
source = source[:begin] + '''    /* FEX handles guest SMC after Wine's SEH dispatch. Do not change guest
     * page permissions here based on native maps: v9-v11 disproved that
     * recovery, and even its disabled path raced on the diagnostic cache. */
\n''' + source[end:]
needle = "    ohos_smc_log_enter( sig, info, pc_in, xrip_in, emu, lr, sp, x16, x17, teb, fn );"
probe = '''    /* Temporary probe: saved registers only, no guest pointer dereferences,
     * libc formatting, maps parsing or allocation in this signal handler. */
    if (sig == SIGSEGV)
    {
        static unsigned int count;
        unsigned int seq = __atomic_add_fetch( &count, 1, __ATOMIC_RELAXED );
        if (seq <= 1024)
        {
            struct ohos_signal_log log = {{0}, 0};
            const int saved_errno = errno;
            ohos_signal_log_str( &log, "[FAULT-MIN]" );
            ohos_signal_log_field_dec( &log, "pid", getpid() );
            ohos_signal_log_field_dec( &log, "tid", syscall( SYS_gettid ) );
            ohos_signal_log_field_dec( &log, "seq", seq );
            ohos_signal_log_field_dec( &log, "code", info ? info->si_code : 0 );
            ohos_signal_log_field_hex( &log, "addr", (uintptr_t)(info ? info->si_addr : NULL) );
            ohos_signal_log_field_hex( &log, "pc", pc_in );
            ohos_signal_log_field_hex( &log, "lr", lr );
            ohos_signal_log_field_hex( &log, "sp", sp );
            ohos_signal_log_field_hex( &log, "teb", (uintptr_t)teb );
            ohos_signal_log_field_hex( &log, "self", (uintptr_t)ohos_route_host_fault );
            ohos_signal_log_field_hex( &log, "x9", uc ? uc->uc_mcontext.regs[9] : 0 );
            ohos_signal_log_field_hex( &log, "x18", uc ? uc->uc_mcontext.regs[18] : 0 );
            ohos_signal_log_emit( &log );
            errno = saved_errno;
        }
    }
'''
assert "[FAULT-MIN]" not in source
source = source.replace(needle, probe + needle, 1)
path.write_text(source)

path = unix / "virtual.c"
source = path.read_text()
needle = "/* set some initial values in a new TEB */"
probe = '''/* Temporary candidate-only TEB lifetime trace; never dereference the
 * suspect TEB while logging. All callers hold virtual_mutex. */
static void ohos_teb_probe( const char *event, void *ptr, SIZE_T size, ULONG value )
{
#ifdef __OHOS__
    static unsigned int count;
    char buf[240];
    int n, saved_errno = errno;
    if (++count > 8192) return;
    n = snprintf( buf, sizeof(buf),
                  "[TEB-PROBE] pid=%d tid=%ld event=%s ptr=%p size=%lx value=%x current=%p\\n",
                  getpid(), syscall( SYS_gettid ), event, ptr, size, value, NtCurrentTeb() );
    if (n > 0) write( 2, buf, n < sizeof(buf) ? n : sizeof(buf) - 1 );
    errno = saved_errno;
#endif
}

'''
assert "[TEB-PROBE]" not in source
source = source.replace(needle, probe + needle, 1)
source = source.replace("    list_add_head( &teb_list, &thread_data->entry );", "    ohos_teb_probe( \"add\", teb, 0, is_wow );\n    list_add_head( &teb_list, &thread_data->entry );", 1)
source = source.replace("    list_remove( &thread_data->entry );", "    ohos_teb_probe( \"remove\", teb, 0, 0 );\n    list_remove( &thread_data->entry );", 1)
source = source.replace("        NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &block_size,\n                                 MEM_COMMIT, PAGE_READWRITE );", "        status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &block_size,\n                                          MEM_COMMIT, PAGE_READWRITE );\n        ohos_teb_probe( \"commit\", ptr, block_size, status );", 1)
begin = source.index("NTSTATUS virtual_clear_tls_index(")
end = source.index("\n}\n", begin)
section = source[begin:end]
section = section.replace("#ifdef _WIN64", '            ohos_teb_probe( "tls-clear", teb, 0, index );\n#ifdef _WIN64')
source = source[:begin] + section + source[end:]
needle = "    case MEM_DECOMMIT:\n        status = decommit_pages( view, base, size );"
assert needle in source
source = source.replace(needle, '    case MEM_DECOMMIT:\n        if ((ULONG_PTR)base < 0x1000000) ohos_teb_probe( "decommit-low", base, size, type );\n        status = decommit_pages( view, base, size );', 1)
path.write_text(source)
print("Staged native-fault/TEB probe; removed disproved W/X recovery path")

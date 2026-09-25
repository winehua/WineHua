/*
 * WineHua DNS smoke probe.
 *
 * Exercises the wine dnsapi unixlib end to end: DnsQuery_A against a real
 * hostname (UDP:53/TCP:53 wire path, any record type) plus an NXDOMAIN lookup
 * (.invalid is RFC-2606 reserved, never resolves, no network dependency).
 *
 * Purpose:  verify the builtin resolver fallback for hosts without libresolv
 * (OHOS musl).  On those hosts wine's libresolv.c compiles to an empty unix
 * library and DnsQuery_A crashes with 0xC0000005 unless the musl fallback
 * (dlls/dnsapi/libresolv_musl.c) is in place.  This probe is the smoke-suite
 * gate: PASS means DnsQuery_A returned an address without crashing; the
 * resolved IP is embedded in the result metrics.
 *
 * Links against the system WinDNS import library (mingw) — the PE call goes
 * through wine's dnsapi.dll as in a real application.
 *
 * Smoke protocol: --automation --run-id --test-id --result <path>
 * (winehua_smoke_protocol.h in the wine submodule, same as the other probes).
 */

#include <windows.h>
#include <windns.h>
#include <stdio.h>
#include <string.h>

#include "../thirdparty/wine/programs/winehua_smoke_protocol.h"

#define HOST_REAL    "www.baidu.com"
#define HOST_NX      "winehua-dns-probe.invalid"

struct probe_state
{
    struct winehua_smoke_options options;
    BOOL can_resolve;
    DWORD resolve_error;
    char resolve_ip[32];
    DWORD nx_query_error;
    BOOL nx_expected_failure;
};

static void format_ipv4( IP4_ADDRESS addr, char *out, int out_len )
{
    const unsigned char *b = (const unsigned char *)&addr;
    snprintf( out, out_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3] );
}

static void probe_resolve_real( struct probe_state *state )
{
    PDNS_RECORD records = NULL;
    DWORD rc;

    state->can_resolve = FALSE;
    state->resolve_error = 0;
    state->resolve_ip[0] = 0;

    rc = DnsQuery_A( HOST_REAL, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &records, NULL );
    if (rc != 0)
    {
        state->resolve_error = rc;
        return;
    }

    /* walk the record list for the first A record */
    {
        PDNS_RECORD rec = records;
        while (rec)
        {
            if ((rec->wType == DNS_TYPE_A) && !state->resolve_ip[0])
                format_ipv4( rec->Data.A.IpAddress, state->resolve_ip, sizeof(state->resolve_ip) );
            rec = rec->pNext;
        }
    }

    state->can_resolve = state->resolve_ip[0] != 0;
    DnsRecordListFree( records, DnsFreeRecordList );
}

static void probe_resolve_nxdomain( struct probe_state *state )
{
    PDNS_RECORD records = NULL;
    DWORD rc;

    /* .invalid must never resolve: any non-zero return is the expected result
     * (most commonly DNS_ERROR_RCODE_NAME_ERROR 9003).  A zero return here
     * would mean the resolver is feeding answers for reserved names. */
    rc = DnsQuery_A( HOST_NX, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &records, NULL );
    state->nx_query_error = rc;
    state->nx_expected_failure = (rc != 0);
}

static BOOL write_result( struct probe_state *state )
{
    char metrics[192];
    char message[256];
    const char *status;
    BOOL ok;

    ok = state->can_resolve && state->nx_expected_failure;
    status = ok ? "PASS" : "FAIL";

    snprintf( metrics, sizeof(metrics),
              "{\"ip\": \"%s\", \"err\": %lu, \"nx_err\": %lu}",
              state->resolve_ip, state->resolve_error, state->nx_query_error );
    snprintf( message, sizeof(message),
              "resolve \"%s\" -> %s (%s); NX \"%s\" -> rc=%lu %s",
              HOST_REAL, ok ? "ok" : "failed",
              state->resolve_ip[0] ? state->resolve_ip : "-",
              HOST_NX, state->nx_query_error, state->nx_expected_failure ? "expected" : "UNEXPECTED" );

    return winehua_smoke_write_result( &state->options, status, "dns-api", message, metrics );
}

int main( int argc, char **argv )
{
    struct probe_state state;
    memset( &state, 0, sizeof(state) );

    if (!winehua_smoke_parse_options( &state.options, argc, argv, 6 )) return 2;

    probe_resolve_real( &state );
    probe_resolve_nxdomain( &state );

    if (!write_result( &state )) return 3;
    return (state.can_resolve && state.nx_expected_failure) ? 0 : 1;
}

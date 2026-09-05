/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 WineHua contributors.
 *
 * This file is part of WineHua. Source of the dnsapi.so shipped in
 * rawfile/dnsapi-fix.zip (see AppLibraryService.ensureDnsapiPatch).
 */
/*
 * dnsapi unixlib for OHOS musl — drop-in replacement for wine's libresolv.c
 *
 * OHOS musl exposes res_query/res_init as link-time stubs (all mapped to one
 * address) and has no _res state, so HAVE_RESOLV can never be satisfied there.
 * This implementation talks DNS directly: /etc/resolv.conf for config, raw
 * UDP:53 for real queries (any record type), getaddrinfo synthesis as a
 * last-resort fallback for A/AAAA when UDP is blocked.
 *
 * Exports the exact wine unixlib ABI:
 *   __wine_unix_call_funcs[]      (unix_get_searchlist, unix_get_serverlist,
 *                                  unix_set_serverlist, unix_query)
 *   __wine_unix_call_wow64_funcs[]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---- minimal Windows-ish types (host ABI) ---- */
typedef unsigned long long QWORD;
typedef uint32_t  DWORD, UINT, ULONG, PTR32_;
typedef uint16_t  WORD, USHORT, WCHAR;
typedef uint8_t   UCHAR, BYTE;
typedef int       NTSTATUS;

#define ERROR_SUCCESS_           0x00000000
#define ERROR_MORE_DATA_         0x000000EA
#define DNS_ERROR_RCODE_FORMAT   9001
#define DNS_ERROR_RCODE_SERVER_FAILURE 9002
#define DNS_ERROR_RCODE_NAME_ERROR     9003
#define DNS_ERROR_RCODE_NOT_IMPLEMENTED 9004
#define DNS_ERROR_RCODE_REFUSED  9005
#define DNS_ERROR_RCODE          9016
#define DNS_ERROR_NO_DNS_SERVERS 9928
#define DNS_ERROR_INVALID_NAME   123
#define DNS_ERROR_TIMEOUT_       1462

/* Windows address families as stored inside MaxSa */
#define WS_AF_INET   2
#define WS_AF_INET6  23

/* ---- windns.h: DNS_ADDR / DNS_ADDR_ARRAY (pragma pack 1) ---- */
#define DNS_ADDR_MAX_SOCKADDR_LENGTH 32
#pragma pack(push,1)
typedef struct {
    char  MaxSa[DNS_ADDR_MAX_SOCKADDR_LENGTH];
    DWORD DnsAddrUserDword[8];
} DNS_ADDR;
typedef struct {
    DWORD MaxCount; DWORD AddrCount; DWORD Tag;
    WORD  Family;   WORD  WordReserved;
    DWORD Flags;    DWORD MatchFlag; DWORD Reserved1; DWORD Reserved2;
    DNS_ADDR AddrArray[1];
} DNS_ADDR_ARRAY;
#pragma pack(pop)

/* ---- wine dnsapi unix params (dlls/dnsapi/dnsapi.h) ---- */
struct get_searchlist_params { WCHAR *list; DWORD *len; };
struct get_serverlist_params { USHORT family; DNS_ADDR_ARRAY *addrs; DWORD *len; };
struct query_params { const char *name; WORD type; DWORD options; void *buf; DWORD *len; };

/* dnsapi.h set_serverlist args */
typedef struct { DWORD AddrCount; DWORD AddrArray[1]; } IP4_ARRAY;

typedef NTSTATUS (*unixlib_entry_t)( void * );

#define MAX_SERVERS  8
#define MAX_SEARCH   8
#define MAX_NAME_LEN 511

struct dns_config
{
    struct sockaddr_storage servers[MAX_SERVERS];
    int server_count;
    char search[MAX_SEARCH][MAX_NAME_LEN + 1];
    int search_count;
};

static struct dns_config g_config;
static int g_config_loaded;
static time_t g_config_mtime;
static ino_t  g_config_ino;

/* servers set through set_serverlist (IPv4 only, like wine's impl) */
static struct sockaddr_in g_override[MAX_SERVERS];
static int g_override_count;

/* ---------- /etc/resolv.conf ---------- */

static void parse_resolv_conf( void )
{
    FILE *f;
    char line[1024];
    struct stat st;

    memset( &g_config, 0, sizeof(g_config) );
    g_config_loaded = 1;

    if (stat( "/etc/resolv.conf", &st ) == 0)
    {
        g_config_mtime = st.st_mtime;
        g_config_ino = st.st_ino;
    }

    f = fopen( "/etc/resolv.conf", "r" );
    if (!f) return;

    while (fgets( line, sizeof(line), f ))
    {
        char key[32], val[512];
        if (sscanf( line, "%31s %511s", key, val ) != 2) continue;

        if (!strcmp( key, "nameserver" ) && g_config.server_count < MAX_SERVERS)
        {
            struct sockaddr_in *sa4 = (struct sockaddr_in *)
                &g_config.servers[g_config.server_count];
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)
                &g_config.servers[g_config.server_count];
            if (inet_pton( AF_INET, val, &sa4->sin_addr ) == 1)
            {
                sa4->sin_family = AF_INET;
                sa4->sin_port = htons( 53 );
                g_config.server_count++;
            }
            else if (inet_pton( AF_INET6, val, &sa6->sin6_addr ) == 1)
            {
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = htons( 53 );
                g_config.server_count++;
            }
        }
        else if (!strcmp( key, "search" ) || !strcmp( key, "domain" ))
        {
            /* "domain foo" has one arg; "search a b" takes the rest of line */
            char *rest = line, *tok;
            while (*rest && !isspace( (unsigned char)*rest )) rest++;
            while (g_config.search_count < MAX_SEARCH)
            {
                tok = strtok( rest, " \t\r\n" );
                if (!tok) break;
                rest = NULL;
                if (strlen( tok ) <= MAX_NAME_LEN)
                {
                    strcpy( g_config.search[g_config.search_count], tok );
                    g_config.search_count++;
                }
            }
        }
    }
    fclose( f );
}

static void ensure_config( void )
{
    struct stat st;
    if (!g_config_loaded) { parse_resolv_conf(); return; }
    if (stat( "/etc/resolv.conf", &st ) != 0) return;
    if (st.st_mtime != g_config_mtime || st.st_ino != g_config_ino)
        parse_resolv_conf();
}

/* ---------- search list ---------- */

static NTSTATUS resolv_get_searchlist( void *args )
{
    struct get_searchlist_params *params = args;
    WCHAR *list = params->list;
    DWORD needed = 0, i;
    char conf[2048];
    int off = 0;

    ensure_config();

    conf[0] = 0;
    for (i = 0; i < (DWORD)g_config.search_count && off < (int)sizeof(conf) - 1; i++)
        off += snprintf( conf + off, sizeof(conf) - off, "%s%s", i ? " " : "",
                         g_config.search[i] );

    needed = sizeof(WCHAR); /* final terminator */
    {
        char *p = conf;
        while (*p)
        {
            char *sp = strchr( p, ' ' );
            size_t l = sp ? (size_t)(sp - p) : strlen( p );
            needed += (l + 1) * sizeof(WCHAR);
            if (!sp) break;
            p = sp + 1;
        }
    }

    if (!list || *params->len < needed)
    {
        *params->len = needed;
        return !list ? ERROR_SUCCESS_ : ERROR_MORE_DATA_;
    }
    *params->len = needed;

    {
        char *p = conf;
        WCHAR *w = list;
        while (*p)
        {
            char *sp = strchr( p, ' ' );
            size_t l = sp ? (size_t)(sp - p) : strlen( p );
            DWORD k;
            for (k = 0; k < l; k++) *w++ = (WCHAR)(unsigned char)p[k];
            *w++ = 0;
            if (!sp) break;
            p = sp + 1;
        }
        *w = 0;
    }
    return ERROR_SUCCESS_;
}

/* ---------- server list ---------- */

static int family_matches( int sin_family, USHORT want )
{
    if (sin_family != AF_INET && sin_family != AF_INET6) return 0;
    if (sin_family == AF_INET6 && want == WS_AF_INET) return 0;
    if (sin_family == AF_INET && want == WS_AF_INET6) return 0;
    return 1;
}

static NTSTATUS resolv_get_serverlist( void *args )
{
    struct get_serverlist_params *params = args;
    DNS_ADDR_ARRAY *addrs = params->addrs;
    struct sockaddr *servers[MAX_SERVERS];
    int total = 0, i, found, filter_idx[MAX_SERVERS];
    DWORD needed;
    const size_t header = 32; /* FIELD_OFFSET(DNS_ADDR_ARRAY, AddrArray) */

    ensure_config();

    for (i = 0; i < g_override_count && total < MAX_SERVERS; i++)
        servers[total++] = (struct sockaddr *)&g_override[i];
    for (i = 0; g_override_count == 0 && i < g_config.server_count && total < MAX_SERVERS; i++)
        servers[total++] = (struct sockaddr *)&g_config.servers[i];

    if (!total) return DNS_ERROR_NO_DNS_SERVERS;

    found = 0;
    for (i = 0; i < total; i++)
    {
        if (!family_matches( servers[i]->sa_family, params->family )) continue;
        filter_idx[found++] = i;
    }
    if (!found) return DNS_ERROR_NO_DNS_SERVERS;

    needed = header + sizeof(DNS_ADDR) * found;
    if (!addrs || *params->len < needed)
    {
        *params->len = needed;
        return !addrs ? ERROR_SUCCESS_ : ERROR_MORE_DATA_;
    }
    *params->len = needed;
    memset( addrs, 0, needed );
    addrs->MaxCount = addrs->AddrCount = found;

    for (i = 0; i < found; i++)
    {
        struct sockaddr *sa = servers[filter_idx[i]];
        BYTE *m = (BYTE *)addrs->AddrArray[i].MaxSa;
        if (sa->sa_family == AF_INET6)
        {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)sa;
            /* Windows sockaddr_in6: family, port, flowinfo, addr */
            *(USHORT *)(m + 0) = WS_AF_INET6;
            *(USHORT *)(m + 2) = s6->sin6_port;
            *(DWORD  *)(m + 4) = s6->sin6_flowinfo;
            memcpy( m + 8, &s6->sin6_addr, 16 );
            addrs->AddrArray[i].DnsAddrUserDword[0] = 28;
        }
        else
        {
            struct sockaddr_in *s4 = (struct sockaddr_in *)sa;
            *(USHORT *)(m + 0) = WS_AF_INET;
            *(USHORT *)(m + 2) = s4->sin_port;
            memcpy( m + 4, &s4->sin_addr, 4 );
            addrs->AddrArray[i].DnsAddrUserDword[0] = 16;
        }
    }
    return ERROR_SUCCESS_;
}

static NTSTATUS resolv_set_serverlist( void *args )
{
    IP4_ARRAY *addrs = args;
    int i;

    g_override_count = 0;
    if (!addrs || !addrs->AddrCount) return ERROR_SUCCESS_;

    for (i = 0; i < (int)addrs->AddrCount && g_override_count < MAX_SERVERS; i++)
    {
        struct sockaddr_in *sa = &g_override[g_override_count++];
        sa->sin_family = AF_INET;
        sa->sin_port = htons( 53 );
        sa->sin_addr.s_addr = addrs->AddrArray[i];
    }
    return ERROR_SUCCESS_;
}

/* ---------- wire query ---------- */

static int encode_qname( unsigned char *out, const char *name )
{
    const char *p = name;
    int total = 1; /* root byte */

    if (!*p || strlen( name ) > MAX_NAME_LEN) return -1;
    while (*p)
    {
        const char *dot = strchr( p, '.' );
        size_t l = dot ? (size_t)(dot - p) : strlen( p );
        if (!l || l > 63) return -1;
        *out++ = (unsigned char)l;
        memcpy( out, p, l );
        out += l;
        total += (int)l + 1;
        p += l;
        if (*p == '.') p++;
    }
    *out++ = 0;
    return total;
}

static NTSTATUS map_rcode( int rcode )
{
    switch (rcode & 0xf)
    {
    case 0:  return ERROR_SUCCESS_;
    case 1:  return DNS_ERROR_RCODE_FORMAT;
    case 2:  return DNS_ERROR_RCODE_SERVER_FAILURE;
    case 3:  return DNS_ERROR_RCODE_NAME_ERROR;
    case 4:  return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    case 5:  return DNS_ERROR_RCODE_REFUSED;
    default: return DNS_ERROR_RCODE;
    }
}

static unsigned gettimeofday_ms( void )
{
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return (unsigned)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

static unsigned short next_id( void )
{
    unsigned short id;
    int fd = open( "/dev/urandom", O_RDONLY );
    if (fd >= 0)
    {
        if (read( fd, &id, sizeof(id) ) == (ssize_t)sizeof(id)) { close( fd ); return id; }
        close( fd );
    }
    return (unsigned short)(getpid() ^ gettimeofday_ms());
}

static int udp_query_one( struct sockaddr *sa, socklen_t salen,
                          const unsigned char *qbuf, int qlen,
                          unsigned char *rbuf, int rbufsize, unsigned short id,
                          int timeout_ms )
{
    int fd = socket( sa->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
    struct pollfd pfd;
    int n;

    if (fd < 0) return -1;
    if (sendto( fd, qbuf, qlen, 0, sa, salen ) != qlen) { close( fd ); return -1; }

    pfd.fd = fd;
    pfd.events = POLLIN;
    n = poll( &pfd, 1, timeout_ms );
    if (n > 0 && (pfd.revents & POLLIN))
    {
        n = recv( fd, rbuf, rbufsize, 0 );
        close( fd );
        if (n >= 12 && ((rbuf[0] << 8) | rbuf[1]) == id && (rbuf[2] & 0x80))
            return n;
        return -1;
    }
    close( fd );
    return -1;
}

/* DNS over TCP: some carrier/campus networks intercept UDP:53 and answer
 * FORMERR for every query; TCP:53 passes through untouched. */
static int tcp_query_one( struct sockaddr *sa, socklen_t salen,
                          const unsigned char *qbuf, int qlen,
                          unsigned char *rbuf, int rbufsize, unsigned short id,
                          int timeout_ms )
{
    int fd = socket( sa->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0 );
    struct pollfd pfd;
    unsigned char lenbuf[2];
    int want, got, n, msglen;

    if (fd < 0) return -1;
    fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK );
    if (connect( fd, sa, salen ) < 0 && errno != EINPROGRESS)
    {
        close( fd );
        return -1;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll( &pfd, 1, timeout_ms ) <= 0) { close( fd ); return -1; }

    lenbuf[0] = qlen >> 8; lenbuf[1] = qlen & 0xff;
    if (send( fd, lenbuf, 2, MSG_NOSIGNAL ) != 2 ||
        send( fd, qbuf, qlen, MSG_NOSIGNAL ) != qlen)
    {
        close( fd );
        return -1;
    }

    /* 2-byte length prefix */
    got = 0;
    while (got < 2)
    {
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll( &pfd, 1, timeout_ms ) <= 0) { close( fd ); return -1; }
        n = recv( fd, lenbuf + got, 2 - got, 0 );
        if (n <= 0) { close( fd ); return -1; }
        got += n;
    }
    msglen = (lenbuf[0] << 8) | lenbuf[1];
    if (msglen < 12 || msglen > rbufsize) { close( fd ); return -1; }

    /* message body */
    got = 0;
    while (got < msglen)
    {
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll( &pfd, 1, timeout_ms ) <= 0) { close( fd ); return -1; }
        n = recv( fd, rbuf + got, msglen - got, 0 );
        if (n <= 0) { close( fd ); return -1; }
        got += n;
    }
    close( fd );

    if (((rbuf[0] << 8) | rbuf[1]) == id && (rbuf[2] & 0x80))
        return msglen;
    return -1;
}

/* synthesize a wire response from getaddrinfo (A/AAAA fallback) */
static int synthesize_from_getaddrinfo( const char *name, WORD type,
                                        unsigned char *buf, int bufsize,
                                        const unsigned char *query, int qlen )
{
    struct addrinfo hints, *res = NULL, *ai;
    unsigned char *p;
    int ancount = 0, rdlen = (type == 1) ? 4 : 16;

    if (type != 1 && type != 28) return -1;
    if (qlen + 16 + 16 + rdlen > bufsize) return -1;

    memset( &hints, 0, sizeof(hints) );
    hints.ai_family = (type == 1) ? AF_INET : AF_INET6;
    if (getaddrinfo( name, NULL, &hints, &res ) != 0 || !res) return -1;

    for (ai = res; ai; ai = ai->ai_next) ancount++;

    /* echo the question, then answers */
    memcpy( buf, query, qlen );
    p = buf + qlen;
    buf[2] = 0x81; /* QR + RD */
    buf[3] = 0x80; /* RA */
    buf[6] = 0; buf[7] = (unsigned char)ancount;

    for (ai = res; ai; ai = ai->ai_next)
    {
        *p++ = 0xc0; *p++ = 0x0c;              /* name pointer */
        *p++ = 0; *p++ = (unsigned char)type;  /* type */
        *p++ = 0; *p++ = 1;                    /* class IN */
        *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 60; /* ttl 60s */
        *p++ = 0; *p++ = (unsigned char)rdlen;
        if (type == 1)
            memcpy( p, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, 4 );
        else
            memcpy( p, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, 16 );
        p += rdlen;
    }
    freeaddrinfo( res );
    return (int)(p - buf);
}

static NTSTATUS resolv_query( void *args )
{
    struct query_params *params = args;
    unsigned char qbuf[512 + 4], *rbuf = (unsigned char *)params->buf;
    unsigned short id = next_id();
    int qlen, n = -1, i, attempt, rcode;
    DWORD bufsize = *params->len;

    if (!params->name || !params->buf || bufsize < 12) return DNS_ERROR_INVALID_NAME;

    /* header */
    qbuf[0] = id >> 8; qbuf[1] = id & 0xff;
    qbuf[2] = 0x01; qbuf[3] = 0x00; /* RD */
    qbuf[4] = 0; qbuf[5] = 1;       /* one question */
    qbuf[6] = qbuf[7] = qbuf[8] = qbuf[9] = 0;
    qlen = encode_qname( qbuf + 12, params->name );
    if (qlen < 0) return DNS_ERROR_INVALID_NAME;
    qlen += 12;
    qbuf[qlen++] = params->type >> 8;   /* QTYPE hi */
    qbuf[qlen++] = params->type & 0xff; /* QTYPE lo */
    qbuf[qlen++] = 0;                   /* QCLASS hi */
    qbuf[qlen++] = 1;                   /* QCLASS lo = IN */

    ensure_config();

    for (attempt = 0; attempt < 2 && n < 0; attempt++)
    {
        int timeout = attempt ? 3000 : 2000;
        for (i = 0; i < g_override_count && n < 0; i++)
        {
            n = udp_query_one( (struct sockaddr *)&g_override[i], sizeof(g_override[i]),
                               qbuf, qlen, rbuf, (int)bufsize, id, timeout );
            if (n < 0)
                n = tcp_query_one( (struct sockaddr *)&g_override[i], sizeof(g_override[i]),
                                   qbuf, qlen, rbuf, (int)bufsize, id, timeout );
        }
        for (i = 0; n < 0 && i < g_config.server_count; i++)
        {
            socklen_t salen = g_config.servers[i].ss_family == AF_INET6 ?
                              sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
            n = udp_query_one( (struct sockaddr *)&g_config.servers[i], salen,
                               qbuf, qlen, rbuf, (int)bufsize, id, timeout );
            if (n < 0)
                n = tcp_query_one( (struct sockaddr *)&g_config.servers[i], salen,
                                   qbuf, qlen, rbuf, (int)bufsize, id, timeout );
        }
    }

    /* Some carrier/campus networks intercept UDP:53 and answer FORMERR for
     * every foreign query while the system resolver (musl getaddrinfo) keeps
     * working. Treat any non-NOERROR wire response the same as no response and
     * fall back to getaddrinfo for A/AAAA. */
    if (n < 0 || (n >= 12 && (rbuf[3] & 0x0f) != 0))
    {
        int wire_rcode = (n >= 12) ? (rbuf[3] & 0x0f) : 2;
        int m = synthesize_from_getaddrinfo( params->name, params->type,
                                            rbuf, (int)bufsize, qbuf, qlen );
        {
            FILE *d = fopen( "/data/storage/el2/base/temp/dnsresp.bin", "wb" );
            if (d)
            {
                fwrite( rbuf, 1, (m < 0 ? (size_t)n : (size_t)m), d );
                fclose( d );
            }
        }
        if (m < 0)
        {
            if (wire_rcode != 2 && wire_rcode != 0) return map_rcode( wire_rcode );
            if (n >= 12 && (rbuf[3] & 0x0f) == 3) return DNS_ERROR_RCODE_NAME_ERROR;
            return DNS_ERROR_TIMEOUT_;
        }
        if ((DWORD)m > bufsize) { *params->len = m; return ERROR_MORE_DATA_; }
        *params->len = m;
        return ERROR_SUCCESS_;
    }

    rcode = rbuf[3] & 0x0f;
    if ((DWORD)n > bufsize) { *params->len = n; return ERROR_MORE_DATA_; }
    *params->len = n;
    return map_rcode( rcode );
}

/* ---------- wow64 thunks ---------- */

static NTSTATUS wow64_resolv_get_searchlist( void *args )
{
    struct { PTR32_ list; PTR32_ len; } const *params32 = args;
    struct get_searchlist_params params =
        { (WCHAR *)(uintptr_t)params32->list, (DWORD *)(uintptr_t)params32->len };
    return resolv_get_searchlist( &params );
}

static NTSTATUS wow64_resolv_get_serverlist( void *args )
{
    struct { USHORT family; USHORT pad; PTR32_ addrs; PTR32_ len; } const *params32 = args;
    struct get_serverlist_params params =
        { params32->family, (DNS_ADDR_ARRAY *)(uintptr_t)params32->addrs,
          (DWORD *)(uintptr_t)params32->len };
    return resolv_get_serverlist( &params );
}

static NTSTATUS wow64_resolv_query( void *args )
{
    struct { PTR32_ name; WORD type; WORD pad; DWORD options; PTR32_ buf; PTR32_ len; } const *params32 = args;
    struct query_params params =
        { (const char *)(uintptr_t)params32->name, params32->type, params32->options,
          (void *)(uintptr_t)params32->buf, (DWORD *)(uintptr_t)params32->len };
    return resolv_query( &params );
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    resolv_get_searchlist,
    resolv_get_serverlist,
    resolv_set_serverlist,
    resolv_query,
};

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_resolv_get_searchlist,
    wow64_resolv_get_serverlist,
    resolv_set_serverlist,
    wow64_resolv_query,
};

/* Versioned Wine/OHOS startup transport. Keep the Wine patch copy identical.
 * Copyright 2026 VintagePomelo contributors. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VP_SPAWN_PROTOCOL_H
#define VP_SPAWN_PROTOCOL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The SDK documents 150 KB. Use a conservative byte budget including NUL. */
#define VP_ENTRY_CAP 150000u
#define VP_PAYLOAD_CAP (112u * 1024u)
#define VP_ITEMS_CAP 4096u
#define VP_FDS_CAP 16u
#define VP_CLIENT_FDS_CAP 15u
#define VP_FRAME_HEADER 12u
#define VP_ENTRY_PREFIX "VPP2:"
typedef struct vp_spawn_spec {
    const char *home, *bin;
    const char **argv, **env, **names;
    uint32_t argc, envc, fdc;
} vp_spawn_spec;
typedef struct vp_received_fds { int items[VP_FDS_CAP]; size_t count; } vp_received_fds;
static inline int vp_is_process_env(const char *env)
{
    return !strncmp(env, "WINESERVERSOCKET=", sizeof("WINESERVERSOCKET=") - 1) ||
        !strncmp(env, "WINE_OHOS_AUDIO_ENABLE=", sizeof("WINE_OHOS_AUDIO_ENABLE=") - 1) ||
        !strncmp(env, "WINE_OHOS_AUDIO_BOOTSTRAP_FD=", sizeof("WINE_OHOS_AUDIO_BOOTSTRAP_FD=") - 1) ||
        !strncmp(env, "WINE_OHOS_AUDIO_PROTOCOL_VERSION=", sizeof("WINE_OHOS_AUDIO_PROTOCOL_VERSION=") - 1);
}

static inline void vp_store32(unsigned char *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint32_t vp_load32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int vp_bad(int error) { errno = error; return -1; }
static inline void vp_free_spec(vp_spawn_spec *s)
{
    uint32_t i;
    free((void *)s->home); free((void *)s->bin);
    if (s->argv) for (i = 0; i < s->argc; ++i) free((void *)s->argv[i]);
    if (s->env) for (i = 0; i < s->envc; ++i) free((void *)s->env[i]);
    if (s->names) for (i = 0; i < s->fdc; ++i) free((void *)s->names[i]);
    free(s->argv); free(s->env); free(s->names); memset(s, 0, sizeof(*s));
}
static inline int vp_validate_spec(const vp_spawn_spec *s)
{
    uint32_t i, j;
    if (!s->home || !s->bin || !s->bin[0] || !s->argc || s->argc > VP_ITEMS_CAP ||
        s->envc > VP_ITEMS_CAP || s->fdc > VP_FDS_CAP || !s->argv ||
        (s->envc && !s->env) || (s->fdc && !s->names)) return vp_bad(EINVAL);
    for (i = 0; i < s->argc; ++i) if (!s->argv[i]) return vp_bad(EINVAL);
    if (!s->argv[0][0]) return vp_bad(EINVAL);
    for (i = 0; i < s->envc; ++i) {
        const char *eq = s->env[i] ? strchr(s->env[i], '=') : NULL;
        if (!eq || eq == s->env[i]) return vp_bad(EINVAL);
    }
    for (i = 0; i < s->fdc; ++i) {
        if (!s->names[i] || !s->names[i][0] || strlen(s->names[i]) > 20) return vp_bad(EINVAL);
        for (j = 0; j < i; ++j) if (!strcmp(s->names[i], s->names[j])) return vp_bad(EINVAL);
    }
    return 0;
}
static inline int vp_measure_string(const char *value, size_t *size)
{
    size_t length = strnlen(value, VP_PAYLOAD_CAP + 1u);
    if (length > VP_PAYLOAD_CAP || *size > VP_PAYLOAD_CAP - 4u ||
        length > VP_PAYLOAD_CAP - *size - 4u) return vp_bad(E2BIG);
    *size += length + 4u; return 0;
}
static inline void vp_put_string(unsigned char **p, const char *value)
{
    size_t n = strlen(value); vp_store32(*p, (uint32_t)n); *p += 4;
    memcpy(*p, value, n); *p += n;
}
static inline int vp_encode(const vp_spawn_spec *s, unsigned char **output, size_t *length)
{
    size_t size = 12; uint32_t i; unsigned char *p;
    *output = NULL; *length = 0;
    if (vp_validate_spec(s) || vp_measure_string(s->home, &size) || vp_measure_string(s->bin, &size)) return -1;
    for (i = 0; i < s->argc; ++i) if (vp_measure_string(s->argv[i], &size)) return -1;
    for (i = 0; i < s->envc; ++i) if (vp_measure_string(s->env[i], &size)) return -1;
    for (i = 0; i < s->fdc; ++i) if (vp_measure_string(s->names[i], &size)) return -1;
    if (!(*output = (unsigned char *)malloc(size))) return vp_bad(ENOMEM);
    p = *output; vp_store32(p, s->argc); vp_store32(p + 4, s->envc); vp_store32(p + 8, s->fdc); p += 12;
    vp_put_string(&p, s->home); vp_put_string(&p, s->bin);
    for (i = 0; i < s->argc; ++i) vp_put_string(&p, s->argv[i]);
    for (i = 0; i < s->envc; ++i) vp_put_string(&p, s->env[i]);
    for (i = 0; i < s->fdc; ++i) vp_put_string(&p, s->names[i]);
    *length = size; return 0;
}
static inline int vp_get_string(const unsigned char **p, const unsigned char *end, const char **value)
{
    uint32_t size; char *copy;
    if ((size_t)(end - *p) < 4) return vp_bad(EPROTO);
    size = vp_load32(*p); *p += 4;
    if (size > (size_t)(end - *p) || memchr(*p, 0, size)) return vp_bad(EPROTO);
    if (!(copy = (char *)malloc((size_t)size + 1))) return vp_bad(ENOMEM);
    memcpy(copy, *p, size); copy[size] = 0; *p += size; *value = copy; return 0;
}
static inline int vp_decode(const unsigned char *data, size_t length, vp_spawn_spec *s)
{
    const unsigned char *p, *end; uint32_t i;
    memset(s, 0, sizeof(*s));
    if (length < 20 || length > VP_PAYLOAD_CAP) return vp_bad(EPROTO);
    p = data + 12; end = data + length;
    s->argc = vp_load32(data); s->envc = vp_load32(data + 4); s->fdc = vp_load32(data + 8);
    if (!s->argc || s->argc > VP_ITEMS_CAP || s->envc > VP_ITEMS_CAP || s->fdc > VP_FDS_CAP ||
        ((size_t)s->argc + s->envc + s->fdc) * 4u > length - 20) return vp_bad(EPROTO);
    s->argv = (const char **)calloc(s->argc, sizeof(char *));
    if (s->envc) s->env = (const char **)calloc(s->envc, sizeof(char *));
    if (s->fdc) s->names = (const char **)calloc(s->fdc, sizeof(char *));
    if (!s->argv || (s->envc && !s->env) || (s->fdc && !s->names)) { errno = ENOMEM; goto failed; }
    if (vp_get_string(&p, end, &s->home) || vp_get_string(&p, end, &s->bin)) goto failed;
    for (i = 0; i < s->argc; ++i) if (vp_get_string(&p, end, &s->argv[i])) goto failed;
    for (i = 0; i < s->envc; ++i) if (vp_get_string(&p, end, &s->env[i])) goto failed;
    for (i = 0; i < s->fdc; ++i) if (vp_get_string(&p, end, &s->names[i])) goto failed;
    if (p != end) { errno = EPROTO; goto failed; }
    if (vp_validate_spec(s)) goto failed;
    return 0;
failed:
    { int error = errno; vp_free_spec(s); return vp_bad(error); }
}
static inline int64_t vp_now_ms(void)
{
    struct timespec now; if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static inline int vp_wait(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t left = deadline - vp_now_ms(); struct pollfd p; int result;
        if (left <= 0) return vp_bad(ETIMEDOUT);
        p.fd = fd; p.events = events; p.revents = 0;
        result = poll(&p, 1, left > 2147483647 ? 2147483647 : (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) return vp_bad(ETIMEDOUT);
        if (p.revents & events) return 0;
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return vp_bad(ECONNRESET);
    }
}
static inline void vp_close_fds(vp_received_fds *fds)
{ size_t i; for (i = 0; i < fds->count; ++i) close(fds->items[i]); fds->count = 0; }
static inline int vp_connect_path(const char *path, int64_t deadline)
{
    struct sockaddr_un addr; int fd, result;
    if (!path || strlen(path) >= sizeof(addr.sun_path)) return vp_bad(ENAMETOOLONG);
    memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX; strcpy(addr.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    for (;;) {
        if (vp_now_ms() >= deadline) { close(fd); return vp_bad(ETIMEDOUT); }
        result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (!result || errno == EISCONN) return fd;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) { if (!vp_wait(fd, POLLOUT, deadline)) continue; }
        else if (errno == EINPROGRESS) {
            int error = 0; socklen_t size = sizeof(error);
            if (!vp_wait(fd, POLLOUT, deadline) && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size)) {
                if (!error) return fd;
                errno = error;
            }
        }
        { int error = errno; close(fd); return vp_bad(error); }
    }
}
/* SCM_RIGHTS belongs to the first successful byte, including after EINTR/EAGAIN. */
static inline int vp_send_bytes(int fd, const void *bytes, size_t length,
                                const int *fds, size_t count, int64_t deadline)
{
    size_t offset = 0; int rights_sent = 0;
    if (count > VP_FDS_CAP) return vp_bad(E2BIG);
    if (count && (!fds || !length)) return vp_bad(EINVAL);
    while (offset < length) {
        struct msghdr msg; struct iovec iov; ssize_t sent;
        union { unsigned char bytes[CMSG_SPACE(sizeof(int) * VP_FDS_CAP)]; struct cmsghdr align; } control;
        if (vp_wait(fd, POLLOUT, deadline)) return -1;
        memset(&msg, 0, sizeof(msg)); memset(&control, 0, sizeof(control));
        iov.iov_base = (void *)((const unsigned char *)bytes + offset); iov.iov_len = length - offset;
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        if (count && !rights_sent) {
            struct cmsghdr *cmsg; msg.msg_control = control.bytes;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * count); cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * count); memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * count);
        }
        sent = sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return sent ? -1 : vp_bad(EPIPE);
        rights_sent = 1; offset += (size_t)sent;
    }
    return 0;
}
/* Use recvmsg for every fragment, so late ancillary data is never discarded. */
static inline int vp_recv_bytes(int fd, void *bytes, size_t length, vp_received_fds *fds, int64_t deadline)
{
    size_t offset = 0;
    while (offset < length) {
        struct msghdr msg; struct iovec iov; struct cmsghdr *cmsg; ssize_t received; int bad = 0;
        union { unsigned char bytes[CMSG_SPACE(sizeof(int) * VP_FDS_CAP)]; struct cmsghdr align; } control;
        if (vp_wait(fd, POLLIN, deadline)) return -1;
        memset(&msg, 0, sizeof(msg)); memset(&control, 0, sizeof(control));
        iov.iov_base = (unsigned char *)bytes + offset; iov.iov_len = length - offset;
        msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
        received = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (received <= 0) return received ? -1 : vp_bad(ECONNRESET);
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            size_t count, i; const int *items;
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0)) {
                bad = 1; continue;
            }
            count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int); items = (const int *)CMSG_DATA(cmsg);
            for (i = 0; i < count; ++i) {
                if (fds->count < VP_FDS_CAP) fds->items[fds->count++] = items[i];
                else { close(items[i]); bad = 1; }
            }
        }
        if (bad || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return vp_bad(EPROTO);
        offset += (size_t)received;
    }
    return 0;
}
static inline int vp_send_request(int fd, const vp_spawn_spec *s, const int *fds, int64_t deadline)
{
    unsigned char *payload, *frame; size_t length; int result;
    if (s->fdc > VP_CLIENT_FDS_CAP) return vp_bad(E2BIG);
    if (vp_encode(s, &payload, &length)) return -1;
    frame = (unsigned char *)malloc(length + VP_FRAME_HEADER);
    if (!frame) { free(payload); return vp_bad(ENOMEM); }
    memcpy(frame, "VPBR", 4); vp_store32(frame + 4, 2); vp_store32(frame + 8, (uint32_t)length);
    memcpy(frame + VP_FRAME_HEADER, payload, length); free(payload);
    result = vp_send_bytes(fd, frame, length + VP_FRAME_HEADER, fds, s->fdc, deadline);
    free(frame); return result;
}
static inline int vp_recv_request_prefix(int fd, vp_spawn_spec *s, vp_received_fds *fds, int64_t deadline, const unsigned char prefix[4])
{
    unsigned char header[VP_FRAME_HEADER], *payload; uint32_t length; int result;
    memset(s, 0, sizeof(*s));
    memcpy(header, prefix, 4);
    if (memcmp(header, "VPBR", 4)) return vp_bad(EPROTONOSUPPORT);
    if (vp_recv_bytes(fd, header + 4, VP_FRAME_HEADER - 4, fds, deadline)) return -1;
    if (vp_load32(header + 4) != 2) return vp_bad(EPROTONOSUPPORT);
    length = vp_load32(header + 8);
    if (length < 20 || length > VP_PAYLOAD_CAP) return vp_bad(E2BIG);
    if (!(payload = (unsigned char *)malloc(length))) return vp_bad(ENOMEM);
    result = vp_recv_bytes(fd, payload, length, fds, deadline);
    if (!result) result = vp_decode(payload, length, s);
    free(payload);
    if (!result && (s->fdc != fds->count || s->fdc > VP_CLIENT_FDS_CAP)) {
        vp_free_spec(s); return vp_bad(EPROTO);
    }
    return result;
}
static inline int vp_recv_request(int fd, vp_spawn_spec *s, vp_received_fds *fds, int64_t deadline)
{
    unsigned char prefix[4];
    memset(s, 0, sizeof(*s));
    if (vp_recv_bytes(fd, prefix, 4, fds, deadline)) return -1;
    return vp_recv_request_prefix(fd, s, fds, deadline, prefix);
}
static inline int vp_recv_reply(int fd, int *pid, int *status, int64_t deadline)
{
    unsigned char reply[8]; vp_received_fds fds = {{0}, 0}; int result;
    result = vp_recv_bytes(fd, reply, sizeof(reply), &fds, deadline);
    if (fds.count) { vp_close_fds(&fds); return vp_bad(EPROTO); }
    if (result) return -1;
    *pid = (int32_t)vp_load32(reply); *status = (int32_t)vp_load32(reply + 4); return 0;
}
#endif

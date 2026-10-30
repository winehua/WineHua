/* Compile with the actual Wine readback implementation injected at SOURCE.
 * The fake GL and Wayland endpoints check ownership, pixels and GL state;
 * they do not measure a GPU or emulate its scheduling/performance. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL, LONG, GLint, EGLint;
typedef unsigned char BYTE;
typedef size_t SIZE_T;
typedef void *HWND, *EGLSurface, *EGLContext;
typedef struct { int left, top, right, bottom; } RECT;
#define TRUE 1
#define FALSE 0
#define GL_FALSE 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONTAINING_RECORD(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
enum { GL_MESA_pack_invert, GL_PIXEL_PACK_BUFFER_BINDING, GL_PACK_ALIGNMENT,
       GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
       GL_READ_FRAMEBUFFER_BINDING, GL_PACK_INVERT_MESA, GL_PIXEL_PACK_BUFFER,
       GL_READ_FRAMEBUFFER, GL_VIEWPORT, GL_BGRA, GL_UNSIGNED_BYTE,
       EGL_WIDTH, EGL_HEIGHT, EGL_NONE, WL_SHM_FORMAT_ARGB8888 };
#define EGL_NO_CONTEXT NULL
enum winehua_gl_stage { WINEHUA_GL_SWAP_ENTER, WINEHUA_GL_FLUSH,
    WINEHUA_GL_READBACK, WINEHUA_GL_CPU_COPY, WINEHUA_GL_SHM_COMMIT };

struct wl_buffer;
struct wl_buffer_listener { void (*release)(void *, struct wl_buffer *); };
struct wl_buffer { const struct wl_buffer_listener *listener; void *user; };
struct wl_proxy { int unused; };
struct client_surface { HWND hwnd; };
struct wayland_client_surface { struct client_surface client; void *wl_surface, *wp_viewport; };
struct wayland_shm_buffer {
    struct wl_buffer *wl_buffer;
    BYTE *allocation, *map_data;
    size_t map_size;
    BOOL busy;
};
struct opengl_drawable { struct client_surface *client; EGLSurface surface; int format; };
struct opengl_client_context { BOOL extensions[1]; };
struct opengl_context { struct opengl_client_context *client_context; };
static struct { struct opengl_context *glContext; } fake_teb;
#define NtCurrentTeb() (&fake_teb)
static struct opengl_client_context *opengl_client_context_from_client(struct opengl_client_context *c) { return c; }
static LONG InterlockedIncrement(LONG *v) { return ++*v; }
static LONG InterlockedDecrement(LONG *v) { return --*v; }
static LONG InterlockedExchange(LONG *v, LONG n) { LONG old = *v; *v = n; return old; }
static LONG InterlockedCompareExchange(LONG *v, LONG n, LONG expected) { LONG old = *v; if (old == expected) *v = n; return old; }
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static int winehua_gl_diag_mutex;
static unsigned long winehua_gl_releases, winehua_gl_drops, winehua_gl_commits;
static LONG winehua_gl_in_flight, winehua_gl_readbacks = 1, winehua_gl_zero_copy_presents = 1;
static struct { void *wl_display, *wl_event_queue; } process_wayland;
static struct { void *display; } platform;
static const void *winehua_present_surface_page;

static BOOL direct_enabled, diagnostics, zero_copy, fail_rgba, fail_shm;
static int width = 3, height = 5, pb_width = 3, pb_height = 5;
static unsigned frame, read_count, flush_count, swap_count, commit_count;
static unsigned live_buffers, rgba_allocations, invert_queries;
static size_t copied_bytes;
static GLint gl_state[32];
static struct wl_buffer *last_attached, *release_during_read;
static void *read_target;
static BOOL pack_invert_during_read;
static BOOL winehua_env_enabled(const char *name) {
    return !strcmp(name, "WINEHUA_GL_DIRECT_SHM_READBACK") ? direct_enabled : diagnostics;
}
static void winehua_wayland_diag(const char *fmt, ...) { (void)fmt; }
static uint64_t winehua_gl_stage_begin(enum winehua_gl_stage s, HWND h, LONG n) { (void)s; (void)h; (void)n; return 0; }
static void winehua_gl_stage_end(enum winehua_gl_stage s, uint64_t n) { (void)s; (void)n; }
static void winehua_gl_diag_idle(void) {}
static uint32_t winehua_prepare_present_surface(struct wayland_client_surface *c) { (void)c; return 7; }
static BOOL winehua_surface_zero_copy_ready(uint32_t id) { (void)id; return zero_copy; }
static void winehua_begin_present_surface(uint32_t id) { (void)id; }
static void winehua_finish_present_surface(void) {}
static void NtUserGetClientRect(HWND h, RECT *r, int dpi) { (void)h; (void)dpi; r->right = width; r->bottom = height; }
static int NtUserGetDpiForWindow(HWND h) { (void)h; return 96; }
static void *egl_config_for_format(int f) { (void)f; return NULL; }
static unsigned getpid(void) { return 1; }
static void wl_proxy_set_queue(struct wl_proxy *p, void *q) { (void)p; (void)q; }
static int wl_buffer_add_listener(struct wl_buffer *b, const struct wl_buffer_listener *l, void *user) { b->listener = l; b->user = user; return 0; }
static struct wayland_shm_buffer *wayland_shm_buffer_create(int w, int h, int format) {
    (void)format;
    if (fail_shm) return NULL;
    struct wayland_shm_buffer *b = calloc(1, sizeof(*b));
    assert(b);
    b->map_size = (size_t)w * h * 4;
    b->allocation = malloc(b->map_size + 16);
    b->wl_buffer = calloc(1, sizeof(*b->wl_buffer));
    assert(b->allocation && b->wl_buffer);
    memset(b->allocation, 0xa5, b->map_size + 16);
    b->map_data = b->allocation + 8;
    live_buffers++;
    return b;
}
static void check_guards(struct wayland_shm_buffer *b) {
    for (unsigned i = 0; i != 8; ++i) {
        assert(b->allocation[i] == 0xa5);
        assert(b->allocation[b->map_size + 8 + i] == 0xa5);
    }
}
static void wayland_shm_buffer_unref(struct wayland_shm_buffer *b) {
    check_guards(b);
    free(b->allocation); free(b->wl_buffer); free(b); live_buffers--;
}
static void release_buffer(struct wl_buffer *b) { b->listener->release(b->user, b); }
static void wl_surface_attach(void *surface, struct wl_buffer *b, int x, int y) { (void)surface; assert(!x && !y); last_attached = b; }
static void wl_surface_damage_buffer(void *s, int x, int y, int w, int h) { (void)s; assert(!x && !y && w == width && h == height); }
static void wp_viewport_set_destination(void *v, int w, int h) { (void)v; assert(w == width && h == height); }
static void wl_surface_commit(void *s) { (void)s; commit_count++; }
static void wl_display_flush(void *d) { (void)d; }
static void *tracked_realloc(void *p, size_t n) { rgba_allocations++; return fail_rgba ? NULL : realloc(p, n); }
static void *tracked_memcpy(void *dst, const void *src, size_t n) { copied_bytes += n; return memcpy(dst, src, n); }

static BOOL query_surface(void *d, EGLSurface s, EGLint key, EGLint *value) { (void)d; (void)s; *value = key == EGL_WIDTH ? pb_width : pb_height; return TRUE; }
static EGLContext current_context(void) { return (void *)(uintptr_t)1; }
static EGLSurface create_pbuffer(void *d, void *config, const EGLint *a) { (void)d; (void)config; pb_width = a[1]; pb_height = a[3]; return (void *)(uintptr_t)1; }
static BOOL make_current(void *d, EGLSurface draw, EGLSurface read, EGLContext c) { (void)d; (void)draw; (void)read; (void)c; return TRUE; }
static void destroy_surface(void *d, EGLSurface s) { (void)d; (void)s; }
static int egl_error(void) { return 0; }
static BOOL swap_buffers(void *d, EGLSurface s) { (void)d; (void)s; swap_count++; return TRUE; }
static void flush_gl(void) { flush_count++; }
static void get_integer(int key, GLint *value) {
    if (key == GL_VIEWPORT) { value[0] = value[1] = 0; value[2] = width; value[3] = height; return; }
    if (key == GL_PACK_INVERT_MESA) invert_queries++;
    *value = gl_state[key];
}
static void bind_buffer(int target, GLint value) { assert(target == GL_PIXEL_PACK_BUFFER); gl_state[GL_PIXEL_PACK_BUFFER_BINDING] = value; }
static void bind_framebuffer(int target, GLint value) { assert(target == GL_READ_FRAMEBUFFER); gl_state[GL_READ_FRAMEBUFFER_BINDING] = value; }
static void pixel_store(int key, GLint value) { gl_state[key] = value; }
static BYTE pixel(int x, int top_y, unsigned channel) {
    switch (channel) {
    case 0: return (BYTE)(x + frame);
    case 1: return (BYTE)(top_y + frame * 3);
    case 2: return (BYTE)(x ^ top_y ^ frame);
    default: return (BYTE)(0x80 | (frame & 0x7f));
    }
}
static void read_pixels(int x, int y, int w, int h, int format, int type, void *dst) {
    assert(!x && !y && w == width && h == height && format == GL_BGRA && type == GL_UNSIGNED_BYTE);
    assert(!gl_state[GL_PIXEL_PACK_BUFFER_BINDING] && !gl_state[GL_READ_FRAMEBUFFER_BINDING]);
    assert(gl_state[GL_PACK_ALIGNMENT] == 1 && !gl_state[GL_PACK_ROW_LENGTH]);
    assert(!gl_state[GL_PACK_SKIP_PIXELS] && !gl_state[GL_PACK_SKIP_ROWS]);
    read_target = dst;
    pack_invert_during_read = gl_state[GL_PACK_INVERT_MESA];
    for (int row = 0; row != h; ++row)
        for (int col = 0; col != w; ++col)
            for (unsigned ch = 0; ch != 4; ++ch)
                ((BYTE *)dst)[((size_t)row * w + col) * 4 + ch] =
                    pixel(col, pack_invert_during_read ? row : h - 1 - row, ch);
    read_count++;
    if (release_during_read) { struct wl_buffer *b = release_during_read; release_during_read = NULL; release_buffer(b); }
}
static const struct {
    BOOL (*p_eglQuerySurface)(void *, EGLSurface, EGLint, EGLint *);
    EGLContext (*p_eglGetCurrentContext)(void);
    EGLSurface (*p_eglCreatePbufferSurface)(void *, void *, const EGLint *);
    BOOL (*p_eglMakeCurrent)(void *, EGLSurface, EGLSurface, EGLContext);
    void (*p_eglDestroySurface)(void *, EGLSurface);
    int (*p_eglGetError)(void);
    BOOL (*p_eglSwapBuffers)(void *, EGLSurface);
    void (*p_glFlush)(void);
    void (*p_glGetIntegerv)(int, GLint *);
    void (*p_glBindBuffer)(int, GLint);
    void (*p_glBindFramebuffer)(int, GLint);
    void (*p_glPixelStorei)(int, GLint);
    void (*p_glReadPixels)(int, int, int, int, int, int, void *);
} gl_funcs = { query_surface, current_context, create_pbuffer, make_current,
    destroy_surface, egl_error, swap_buffers, flush_gl, get_integer,
    bind_buffer, bind_framebuffer, pixel_store, read_pixels }, *funcs = &gl_funcs;
#define egl (&platform)
#define realloc tracked_realloc
#define memcpy tracked_memcpy
/* SOURCE */
#undef realloc
#undef memcpy

static struct wayland_shm_buffer *attached_buffer(struct winehua_readback_state *s) {
    for (unsigned i = 0; i != ARRAY_SIZE(s->slots); ++i)
        if (s->slots[i].buffer->wl_buffer == last_attached) return s->slots[i].buffer;
    assert(0); return NULL;
}
static void check_pixels(struct wayland_shm_buffer *b) {
    assert(b->map_size == (size_t)width * height * 4);
    check_guards(b);
    for (int y = 0; y != height; ++y)
        for (int x = 0; x != width; ++x)
            for (unsigned ch = 0; ch != 4; ++ch)
                assert(b->map_data[((size_t)y * width + x) * 4 + ch] == pixel(x, y, ch));
}
static void present_and_check(struct winehua_readback_drawable *gl, BOOL direct, BOOL release) {
    GLint before[32];
    memcpy(before, gl_state, sizeof(before));
    size_t old_copies = copied_bytes;
    unsigned old_reads = read_count, old_flushes = flush_count, old_swaps = swap_count, old_commits = commit_count;
    assert(winehua_readback_present(&gl->base));
    assert(!memcmp(before, gl_state, sizeof(before)));
    assert(read_count == old_reads + 1 && flush_count == old_flushes + 1);
    assert(swap_count == old_swaps + 1 && commit_count == old_commits + 1);
    struct wayland_shm_buffer *b = attached_buffer(gl->state);
    assert((read_target == b->map_data) == direct);
    assert(pack_invert_during_read == direct);
    assert(copied_bytes - old_copies == (direct ? 0 : (size_t)width * height * 4));
    check_pixels(b);
    if (release) release_buffer(last_attached);
    frame++;
}
int main(void) {
    struct wayland_client_surface client = { { (void *)(uintptr_t)1 }, (void *)(uintptr_t)1, (void *)(uintptr_t)1 };
    struct opengl_client_context caps = { { TRUE } };
    struct opengl_context context = { &caps };
    struct winehua_readback_drawable gl = {0};
    fake_teb.glContext = &context;
    gl.base.client = &client.client;
    gl.base.surface = (void *)(uintptr_t)1;
    gl.state = winehua_readback_state_create(client.client.hwnd, width, height);
    assert(gl.state && live_buffers == 3);
    gl_state[GL_PACK_ALIGNMENT] = 8;
    gl_state[GL_PIXEL_PACK_BUFFER_BINDING] = 42;
    gl_state[GL_PACK_ROW_LENGTH] = 13;
    gl_state[GL_PACK_SKIP_PIXELS] = 2;
    gl_state[GL_PACK_SKIP_ROWS] = 7;
    gl_state[GL_READ_FRAMEBUFFER_BINDING] = 19;

    /* Disabled mode and unsupported/no-current contexts use the original path. */
    present_and_check(&gl, FALSE, TRUE);
    assert(!invert_queries);
    direct_enabled = TRUE;
    caps.extensions[0] = FALSE;
    present_and_check(&gl, FALSE, TRUE);
    assert(!invert_queries);
    caps.extensions[0] = TRUE;
    fake_teb.glContext = NULL;
    present_and_check(&gl, FALSE, TRUE);
    fake_teb.glContext = &context;
    context.client_context = NULL;
    present_and_check(&gl, FALSE, TRUE);
    context.client_context = &caps;

    /* Enabled path never reallocates the CPU scratch image or row-copies it. */
    free(gl.rgba); gl.rgba = NULL; gl.rgba_size = 0;
    unsigned allocations = rgba_allocations;
    for (unsigned i = 0; i != 1200; ++i) {
        gl_state[GL_PACK_INVERT_MESA] = (GLint)(i & 1);
        present_and_check(&gl, TRUE, TRUE);
        assert(gl.state->in_flight == 0 && gl.state->ref == 1);
    }
    assert(rgba_allocations == allocations && !gl.rgba);

    /* Own all slots: readback still completes without touching busy SHM. */
    struct wl_buffer *pending[3];
    for (unsigned i = 0; i != 3; ++i) { present_and_check(&gl, TRUE, FALSE); pending[i] = last_attached; }
    unsigned old_commits = commit_count, old_reads = read_count;
    size_t old_copies = copied_bytes;
    diagnostics = TRUE;
    assert(winehua_readback_present(&gl.base));
    diagnostics = FALSE;
    assert(read_count == old_reads + 1 && commit_count == old_commits);
    assert(copied_bytes == old_copies && !pack_invert_during_read);
    assert(winehua_gl_drops == 1 && winehua_gl_in_flight == 3);
    assert(gl_state[GL_PACK_INVERT_MESA] == 1 && gl.state->in_flight == 3);
    for (unsigned i = 0; i != 3; ++i) {
        struct wayland_shm_buffer *b = gl.state->slots[i].buffer;
        check_guards(b);
        assert(b->map_data[0] == (BYTE)(frame - 3 + i));
    }
    /* A release during fallback readback can be reused only by a CPU copy. */
    release_during_read = pending[0];
    present_and_check(&gl, FALSE, FALSE);
    assert(gl.state->in_flight == 3);
    pending[0] = last_attached;
    for (unsigned i = 0; i != 3; ++i) release_buffer(pending[i]);
    assert(gl.state->in_flight == 0 && gl.state->ref == 1);

    /* Realistic odd-width output and repeated resize keep pixel orientation. */
    width = 793; height = 351;
    assert(winehua_readback_present(&gl.base)); /* Rebind pbuffer, skip old frame. */
    for (unsigned i = 0; i != 240; ++i) present_and_check(&gl, TRUE, TRUE);
    assert(gl.rgba_size < (size_t)width * height * 4);
    present_and_check(&gl, TRUE, FALSE);
    struct wl_buffer *old_size = last_attached;
    struct winehua_readback_state *retained_state = gl.state;
    width = 1; height = 1;
    assert(winehua_readback_present(&gl.base));
    present_and_check(&gl, TRUE, TRUE);
    assert(retained_state->ref == 1 && live_buffers == 6);
    release_buffer(old_size);
    assert(live_buffers == 3);

    /* Allocation failures keep ownership/reference counts balanced. */
    direct_enabled = FALSE; gl_state[GL_PACK_INVERT_MESA] = FALSE;
    free(gl.rgba); gl.rgba = NULL; gl.rgba_size = 0;
    fail_rgba = TRUE;
    assert(!winehua_readback_present(&gl.base));
    assert(!gl.state->in_flight && gl.state->ref == 1);
    fail_rgba = FALSE;
    width = pb_width = 7; height = pb_height = 9; fail_shm = TRUE;
    assert(!winehua_readback_present(&gl.base));
    assert(!gl.state->in_flight && gl.state->ref == 1);
    fail_shm = FALSE;
    direct_enabled = TRUE;
    present_and_check(&gl, TRUE, TRUE);

    /* Zero-copy presentation remains ahead of both readback paths. */
    old_reads = read_count; old_commits = commit_count; zero_copy = TRUE;
    assert(winehua_readback_present(&gl.base));
    assert(read_count == old_reads && commit_count == old_commits);
    zero_copy = FALSE;
    diagnostics = TRUE;
    present_and_check(&gl, TRUE, TRUE);
    assert(winehua_gl_commits == 1 && winehua_gl_releases == 1);
    assert(winehua_gl_drops == 1 && winehua_gl_in_flight == 0);
    winehua_readback_state_unref(gl.state);
    free(gl.rgba);
    assert(live_buffers == 0);
    puts("PASS: 1440 alternating frames, pixels, GL state, saturation, resize, failures, zero-copy and ownership");
    return 0;
}

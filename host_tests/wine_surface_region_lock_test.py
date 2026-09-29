"""Exercise Wine's actual caller and region updater against a flush callback.

Run on a Linux host with gcc. The stubs model USER and surface lock ownership,
not the Wayland compositor or GPU. Both --baseline and --partial must reproduce
the complete lock cycle. USER is recursive, as in production.
"""
import argparse
import json
import hashlib
from pathlib import Path
import resource
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef int HWND;
typedef int HRGN;
typedef unsigned DWORD;
typedef unsigned UINT;
typedef int HICON;
typedef struct { int unused; } ICONINFO;
typedef int BOOL;
typedef struct { int left, top, right, bottom; } RECT;
struct window_rects { RECT window, client, visible; };
struct window_surface { pthread_mutex_t mutex; int refs, shapes, clips; UINT alpha_mask; RECT rect; };
typedef struct { struct window_surface *surface; DWORD dwExStyle, dwStyle;
    HWND parent; struct window_rects rects; int clip_clients, flags, has_icons;
    HICON hIcon, hIconSmall2, hIconSmall; RECT present_rect; } WND;
#define TRUE 1
#define FALSE 0
#define RGN_AND 1
#define WS_EX_LAYOUTRTL 0x400000
#define WND_DESKTOP ((WND *)1)
#define WND_OTHER_PROCESS ((WND *)2)
static pthread_mutex_t user_mutex;
static pthread_mutex_t win_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t phase_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t phase_cv = PTHREAD_COND_INITIALIZER;
static _Thread_local int user_depth;
static struct window_surface original = { PTHREAD_MUTEX_INITIALIZER, 1, 0, 0 };
static struct window_surface replacement = { PTHREAD_MUTEX_INITIALIZER, 1, 0, 0 };
static WND win = { &original, 0 };
static WND child;
static struct window_surface *tested_surface = &original;
static int race, phase, replace_surface, fail_shape, fail_clip, null_clip, mirrors;
static int caller, parent_case, server_error, no_surface, region_calls, pos_changed;
static int swap_surface, no_shape;
static int shape_deleted, clip_deleted;
static void lock_user(void) { pthread_mutex_lock(&user_mutex); user_depth++; }
static void unlock_user(void) { assert(user_depth > 0); user_depth--; pthread_mutex_unlock(&user_mutex); }
static void user_check_not_lock(void) { assert(user_depth == 0); }
static WND *get_win_ptr(HWND hwnd) { lock_user(); return hwnd == 43 ? &child : &win; }
static void release_win_ptr(WND *ptr) { assert(ptr == &win || ptr == &child); unlock_user(); }
static void window_surface_add_ref(struct window_surface *surface) { assert(surface->refs > 0); surface->refs++; }
static void window_surface_release(struct window_surface *surface) { assert(surface->refs > 0); surface->refs--; }
static int get_window_region(HWND hwnd, BOOL clip, HRGN *rgn, RECT *visible) {
    assert(hwnd == 42);
    region_calls++;
    if (!clip && race) {
        printf("REGION user_depth=%d\n", user_depth);
        pthread_mutex_lock(&phase_mutex);
        phase = 1; pthread_cond_broadcast(&phase_cv);
        while (phase < 2) pthread_cond_wait(&phase_cv, &phase_mutex);
        pthread_mutex_unlock(&phase_mutex);
    }
    if (!clip && replace_surface) {
        /* Another window update can replace the owner's reference while the
         * region request runs. The updater must keep the old surface alive. */
        assert(user_depth == 0);
        lock_user();
        assert(original.refs == 2);
        win.surface = &replacement;
        window_surface_release(&original);
        unlock_user();
    }
    if ((!clip && fail_shape) || (clip && fail_clip)) return 1;
    *visible = (RECT){10, 20, 110, 100};
    *rgn = clip ? (null_clip ? 0 : 22) : (no_shape ? 0 : 11);
    return 0;
}
static HRGN NtGdiCreateRectRgn(int l, int t, int r, int b) { (void)l; (void)t; assert(r == 100 && b == 80); return 33; }
static void NtGdiCombineRgn(HRGN d, HRGN a, HRGN b, int op) { assert(d && a && b && op == RGN_AND); }
static void NtUserMirrorRgn(HWND hwnd, HRGN rgn) { (void)hwnd; assert(rgn == 11); mirrors++; }
static void NtGdiOffsetRgn(HRGN rgn, int x, int y) { assert(rgn == 22 && x == -10 && y == -20); }
static void NtGdiDeleteObjectApp(HRGN rgn) {
    if (rgn == 11) { assert(!shape_deleted); shape_deleted++; }
    if (rgn == 22) { assert(!clip_deleted); clip_deleted++; }
}
static void window_surface_set_shape(struct window_surface *surface, HRGN rgn) {
    assert(surface == tested_surface && surface->refs > 0 && rgn == (no_shape ? 0 : 11));
    if (race) printf("UPDATER waiting_surface user_depth=%d\n", user_depth);
    pthread_mutex_lock(&surface->mutex);
    surface->shapes++;
    pthread_mutex_unlock(&surface->mutex);
}
static void window_surface_set_clip(struct window_surface *surface, HRGN rgn) {
    assert(user_depth == 0);
    assert(surface == tested_surface && surface->refs > 0 && rgn == (null_clip ? (no_shape ? 0 : 11) : 22));
    pthread_mutex_lock(&surface->mutex);
    surface->clips++;
    pthread_mutex_unlock(&surface->mutex);
}
static void *flush_callback(void *unused) {
    (void)unused;
    pthread_mutex_lock(&phase_mutex);
    while (phase < 1) pthread_cond_wait(&phase_cv, &phase_mutex);
    pthread_mutex_unlock(&phase_mutex);
    pthread_mutex_lock(&tested_surface->mutex);
    pthread_mutex_lock(&win_data_mutex);
    pthread_mutex_lock(&phase_mutex);
    puts("FLUSH owns_surface_and_win_data; requesting_USER");
    phase = 2; pthread_cond_broadcast(&phase_cv);
    pthread_mutex_unlock(&phase_mutex);
    /* Same lock sequence as wayland_surface_update_min_max in the device
     * stack: flush owns surface; querying window style acquires USER. */
    lock_user(); unlock_user();
    pthread_mutex_unlock(&win_data_mutex);
    pthread_mutex_unlock(&tested_surface->mutex);
    return NULL;
}
'''
CALLER_STUBS = r'''
/* Platform/server operations are stubs; the complete production caller below
 * retains its own branches, nested get/release pairs and surface ownership. */
typedef struct { RECT rcMonitor; } MONITORINFO;
#define TRACE(...) ((void)0)
#define GA_ROOT 1
#define COORDS_PARENT 1
#define COORDS_CLIENT 2
#define GWL_EXSTYLE 1
#define WS_VISIBLE 0x10000000
#define WS_THICKFRAME 0x40000
#define WS_MINIMIZE 0x20000000
#define WIN_CHILDREN_MOVED 1
#define SWP_FRAMECHANGED 0x20
#define SWP_NOREDRAW 0x8
#define SWP_NOCOPYBITS 0x100
#define SWP_HIDEWINDOW 0x80
#define SWP_SHOWWINDOW 0x40
#define SWP_STATECHANGED 0x8000
#define SWP_AGG_NOPOSCHANGE 0xf
#define WINE_SWP_RESIZABLE 0x10000
#define WINE_SWP_FULLSCREEN 0x20000
#define SET_WINPOS_PAINT_SURFACE 1
#define SET_WINPOS_LAYERED_WINDOW 2
#define SET_WINPOS_PIXEL_FORMAT 4
#define ICON_BIG 1
#define ICON_SMALL 0
#define GW_OWNER 4
static struct window_surface dummy_surface;
static UINT get_thread_dpi(void) { return 96; }
static HWND NtUserGetAncestor(HWND h, int f) { (void)h; (void)f; return 42; }
static UINT get_win_monitor_dpi(HWND h, UINT *raw) { (void)h; return *raw = 96; }
static UINT monitor_dpi_from_rect(RECT r, UINT d, UINT *raw) { (void)r; (void)d; return *raw = 96; }
static void get_window_rects(HWND h, int c, struct window_rects *r, UINT d) { (void)c; (void)d; *r = h == 43 ? child.rects : win.rects; }
static int IsRectEmpty(const RECT *r) { return !r || r->right <= r->left || r->bottom <= r->top; }
static int EqualRect(const RECT *a, const RECT *b) { return !memcmp(a, b, sizeof(*a)); }
static void OffsetRect(RECT *r, int x, int y) { r->left += x; r->right += x; r->top += y; r->bottom += y; }
static void SetRectEmpty(RECT *r) { memset(r, 0, sizeof(*r)); }
struct server_request { HWND handle, previous; UINT swp_flags, monitor_dpi, paint_flags; RECT window, client; };
struct server_reply { DWORD new_style, new_ex_style; HWND surface_win; };
#define SERVER_START_REQ(name) do { struct server_request request = {0}, *req = &request; struct server_reply response = {0, win->dwExStyle, 42}, *reply = &response;
#define SERVER_END_REQ } while (0)
#define wine_server_user_handle(h) (h)
#define wine_server_ptr_handle(h) (h)
#define wine_server_rectangle(r) (r)
static void wine_server_add_data(struct server_request *r, const void *d, size_t s) { (void)r; (void)d; (void)s; }
static int wine_server_call(struct server_request *r) { (void)r; assert(user_depth == 1); return server_error; }
static DWORD get_window_long(HWND h, int offset) { (void)h; (void)offset; lock_user(); unlock_user(); return 0; }
static void get_client_rect_rel(HWND h, int c, RECT *r, UINT d) { (void)h; (void)c; (void)d; SetRectEmpty(r); }
static void mirror_rect(const RECT *c, RECT *r) { (void)c; (void)r; }
static void invalidate_dce(WND *w, const RECT *r) { (void)w; (void)r; assert(user_depth == 1); }
static struct window_rects map_dpi_window_rects(struct window_rects r, UINT a, UINT b) { (void)a; (void)b; return r; }
static struct window_rects map_window_rects_virt_to_raw(struct window_rects r, UINT a) { (void)a; return r; }
static MONITORINFO monitor_info_from_rect(RECT r, UINT d) { (void)d; return (MONITORINFO){r}; }
static int is_fullscreen(const MONITORINFO *i, const RECT *r) { return EqualRect(&i->rcMonitor, r); }
static void register_window_surface(struct window_surface *a, struct window_surface *b) { (void)a; (void)b; assert(!user_depth); }
static void move_window_bits_surface(HWND h, const RECT *r, struct window_surface *s, const RECT *o, const RECT *v) { (void)h; (void)r; (void)s; (void)o; (void)v; assert(!user_depth); }
static void move_window_bits(HWND h, const struct window_rects *r, const RECT *v) { (void)h; (void)r; (void)v; assert(!user_depth); }
static HICON get_window_icon_info(HWND h, int k, HICON i, ICONINFO *info) { (void)h; (void)k; (void)info; return i; }
static HWND NtUserGetWindowRelative(HWND h, int f) { (void)h; (void)f; return 0; }
static HWND NtUserWindowFromPoint(int x, int y) { (void)x; (void)y; return 0; }
static struct window_surface *get_driver_window_surface(struct window_surface *s, UINT dpi) { (void)dpi; return s; }
static void update_client_surfaces(HWND h) { (void)h; assert(!user_depth); }
static void driver_move(HWND h, const struct window_rects *a, const struct window_rects *b, const RECT *v) { (void)h; (void)a; (void)b; (void)v; assert(!user_depth); }
static void driver_icons(HWND h, HICON a, ICONINFO *i, HICON b, ICONINFO *j) { (void)h; (void)a; (void)i; (void)b; (void)j; assert(!user_depth); }
static void driver_changed(HWND h, HWND a, HWND o, UINT f, const struct window_rects *r, struct window_surface *s) { (void)h; (void)a; (void)o; (void)f; (void)r; (void)s; assert(!user_depth); pos_changed++; }
static struct { void (*pMoveWindowBits)(HWND, const struct window_rects *, const struct window_rects *, const RECT *);
    void (*pSetWindowIcons)(HWND, HICON, ICONINFO *, HICON, ICONINFO *);
    void (*pWindowPosChanged)(HWND, HWND, HWND, UINT, const struct window_rects *, struct window_surface *);
} driver = { driver_move, driver_icons, driver_changed }, *user_driver = &driver;
'''
MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    setbuf(stdout, NULL);
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&user_mutex, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    caller = !strncmp(argv[1], "caller-", 7);
    swap_surface = strstr(argv[1], "swap") != NULL;
    no_shape = strstr(argv[1], "no-shape") != NULL;
    if (swap_surface) tested_surface = &replacement;
    parent_case = !strcmp(argv[1], "caller-parent-race");
    server_error = !strcmp(argv[1], "caller-server-error");
    no_surface = strstr(argv[1], "no-surface") != NULL;
    race = strstr(argv[1], "race") != NULL;
    replace_surface = !strcmp(argv[1], "replacement");
    fail_shape = strstr(argv[1], "shape-error") != NULL;
    fail_clip = strstr(argv[1], "clip-error") != NULL;
    null_clip = strstr(argv[1], "no-clip") != NULL;
    if (!strcmp(argv[1], "rtl")) win.dwExStyle = WS_EX_LAYOUTRTL;
    if (no_surface) win.surface = NULL;
    if (!strcmp(argv[1], "entry-contract")) lock_user();
    pthread_t worker;
    if (race) assert(!pthread_create(&worker, NULL, flush_callback, NULL));
    if (caller) {
        struct window_rects rects = {{10, 20, 110, 100}, {10, 20, 110, 100}, {10, 20, 110, 100}};
        RECT valid[2] = {{0}, {0}};
        struct window_surface *surface = parent_case ? NULL : (swap_surface ? &replacement : win.surface);
        /* A newly created replacement starts with its caller reference. */
        if (surface && !swap_surface) window_surface_add_ref(surface);
        assert(apply_window_pos(parent_case ? 43 : 42, 0, 0, surface, &rects, valid) == !server_error);
        if (surface) window_surface_release(surface);
        assert(pos_changed == !server_error);
    } else update_surface_region(42);
    if (race) assert(!pthread_join(worker, NULL));
    assert(user_depth == 0 && !pthread_mutex_trylock(&user_mutex));
    pthread_mutex_unlock(&user_mutex);
    assert(original.refs == ((replace_surface || swap_surface) ? 0 : 1));
    assert(replacement.refs == 1);
    if (swap_surface) {
        assert(win.surface == &replacement && !original.shapes && !original.clips);
    } else assert(!replacement.shapes && !replacement.clips);
    if (no_surface || fail_shape || server_error) {
        assert(tested_surface->shapes == 0 && tested_surface->clips == 0 && !shape_deleted && !clip_deleted);
    } else {
        assert(tested_surface->shapes == 1 && shape_deleted == !no_shape);
        assert(tested_surface->clips == (fail_clip ? 0 : 1));
        assert(clip_deleted == ((!fail_clip && !null_clip) ? 1 : 0));
    }
    assert(mirrors == (win.dwExStyle ? 1 : 0));
    puts("PASS");
    return 0;
}
'''

def extract(path, function, following):
    text = path.read_text()
    begin = text.index(function)
    end = text.index(following, begin)
    return text[begin:end]

def compile_case(source, build, name):
    cfile, binary = build / (name + '.c'), build / name
    cfile.write_text(HEADER + CALLER_STUBS +
        extract(source, 'static void update_surface_region( HWND hwnd )', '\nstatic RECT get_visible_rect') +
        extract(source, 'static BOOL apply_window_pos( HWND hwnd,', '\nstatic BOOL expose_window_surface') + MAIN)
    subprocess.run(['gcc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-pthread', str(cfile), '-o', str(binary)], check=True)
    return binary

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--partial', type=Path)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    build = ROOT / 'build/direct-surface-lock-20260929'
    build.mkdir(parents=True, exist_ok=True)
    binary = compile_case(ROOT / 'thirdparty/wine-valve/dlls/win32u/window.c', build, 'region-fixed')
    result = {'scope': 'Complete production apply_window_pos + update_surface_region extracted verbatim; recursive USER, deterministic Linux lock/lifetime harness with platform stubs, not device or GPU validation.', 'cases': {}}
    for case in ('race', 'replacement', 'shape-error', 'clip-error', 'no-clip', 'rtl', 'no-surface',
                 'caller-race', 'caller-parent-race', 'caller-shape-error', 'caller-clip-error',
                 'caller-no-surface', 'caller-server-error', 'entry-contract',
                 'no-shape', 'no-shape-no-clip', 'caller-swap-race', 'caller-swap-clip-error'):
        run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=5)
        result['cases'][case] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
        assert run.returncode == (-signal.SIGABRT if case == 'entry-contract' else 0), (case, run.stderr)
    for name, source, depth in [('baseline', args.baseline, 2), ('partial', args.partial, 1)]:
        if not source:
            continue
        baseline = compile_case(source, build, 'region-' + name)
        try:
            run = subprocess.run([str(baseline), 'caller-race'], capture_output=True, text=True, timeout=2)
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b'').decode()
            assert f'REGION user_depth={depth}' in output, output
            assert 'FLUSH owns_surface_and_win_data; requesting_USER' in output, output
            assert f'UPDATER waiting_surface user_depth={depth}' in output, output
            result[name + 'Race'] = {'result': 'cycle reproduced; killed at 2 seconds', 'stdout': output,
                                    'sourceSha256': hashlib.sha256(source.read_bytes()).hexdigest()}
        else:
            raise AssertionError(('Baseline did not reproduce the lock cycle', run.returncode, run.stderr))
    result_name = 'host-regression-with-baseline.json' if args.baseline else 'host-regression.json'
    (build / result_name).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))

if __name__ == '__main__':
    main()

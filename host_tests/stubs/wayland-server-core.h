#pragma once
// Only resource identity/user-data are mocked. Tests compile the production
// compositor, input resolver, toplevel manager and pixel blitter unchanged.
#include <cstdint>
#include <cstddef>
struct wl_client;
struct wl_display;
struct wl_global;
struct wl_event_loop;
struct wl_event_source;
struct wl_list { wl_list* prev; wl_list* next; };
inline void wl_list_init(wl_list* l) { l->prev = l->next = l; }
inline void wl_list_remove(wl_list* l) { l->prev->next = l->next; l->next->prev = l->prev; }
struct wl_listener { wl_list link; void (*notify)(wl_listener*, void*); };
struct wl_resource { void* userData = nullptr; wl_client* client = nullptr; };
// Scene tests have fixed resource lifetimes. Destruction notifications are
// exercised separately by the real-libwayland XDG/subsurface ASan tests.
inline void wl_resource_add_destroy_listener(wl_resource*, wl_listener* l) { wl_list_init(&l->link); }
inline void* wl_resource_get_user_data(wl_resource* resource) { return resource->userData; }
inline wl_client* wl_resource_get_client(wl_resource* resource) { return resource->client; }
inline int wl_resource_get_version(wl_resource*) { return 8; }
using wl_fixed_t = int32_t;
inline double wl_fixed_to_double(wl_fixed_t v) { return v / 256.0; }
inline wl_fixed_t wl_fixed_from_double(double v) { return static_cast<int32_t>(v * 256.0); }
inline wl_fixed_t wl_fixed_from_int(int v) { return v * 256; }
struct wl_array { size_t size = 0; size_t alloc = 0; void* data = nullptr; };
inline void wl_array_init(wl_array* a) { *a = {}; }
inline void wl_array_release(wl_array*) {}
void wl_resource_destroy(wl_resource*);
#define WL_EVENT_READABLE 1
wl_event_loop* wl_display_get_event_loop(wl_display*);
wl_event_source* wl_event_loop_add_fd(wl_event_loop*, int, uint32_t,
                                     int (*)(int, uint32_t, void*), void*);
int wl_event_source_remove(wl_event_source*);
void wl_display_flush_clients(wl_display*);
#include <sys/types.h>
void wl_client_get_credentials(wl_client*, pid_t*, uid_t*, gid_t*);

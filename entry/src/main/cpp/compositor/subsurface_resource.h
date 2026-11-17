#pragma once
#include <wayland-server-core.h>

// Owned by wl_subsurface, accessed only on the Wayland event thread. Protocol
// destruction can occur after the child wl_surface has already been freed.
struct SubsurfaceResource {
    wl_listener childDestroyed{}; // first member for notify's container cast
    wl_resource* child = nullptr;

    explicit SubsurfaceResource(wl_resource* surface) : child(surface) {
        childDestroyed.notify = [](wl_listener* listener, void*) {
            auto* data = reinterpret_cast<SubsurfaceResource*>(listener);
            data->child = nullptr;
            wl_list_remove(&listener->link);
            wl_list_init(&listener->link);
        };
        wl_resource_add_destroy_listener(surface, &childDestroyed);
    }
    ~SubsurfaceResource() { wl_list_remove(&childDestroyed.link); }
    SubsurfaceResource(const SubsurfaceResource&) = delete;
    SubsurfaceResource& operator=(const SubsurfaceResource&) = delete;
};

inline wl_resource* SubsurfaceChild(wl_resource* subsurface) {
    auto* data = static_cast<SubsurfaceResource*>(wl_resource_get_user_data(subsurface));
    return data ? data->child : nullptr;
}

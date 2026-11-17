#pragma once
#include <wayland-server-core.h>

// Event-thread-only weak link. libwayland can destroy resources in any order
// when a client disconnects; a protocol role must never retain a freed object.
struct ResourceWatch {
    wl_listener listener{}; // first member for the notify container cast
    wl_resource** slot;

    explicit ResourceWatch(wl_resource*& value) : slot(&value) {
        wl_list_init(&listener.link);
        listener.notify = [](wl_listener* listener, void*) {
            auto* watch = reinterpret_cast<ResourceWatch*>(listener);
            *watch->slot = nullptr;
            wl_list_remove(&listener->link);
            wl_list_init(&listener->link);
        };
    }
    void Set(wl_resource* value) {
        wl_list_remove(&listener.link);
        wl_list_init(&listener.link);
        *slot = value;
        if (value) wl_resource_add_destroy_listener(value, &listener);
    }
    ~ResourceWatch() { wl_list_remove(&listener.link); }
    ResourceWatch(const ResourceWatch&) = delete;
    ResourceWatch& operator=(const ResourceWatch&) = delete;
};

#include "wine/guest_gl_loader_policy.h"
#include <cassert>
int main() {
    using winehua::UsesSystemZinkLoader;
    assert(UsesSystemZinkLoader("zink", "/system/lib64/libvulkan.so"));
    assert(!UsesSystemZinkLoader("virpipe", "/system/lib64/libvulkan.so"));
    assert(!UsesSystemZinkLoader("zink", "libvulkan.so.1"));
    assert(!UsesSystemZinkLoader(nullptr, "/system/lib64/libvulkan.so"));
    assert(!UsesSystemZinkLoader("zink", nullptr));
}

#include "graphics/surface_queue_recovery.h"
#include <cassert>

int main()
{
    using namespace winehua;
    static_assert(kSurfaceBufferTimeoutMs > 0 && kSurfaceBufferTimeoutMs <= 20,
                  "Producer dequeue must be bounded without immediate no-buffer starvation");
    SurfaceQueueRecovery recovery;
    assert(!recovery.Pending());
    assert(!recovery.Deferred(0));
    recovery.Request(100);
    assert(recovery.Pending());
    assert(recovery.Deferred(100));
    assert(recovery.Deferred(100 + kSurfaceRecoveryDelayNs - 1));
    assert(!recovery.Deferred(100 + kSurfaceRecoveryDelayNs));
    // Becoming due permits a rebuild but does not pretend it has succeeded.
    assert(recovery.Pending());
    const uint64_t retry = recovery.RetryAfterNs();
    recovery.Request(retry);
    assert(recovery.Deferred(retry));
    assert(recovery.RetryAfterNs() == retry + kSurfaceRecoveryDelayNs);
    recovery.Reset();
    assert(!recovery.Pending());
    assert(!recovery.Deferred(0));
    assert(recovery.RetryAfterNs() == 0);
    // A new target/context must not inherit an old surface's cooldown.
    recovery.Request(1000000000);
    recovery.Reset();
    assert(!recovery.Deferred(1000000001));
}

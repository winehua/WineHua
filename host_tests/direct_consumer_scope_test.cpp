#include "direct/direct_consumer_scope.h"
#include <cassert>
#include <cstdio>

int main() {
    using winehua::direct::DirectConsumerScopes;
    DirectConsumerScopes scopes;
    int a, b, root, late;
    assert(!scopes.Register(nullptr, 1));
    assert(scopes.OwnerFor(1) == nullptr);
    assert(scopes.Register(&a, 1));
    assert(scopes.Register(&b, 2));
    assert(scopes.OwnerFor(1) == &a && scopes.OwnerFor(2) == &b);
    assert(scopes.OwnerFor(3) == nullptr);
    assert(!scopes.Register(&late, 1));
    assert(!scopes.Register(&root, 0));
    assert(scopes.Register(&a, 1));
    assert(!scopes.Register(&a, 2));
    scopes.Remove(&late);
    assert(scopes.OwnerFor(1) == &a);
    scopes.Remove(&a);
    assert(scopes.OwnerFor(1) == nullptr && scopes.OwnerFor(2) == &b);
    assert(scopes.Register(&late, 1));
    scopes.Remove(&a);
    assert(scopes.OwnerFor(1) == &late);
    scopes.Remove(&late); scopes.Remove(&b);
    assert(scopes.Register(&root, 0));
    assert(scopes.OwnerFor(1) == &root && scopes.OwnerFor(99) == &root);
    assert(!scopes.Register(&a, 1));
    scopes.Remove(&root);
    assert(scopes.Register(&a, 2) && scopes.OwnerFor(2) == &a);
    std::puts("PASS Direct queue scope: managed isolation, root exclusion, identity and late teardown");
}

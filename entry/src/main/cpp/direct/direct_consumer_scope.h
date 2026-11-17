#pragma once
#include <cstdint>
#include <unordered_map>

namespace winehua::direct {
// Called under the controller mutex. A queue has exactly one consumer: either
// the desktop (scope 0) or its own managed window. Release GPU frames before
// unregistering; rejected/late callbacks cannot steal or remove another owner.
class DirectConsumerScopes {
public:
    bool Register(const void* owner, uint32_t scope) {
        if (!owner) return false;
        for (const auto& [id, current] : owners_) {
            if (current == owner) return id == scope;
            if (!id || !scope || id == scope) return false;
        }
        owners_.emplace(scope, owner);
        return true;
    }
    void Remove(const void* owner) {
        for (auto it = owners_.begin(); it != owners_.end();)
            if (it->second == owner) it = owners_.erase(it); else ++it;
    }
    const void* OwnerFor(uint32_t toplevel) const {
        auto it = owners_.find(0);
        if (it == owners_.end()) it = owners_.find(toplevel);
        return it == owners_.end() ? nullptr : it->second;
    }
private:
    std::unordered_map<uint32_t, const void*> owners_;
};
} // namespace winehua::direct

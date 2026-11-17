#pragma once
#include "compositor/frame/direct_pass_policy.h"

// Replace renderer discovery, not compositor capability decisions.
class HostDirectPassPolicy : public winehua::DirectPassPolicy {
public:
    uint32_t bits = winehua::kDirectPassCapabilitiesAll;
    uint32_t DirectPassCapabilities() const override { return bits; }
};
class PluginManager {
public:
    static PluginManager* GetInstance() { static PluginManager manager; return &manager; }
    HostDirectPassPolicy* GetRendererForToplevel(uint32_t) { return &policy; }
    HostDirectPassPolicy policy;
};

#pragma once

#include <cstdint>
#include <string>

namespace winehua {

struct VirglHostConfig
{
    std::string helperPath;
    std::string socketPath;
    std::string libraryPath;
    std::string syncMode;
    std::string logPath;
    std::string shadowMode;
    std::string shadowTrace;
    std::string presentMode;
    std::string shadowMergeRanges;
    std::string descriptorUpdateSerialize;
    std::string gpuUploadWait;
};

struct VirglHostLaunchConfig
{
    std::string entryParams;
    uint64_t fingerprint = 0;
    bool forwardPerfSummary = false;
};

bool ValidateVirglHostConfig(const VirglHostConfig& config, std::string* error = nullptr);
uint64_t FingerprintVirglHostConfig(const VirglHostConfig& config);
bool BuildVirglHostLaunchConfig(const VirglHostConfig& config,
                                VirglHostLaunchConfig* launch,
                                std::string* error = nullptr);

} // namespace winehua

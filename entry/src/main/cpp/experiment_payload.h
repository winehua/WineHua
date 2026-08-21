#pragma once

#include <string>
#include <vector>

namespace winehua {

struct ExperimentArtifact {
    std::string name;
    std::string sha256;
};

bool StageExperimentPayload(const std::string& experimentId,
                            const std::vector<ExperimentArtifact>& artifacts,
                            const std::string& prefixMode,
                            const std::string& sourceUrl,
                            std::string* errorMessage);

} // namespace winehua

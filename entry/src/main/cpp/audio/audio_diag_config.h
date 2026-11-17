#ifndef WINEHUA_AUDIO_DIAG_CONFIG_H
#define WINEHUA_AUDIO_DIAG_CONFIG_H

#include "audio/audio_pcm_metrics.h"

namespace winehua {

// Diagnostic-only. Default G00 is bit-exact with the B3 product path.
// Change this constant and rebuild Host C++ to run G10/G01/G11.
#ifndef WINEHUA_AUDIO_DIAG_GAIN_PROFILE
constexpr AudioDiagGainProfile kAudioDiagGainProfile = AudioDiagGainProfile::G00;
#else
constexpr AudioDiagGainProfile kAudioDiagGainProfile =
    static_cast<AudioDiagGainProfile>(WINEHUA_AUDIO_DIAG_GAIN_PROFILE);
#endif

// Diagnostic-only. Do not enable for product builds.
constexpr bool kKeepRendererPhysicallyStartedForB2 = false;

// Diagnostic-only. Arm 2 s Ring/output dumps when started renderers go 1→2.
constexpr bool kAudioDiagP2Capture = false;
constexpr uint32_t kAudioDiagP2MaxCaptures = 4;

#ifndef WINEHUA_AUDIO_GIT_COMMIT
#define WINEHUA_AUDIO_GIT_COMMIT "unknown"
#endif

constexpr const char* kAudioDiagGitCommit = WINEHUA_AUDIO_GIT_COMMIT;

} // namespace winehua

#endif

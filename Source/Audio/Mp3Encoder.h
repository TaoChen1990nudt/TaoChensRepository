#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace atomic::mp3
{

//==============================================================================
/**
    Phase 52: MP3 output.

    There is no encoder in JUCE — `MP3AudioFormat` decodes only, and
    `LAMEEncoderAudioFormat` needs a LAME shared library shipped next to the
    executable, which this project does not do. On Windows the operating system
    already contains an MP3 encoder (the Media Foundation sink writer), so that
    is what this uses: no third-party binary to bundle, no extra licence, and a
    file every other program can read.

    `encode` returns an EMPTY string on success and a user-facing sentence on
    failure. Callers show that sentence; there is no silent fallback to WAV,
    because a file named .mp3 that is really PCM is a trap for whoever opens it
    next.
*/

inline constexpr int minBitrateKbps = 128;
inline constexpr int maxBitrateKbps = 320;

/** The encoder accepts a range, not a menu: 128..320, in kbps. */
int clampBitrate (int kbps) noexcept;

/** False on platforms whose MP3 story is "install something first". The export
    dialog greys the format out rather than letting the click fail. */
bool isAvailable() noexcept;

juce::String encode (const juce::File& destination,
                     const juce::AudioBuffer<float>& buffer,
                     double sampleRate,
                     int bitrateKbps);

} // namespace atomic::mp3

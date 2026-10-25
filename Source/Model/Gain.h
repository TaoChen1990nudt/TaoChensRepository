#pragma once

#include <cmath>

namespace atomic::gain
{

/** dB -> linear, with a floor that means "silent".

    Deliberately not juce::Decibels: that lives in juce_audio_basics, and the
    model layer must not depend on an audio module just to convert a fader
    value. Same reasoning that kept MidiMessage out of Phase 1.
*/
inline float fromDb (float db, float floorDb = -48.0f) noexcept
{
    if (db <= floorDb)
        return 0.0f;

    return std::pow (10.0f, db / 20.0f);
}

inline float toDb (float linearGain, float floorDb = -48.0f) noexcept
{
    if (linearGain <= 0.0f)
        return floorDb;

    return 20.0f * std::log10 (linearGain);
}

} // namespace atomic::gain

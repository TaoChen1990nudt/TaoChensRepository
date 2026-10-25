#pragma once

#include <juce_core/juce_core.h>

namespace atomic
{

/** juce::String(const char*) treats its input as ASCII (CharPointer_ASCII),
    so any non-ASCII narrow literal silently becomes mojibake. Every Chinese
    string in this project must be wrapped in this helper. */
inline juce::String utf8 (const char* utf8Text)
{
    return juce::String (juce::CharPointer_UTF8 (utf8Text));
}

/** Widely used separator, kept in one place so copy stays consistent. */
inline juce::String separatorDot()
{
    return utf8 ("  ·  ");
}

/** MIDI 60 == C4. Local helper so the UI modules do not have to pull in
    juce_audio_basics just to label a piano key. */
inline juce::String noteName (int pitch)
{
    static const char* const names[] { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };

    return juce::String (names[juce::jlimit (0, 11, pitch % 12)])
             + juce::String (pitch / 12 - 1);
}

} // namespace atomic

#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace atomic
{

//==============================================================================
/**
    Phase 24: one published parameter of a HOSTED instrument, as the editor
    needs to see it.

    The internal oscillator has exactly three automatable things (level, pan,
    pitch) and they are an enum in NoteEnvelope.h. A VST3 has however many the
    vendor decided on, named by the vendor, so the menu cannot be an enum: it
    has to be a list, asked for at the moment the menu opens.

    `index` is the position in `AudioProcessor::getParameters()` — the same
    index the project file stores and the same index the audio thread uses to
    find the parameter again. A name is for the human; an index is for us.

    Message thread only. Building this list means touching a live plugin, which
    is exactly what the audio thread must never do.
*/
struct PluginParamInfo
{
    int          index        = -1;
    juce::String name;
    float        defaultValue = 0.0f;   // normalised 0..1, for "reset to neutral"
};

/** `slotId` -> that synth's parameters. Returns empty for an internal synth, an
    unknown slot, or a host that cannot enumerate anything.

    A std::function rather than a PluginHost* member: the roll is a pure editor
    and the plugin host belongs to the session, so the wiring stays in one place
    (MainComponent) instead of every component knowing about VST3. */
using PluginParamLister = std::function<std::vector<PluginParamInfo> (int slotId)>;

} // namespace atomic

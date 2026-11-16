#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "Audio/MidiInputHub.h"

namespace atomic
{

//==============================================================================
/** What the person chose the last time they used the MIDI page.

    This is **intent, not a device census**. A keyboard that was unplugged at
    launch is still in here: the next start has to go looking for it again.
    Erasing a binding because `MidiInput::openDevice` failed once would turn a
    loose USB cable into a permanent forgetting.

    Kept next to the hub (rather than in Model/) because the value it stores is
    the hub's own state: the two switches and the ports it was asked to open.
    The project file does not carry any of it — unplugging a keyboard must not
    make a saved song dirty.
*/
struct MidiPreferences
{
    MidiSources                    sources;
    int                            typingOctave = 0;
    std::vector<MidiDeviceBinding> devices;

    bool operator== (const MidiPreferences& other) const noexcept
    {
        if (! (sources == other.sources) || typingOctave != other.typingOctave
            || devices.size() != other.devices.size())
            return false;

        for (size_t i = 0; i < devices.size(); ++i)
            if (! (devices[i] == other.devices[i]))
                return false;

        return true;
    }

    bool operator!= (const MidiPreferences& other) const noexcept { return ! operator== (other); }
};

/** `userApplicationDataDirectory/AtomicDAW/midi-settings.xml`. Separate from
    the project and from the plugin cache: it is neither a song nor a scan. */
juce::File getDefaultMidiPreferencesFile();

/** A missing, truncated or foreign file is the factory state, not an error
    the caller has to handle. Both readers and writers are file-only: no global
    state, so the self-test can hand them a temp file and prove the round trip. */
MidiPreferences loadMidiPreferences (const juce::File& file);
bool            saveMidiPreferences (const juce::File& file, const MidiPreferences& preferences);

} // namespace atomic

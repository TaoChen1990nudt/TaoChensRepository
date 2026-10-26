#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "Audio/MidiInputHub.h"
#include "Audio/PluginHost.h"

namespace atomic
{

//==============================================================================
/**
    The settings window behind the gear button.

    Three tabs, and each one is a real page rather than a placeholder:

    - **音频设置** is JUCE's own device selector: driver, output device, sample
      rate, buffer size. It talks to the same AudioDeviceManager the transport
      does, so a change here is audible immediately.
    - **MIDI 设置** decides what plays the synth. Two switches, not a choice:
      the external controller and the computer keyboard (FL Studio's layout) can
      both be on, and the typing keyboard is on out of the box. It owns no
      device — the MidiInputHub does, because a port can only be opened once and
      the thing that opens it has to be the thing that makes the sound.
    - **插件管理器** lists the scanned VST3s, split into instruments and effects
      because a synth in an insert slot and an effect in the rack are both
      useless. Double-click adds it in the right place.
*/
class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (PluginHost& pluginHost,
                    juce::AudioDeviceManager& deviceManager,
                    MidiInputHub& midiHub);
    ~SettingsWindow() override;

    /** 0 = audio, 1 = MIDI, 2 = plugins. */
    void showPage (int tabIndex);

    /** 3. Read by the self-test, so "three tabs" is a fact and not a comment. */
    int getNumPages() const;

    /** Re-reads the plugin library before the window appears. */
    void refresh();

    /** Renders one tab to an image without showing the window. A headless run
        cannot photograph a desktop window, the same reason the mixer has one. */
    juce::Image createPageSnapshot (int tabIndex, int width = 820, int height = 520);

    void closeButtonPressed() override { setVisible (false); }

    /** Double-clicked a plugin in the manager. The host decides where it goes. */
    std::function<void (const juce::PluginDescription&)> onAddInstrument;
    std::function<void (const juce::PluginDescription&)> onAddEffect;

    /** Phase 40: the user flipped one of the two sources on or off. The host
        applies it — it is the one that has to release whatever the source that
        went away was holding, and the one that decides where notes go. */
    std::function<void (MidiSources)> onMidiSourcesChanged;

    /** Phase 39: what the played keys will sound, for the page's own caption.
        A function rather than a string so it cannot go stale while the window
        is open. */
    std::function<juce::String()> getLiveTargetName;

private:
    class Panel;

    Panel* panel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

} // namespace atomic

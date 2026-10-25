#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

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
    - **MIDI 设置** lists the input devices, opens the ones you tick, and shows
      what is arriving on them. Notes are NOT yet recorded to the timeline —
      the monitor is there so "is my keyboard even connected" stops being a
      guess.
    - **插件管理器** lists the scanned VST3s, split into instruments and effects
      because a synth in an insert slot and an effect in the rack are both
      useless. Double-click adds it in the right place.
*/
class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (PluginHost& pluginHost, juce::AudioDeviceManager& deviceManager);
    ~SettingsWindow() override;

    /** 0 = audio, 1 = MIDI, 2 = plugins. */
    void showPage (int tabIndex);

    /** 3. Read by the self-test, so "three tabs" is a fact and not a comment. */
    int getNumPages() const;

    /** Re-reads the plugin library before the window appears. */
    void refresh();

    /** Renders one tab to an image without showing the window. A headless run
        cannot photograph a desktop window, the same reason the mixer has one. */
    juce::Image createPageSnapshot (int tabIndex, int width = 820, int height = 500);

    void closeButtonPressed() override { setVisible (false); }

    /** Double-clicked a plugin in the manager. The host decides where it goes. */
    std::function<void (const juce::PluginDescription&)> onAddInstrument;
    std::function<void (const juce::PluginDescription&)> onAddEffect;

private:
    class Panel;

    Panel* panel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

} // namespace atomic

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>

#include "Model/MixerModel.h"

namespace atomic
{

//==============================================================================
/**
    A hosted VST3's own editor, wrapped in a desktop window.

    Why the editor is created here and not in the rack: an AudioProcessorEditor
    belongs to its processor, and a processor can only have one. Whoever opens
    the window owns the editor for as long as the window lives, which is exactly
    the lifetime this class has.

    Closing must go back through the host (`onCloseRequest`) instead of deleting
    `this`: the host owns the window, and a window that deletes itself inside
    closeButtonPressed() is a use-after-free waiting for the next repaint.
*/
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioPluginInstance& processor,
                        const juce::String& pluginName,
                        int instanceIdToShow);

    ~PluginEditorWindow() override;

    /** Handed back to MainComponent, which erases this window asynchronously. */
    std::function<void()> onCloseRequest;

    int getInstanceId() const noexcept { return instanceId; }

    /** Phase 42: the host's key forwarder.

        A plugin window is a different top-level component, so a key pressed here
        never walks up to the main window: 空格 stopped starting the transport
        and the computer keyboard stopped playing notes the moment the user
        clicked the synth's own UI. The listener goes on the window AND on the
        content — the content is the plugin's editor, and a key listener there
        runs before the editor's own handler. */
    void setKeyForwarder (juce::KeyListener* listener);

    /** True when a forwarder was installed. The self-test asks, because "the
        keys work" is otherwise only observable with a real keyboard. */
    bool hasKeyForwarder() const noexcept { return keyForwarder != nullptr; }

    void closeButtonPressed() override;

private:
    juce::AudioProcessorEditor* editor = nullptr;
    juce::KeyListener*          keyForwarder = nullptr;
    int instanceId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditorWindow)
};

//==============================================================================
/**
    The built-in effects (low pass, drive, delay, air) have no plugin GUI. This
    is theirs: the same parameters the rack strip exposes, at a size where a
    mouse can actually aim at them.

    It writes straight through MixerModel, so the strip and this window are two
    views of one number — dragging here moves the bar there on the next repaint.
*/
class InsertEditorContent final : public juce::Component,
                                  private juce::Timer
{
public:
    InsertEditorContent (MixerModel& mixerModel, int channel, int slot);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int preferredWidth  = ui::px (420);
    static constexpr int preferredHeight = ui::px (168);

private:
    void timerCallback() override;

    juce::Rectangle<int> getParamRowBounds (int index) const;
    juce::Rectangle<int> getBarBounds (int index) const;
    juce::Rectangle<int> getPowerBounds() const;

    /** How many parameters this effect has (delay is the only one with two). */
    int getNumParams() const;
    float getParam (int index) const;
    void  setParam (int index, float value);
    juce::String getParamName (int index) const;

    MixerModel& mixer;
    int channel = 0;
    int slot    = 0;

    int dragParam = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertEditorContent)
};

//==============================================================================
/** Window shell for InsertEditorContent. Same close contract as above. */
class InsertEditorWindow final : public juce::DocumentWindow
{
public:
    InsertEditorWindow (MixerModel& mixerModel, int channel, int slot);

    std::function<void()> onCloseRequest;

    int getChannel() const noexcept { return channel; }
    int getSlot() const noexcept    { return slot; }

    void closeButtonPressed() override;

private:
    int channel = 0;
    int slot    = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertEditorWindow)
};

} // namespace atomic

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
    Phase 106: the host's own preview keyboard, at the bottom of a plugin's
    window.

    Why it exists: an instrument's editor is where the patch gets chosen, and a
    patch can only be judged by hearing it. Before this strip the only way to
    play while that window was in front was the computer keyboard — which does
    work (Phase 42 forwards the keys) but is invisible, and goes silent the
    moment focus lands in one of the plugin's own text fields. A row of keys
    that can be clicked is the one preview entry point that does not care where
    the focus is.

    Why it is the host's and not the plugin's: a plugin may have no editor, no
    keyboard, or a keyboard that only drives its own internal voices. The host
    can always send a note-on to the slot, so the strip drives THAT — through
    the same `auditionPreview` the piano roll uses. One path, one sound.

    A held key is refreshed on a timer rather than sent once: the host releases
    a hosted preview a little after the last request, so "still held" has to
    keep saying so. Letting go stops the timer and the note expires by itself.
*/
class AuditionStrip final : public juce::Component,
                            private juce::Timer
{
public:
    AuditionStrip();
    ~AuditionStrip() override;

    /** The band's height, and the only number the window has to reserve. */
    static int preferredHeight() noexcept { return atomic::ui::px (44); }

    /** The strip names a pitch and a velocity; the host decides which slot
        hears it. Called again every 200 ms while a key is held. */
    std::function<void (int pitch, float velocity)> onHold;
    std::function<void()>                        onRelease;

    static constexpr int lowestPitch = 48;      // C3, two octaves up from there
    static constexpr int numKeys     = 24;

    /** The key layout, asked by the paint code and by the self-test: a keyboard
        is a white-key ruler with short dark keys straddling the lines. */
    static bool isBlackKey (int index) noexcept;

    juce::Rectangle<float> getKeyBounds (int index) const;
    int  getKeyIndexAt (juce::Point<float> position) const;
    int  getHeldIndex() const noexcept { return heldIndex; }
    int  getHeldPitch() const noexcept { return heldIndex >= 0 ? lowestPitch + heldIndex : -1; }

    /** Same path a mouse press takes, so a check about a held key is about the
        behaviour and not about synthesising a click. */
    void beginHoldForTest (int index);
    void endHoldForTest();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> getKeyboardArea() const;
    juce::Rectangle<int>   getLabelArea() const;

    void hold (int index);
    void releaseHeld();
    void timerCallback() override;

    int heldIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionStrip)
};

//==============================================================================
/** The window's content: the plugin's body on top, the preview strip below.

    Split out so the window itself stays a DocumentWindow and the plugin's
    editor keeps its own size. The strip's height is reserved by resizing the
    body, never by asking the plugin to shrink to a size it never offered.
*/
class EditorHost final : public juce::Component
{
public:
    /** Takes ownership of `bodyToUse` — an editor, or a label when the plugin
        has no UI. A null body is allowed and leaves the band empty. */
    explicit EditorHost (std::unique_ptr<juce::Component> bodyToUse);

    AuditionStrip& getStrip() noexcept { return strip; }
    juce::Component* getBody() const noexcept { return body.get(); }

    /** Where the plugin's own pixels live, in this component's coordinates. */
    juce::Rectangle<int> getBodyArea() const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    std::unique_ptr<juce::Component> body;
    AuditionStrip strip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorHost)
};

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

    /** Phase 106: the preview keyboard lives in this window's content. Two
        callbacks, not a slot app: the window does not know which slot it is
        showing to the engine, only the host does. */
    void setAuditionCallbacks (std::function<void (int pitch, float velocity)> onHold,
                               std::function<void()> onRelease);

    bool hasAuditionStrip() const noexcept { return host != nullptr; }

    /** The strip, for the self-test. Null when the window has no content. */
    AuditionStrip* getAuditionStrip() const noexcept;

    /** Where the plugin's own pixels sit inside the window's content. */
    juce::Rectangle<int> getEditorBodyAreaForTest() const;

    /** Phase 106: the height the body ASKED for, before the strip was added.

        Comparing it with the area it actually got is the only way to tell
        "the window grew by a keyboard" from "the keyboard ate the plugin" —
        those two look identical from the content's total height. */
    int getEditorBodyHeightRequested() const noexcept { return requestedBodyHeight; }

    void closeButtonPressed() override;

private:
    EditorHost*        host = nullptr;
    juce::KeyListener* keyForwarder = nullptr;
    int instanceId = 0;
    int requestedBodyHeight = 0;

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

    /** The window title's middle part, e.g. "均衡" / "混响". Public so the
        window shell can name itself the same thing the content paints. */
    static juce::String effectName (fx::Type type);

    /** Window height for `numParams` knob rows: header band + rows + footer. */
    static int preferredHeightFor (int numParams);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int preferredWidth = ui::px (420);

private:
    void timerCallback() override;

    juce::Rectangle<int> getParamRowBounds (int index) const;
    juce::Rectangle<int> getBarBounds (int index) const;
    juce::Rectangle<int> getPowerBounds() const;

    /** How many knobs this effect has (EQ nine, comp six, reverb five). */
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

    /** Phase 47: same contract as PluginEditorWindow. This window is its own
        top-level component, so a key pressed here never reaches MainComponent:
        空格 stopped starting the transport and the computer keyboard stopped
        playing notes the moment the user clicked an effect's editor. */
    void setKeyForwarder (juce::KeyListener* listener);
    bool hasKeyForwarder() const noexcept { return keyForwarder != nullptr; }

    void closeButtonPressed() override;

private:
    int channel = 0;
    int slot    = 0;
    juce::KeyListener* keyForwarder = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertEditorWindow)
};

} // namespace atomic

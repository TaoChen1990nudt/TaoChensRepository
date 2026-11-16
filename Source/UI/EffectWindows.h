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
class PluginEditorWindow final : public juce::DocumentWindow,
                                 private juce::Timer
{
public:
    PluginEditorWindow (juce::AudioProcessor& processor,
                        const juce::String& pluginName,
                        int instanceIdToShow,
                        bool followEditorSize = true);

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

    /** Phase 122: this window opts out of per-monitor DPI rescaling. The
        plugin's native view manages its own content scale (VST3's
        IPlugViewContentScaleSupport); a second, host-side scale on the frame
        is what leaves the plugin's black backing component exposed when the
        window is dragged to a display with a different DPI. JUCE's own
        AudioPluginHost pins the same value for exactly this reason. */
    static constexpr float frameDesktopScale = 1.0f;
    float getDesktopScaleFactor() const override { return frameDesktopScale; }

    /** Test seam: the editor being framed, or nullptr for a plugin with no UI. */
    juce::AudioProcessorEditor* getEditorForTest() const noexcept { return editor; }

    /** Phase 123: put the outer frame back in exact agreement with the editor.

        A VST3 view recomputes its own size when the window crosses onto a
        display with a different DPI. JUCE's resizeToFitContent only reacts
        when the editor calls setSize; a plugin that resizes its native HWND
        without touching the JUCE component leaves the frame larger than the
        view, and the plugin's black backing component shows through. Calling
        this after the fact realigns the two — the frame is sized from the
        editor, never the other way round. Idempotent, and safe to call from
        any thread that owns the message loop. */
    void resyncFrameToEditor();

    /** The rectangle the editor is supposed to fill. Equal to the editor's
        own bounds once resyncFrameToEditor has run; the self-test compares
        the two to prove there is no gap for the black backing to show. */
    juce::Rectangle<int> getFrameContentAreaForTest() const noexcept;

    /** Public so the self-test can drive one poll directly instead of waiting
        for a real timer tick (and so removing the call inside is a biteable
        mutation). */
    void timerCallback() override;

    /** A top-level window crossing onto another display is the only moment a
        real DPI change is observable. The baseline poll catches the cases a
        move callback misses; both funnel into checkDisplayScale(). */
    void moved() override;

    /** Test seam: make the next poll believe the display changed, without
        needing a second physical monitor in the self-test. `scale` is the
        scale to report; 0 means "use the real display's scale". */
    void simulateDisplayChangeForTest (double scale = 0.0);

private:
    void checkDisplayScale();
    /** Phase 124: the one host call that reaches the plugin's own scaling
        (VST3: IPlugViewContentScaleSupport::setContentScaleFactor). Kept as a
        separate function so the self-test can bite it by removing the body. */
    void applyEditorScale (double scale);
    void logDpiState (const char* event, double scale);

    /** Phase 123: the frame follows the editor, never editor follows frame.
        false is only for the self-test fixture that deliberately desynchronises
        the two; production windows must leave it true. */
    bool followingEditorSize = true;
    bool syncingFrame = false;
    double lastDisplayScale = 1.0;
    double forcedDisplayScale = 0.0;   // self-test only; 0 = ask the desktop
    bool displayScaleReady = false;    // construction must not push a scale

    /** Phase 122: the frame's constrainer counts window pixels; the plugin's        counts editor pixels. Neither may be used raw for the other — the host
        has to translate, including the native title bar's border, or dragging
        the frame asks the plugin for a size it cannot honour. */
    class EditorConstrainer final : public juce::BorderedComponentBoundsConstrainer
    {
    public:
        EditorConstrainer (PluginEditorWindow& w, juce::AudioProcessorEditor* e) noexcept;

        juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override;
        juce::BorderSize<int> getAdditionalBorder() const override;

    private:
        PluginEditorWindow&  window;
        juce::AudioProcessorEditor* editor;
    };

    juce::KeyListener* keyForwarder = nullptr;
    int instanceId = 0;
    juce::AudioProcessorEditor* editor = nullptr;
    std::unique_ptr<EditorConstrainer> editorConstrainer;

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

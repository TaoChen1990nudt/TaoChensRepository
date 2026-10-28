#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace atomic
{

//==============================================================================
/**
    Tempo field.

    Three ways to set it, because a DAW needs all three:

      - drag up / down          continuous, and the transport follows live
      - click                   opens a text editor, Enter commits
      - the two chevrons        +/- 1 BPM (Shift: +/- 10)

    It deliberately does not own the tempo: it reports a number and the host
    decides what to do with it. That keeps this component free of any knowledge
    about the engine, and makes it trivial to test.
*/
class BpmField final : public juce::Component
{
public:
    BpmField();
    ~BpmField() override;

    std::function<void (double)> onBpmChanged;

    /** Phase 80: the right-click menu's second verb. The field does not own the
        automation (it does not even know a curve exists), it only asks the host
        to put the tempo row where the user can see it. */
    std::function<void()> onShowTempoAutomation;

    /** Whether the host currently has a tempo row in the automation window. Used
        only to tick the menu item; the field never infers it. */
    void setTempoAutomationShown (bool shown) noexcept { tempoAutomationShown = shown; }
    bool isTempoAutomationShown() const noexcept       { return tempoAutomationShown; }

    static constexpr double minBpm = 30.0;
    static constexpr double maxBpm = 300.0;

    /** Where 复位 puts it. One constant, because the session's starting tempo,
        the reset target and the self-test's expectation must be one number. */
    static constexpr double defaultBpm = 120.0;

    /** `notify` false is for host-initiated updates, so setting the field from
        the model does not bounce straight back into the model. */
    void   setBpm (double newBpm, bool notify);
    double getBpm() const noexcept { return bpm; }

    bool isEditing() const noexcept { return editing; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void applyBpm (double candidate, bool notify);
    void nudge (double delta);
    void openEditor();
    void closeEditor (bool commit);
    juce::String formatBpm() const;

    juce::Rectangle<int> getDragBounds() const;
    juce::Rectangle<int> getUpBounds() const;
    juce::Rectangle<int> getDownBounds() const;

    juce::TextEditor editor;

    double bpm          = 120.0;
    double dragStartBpm = 120.0;
    int    dragStartY   = 0;
    bool   dragging     = false;
    bool   dragMoved    = false;
    bool   editing      = false;

    /** Phase 80: the id the extra menu item reports back. Well clear of 9001
        (复位) and of the small numbers a sub-menu owns. */
    static constexpr int tempoAutomationMenuId = 9101;

    bool tempoAutomationShown = false;
    bool hover       = false;
    bool hoverUp     = false;
    bool hoverDown   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BpmField)
};

} // namespace atomic

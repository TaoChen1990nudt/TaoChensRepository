#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>
#include <memory>
#include <vector>

#include "Model/HardwareRack.h"
#include "UI/AutomationLaneComponent.h"

namespace atomic
{

//==============================================================================
/**
    The right-hand synth rack, now also the synth mixer.

    Each card shows what the synth is (waveform, level), where it gets its
    signal from (note count + how many of those notes bypassed their layer),
    how loud it is right now, and — Phase 5 — its own fader, pan, solo, mute and
    an unfoldable automation lane for one synth parameter (filter or level).

    The panel deliberately knows nothing about the audio classes: it reads
    numbers through callbacks. That keeps the UI testable — and keeps the audio
    thread out of the paint path.
*/
class RackPanel final : public juce::Component,
                        public RackModel::Listener
{
public:
    static constexpr int panelWidth   = ui::px (268);
    static constexpr int headerHeight = ui::px (52);
    static constexpr int cardHeight   = ui::px (128);   // Phase 11 added the send row
    static constexpr int laneHeight   = ui::px (92);
    static constexpr int cardGap      = ui::px (10);
    static constexpr int footerHeight = ui::px (58);

    /** Phase 17: a folded card is one line — the synth's name. */
    static constexpr int collapsedHeight = ui::px (40);

    explicit RackPanel (RackModel& rackToUse);
    ~RackPanel() override;

    /** Fed by the host: 0..1 peak for a slot id. */
    std::function<float (int slotId)> levelSource;
    /** Fed by the host: how many notes currently route to a slot. */
    std::function<int (int slotId)>   noteCountSource;
    /** ... and how many of those bypassed their layer. */
    std::function<int (int slotId)>   overrideCountSource;

    /** The "＋ 添加音源" button. The host owns the plugin library, so it owns
        the menu that offers built-in synths next to hosted VST3s. */
    std::function<void (juce::Point<int> screenPos)> onAddSlotRequested;

    /** Right-click on a card, or a click on its "→ 输出" chip. The host owns the
        plugin library AND the mixer channel list, so it owns this menu:
        replace-the-instrument, send-to-channel, automation, remove. */
    std::function<void (int slotId, juce::Point<int> screenPos)> onCardMenuRequested;

    /** Phase 21: the card menu as a hover sub-menu of the fader's value menu.
        Same split as the layer panel's pair. */
    std::function<void (juce::PopupMenu&, int slotId)> buildCardSubmenu;
    std::function<void (int slotId, int result)> onCardSubmenuChosen;

    /** A card was clicked while notes are selected: re-route them to it. Now
        bound to Alt-click (and the card menu), because a plain click means
        "inspect this synth" — see onCardFocused. */
    std::function<void (int slotId)> onSlotChosen;

    /** Phase 32: the × on a card. A sampler holds the only copy of the link
        between itself and the clips that play it, so the host has to delete
        both in one step — the panel cannot know about the roll. When unset the
        panel falls back to removing the slot on its own. */
    std::function<void (int slotId)> onSlotRemoveRequested;

    /** Phase 51: the sampler card's 拉伸 control. Stretch mode is a BLOCK
        property, and the panel cannot see blocks — the host reads and writes
        them. `samplerStretchState` returns the state across all blocks that
        play the slot: the StretchMode value when they all agree, -1 when they
        disagree (labelled 混合), -2 when no block uses the slot. The menu
        offers the three modes and reports the chosen StretchMode through
        `onSamplerStretchChosen`, which the host applies to those blocks. */
    std::function<int (int slotId)>  samplerStretchState;
    std::function<void (int slotId, int stretchMode)> onSamplerStretchChosen;

    /** Phase 52: the 只变调 range, in semitones.

        Same contract as the stretch pair above and for the same reason — the
        value is a BLOCK property (two blocks of one file may want different
        ceilings), and this panel cannot see blocks. `samplerPitchRangeState`
        returns the semitones when every block on the slot agrees, -1 when they
        differ (labelled 混合), -2 when no block uses the slot at all. The chip
        is only drawn while those blocks are in 只变调: a range for a mode
        nobody is in is a control that changes nothing. */
    std::function<int (int slotId)>  samplerPitchRangeState;
    std::function<void (int slotId, int semitones)> onSamplerPitchRangeChanged;



    /** Phase 23: "there is a note/group selection right now". With a selection
        on screen, a plain card click mounts it on that synth — the gesture the
        user is already making when they look at a selection and then reach for
        an instrument. With nothing selected the plain click stays harmless
        (inspect only), so a stray click can never rewire a sheet. */
    void setSelectionActive (bool active);
    bool isSelectionActive() const noexcept { return selectionActive; }

    /** Phase 16: a card body was clicked. The host spotlights this synth in the
        piano roll and the layer list. -1 means "the empty rack was clicked". */
    std::function<void (int slotId)> onCardFocused;

    /** Highlights the synth the current note selection plays on. `hasSelection`
        is separate from the id because "nothing is selected" and "the
        selection resolves to no synth" must not look the same. */
    void setRoutingFocus (int slotId, bool hasSelection);
    bool hasRoutingFocus() const noexcept { return focusActive; }

    /** Phase 51: the sampler card's 拉伸 button. Public because the self-test
        asserts it sits inside the card and clear of the readout text — a
        control that overlaps the text it sits beside is invisible in a
        screenshot and obvious only when the click does the wrong thing. */
    juce::Rectangle<int> getStretchBounds (int index) const;

    /** Phase 52: the 只变调 range chip, left of the stretch chip on the readout
        row. Empty when the slot is not a sampler or its blocks are not in
        只变调 — which is also the hit test, so "not drawn" and "not clickable"
        cannot disagree. */
    juce::Rectangle<int> getPitchRangeBounds (int index) const;

    /** Phase 51: test hook — the card rectangle, so "the chip is inside the
        card" is a comparison, not an impression. */
    juce::Rectangle<int> getCardBoundsForTest (int index) const { return getCardBounds (index); }

    /** Which card is highlighted, or -1. The self-test asserts the highlight
        follows the selection, so the id has to be readable. */
    int getRoutingFocusSlotId() const noexcept { return focusSlotId; }

    /** Called at ~30 Hz while the transport runs. */
    void updateMeters();

    void setPlayheadBeat (double beats);

    //-- Component ------------------------------------------------------------
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    //-- Phase 17: scrolling --------------------------------------------------
    int  getContentHeight() const;
    int  getViewHeight() const;
    int  getScrollY() const noexcept { return (int) scrollY; }
    void setScrollY (int pixels);

    //-- RackModel::Listener ---------------------------------------------------
    void rackChanged() override;

private:
    enum class Part { none, body, solo, mute, close, waveform, volume, pan, send, collapse,
                      stretch, pitchRange };

    int  cardTop (int index) const;
    bool isExpanded (int index) const;
    bool isCollapsed (int index) const;
    int  cardHeightAt (int index) const;
    void clampScroll();
    juce::Rectangle<int> getViewArea() const;
    juce::Rectangle<int> getCollapseBounds (int index) const;

    juce::Rectangle<int> getCardBounds (int index) const;
    juce::Rectangle<int> getWaveformBounds (int index) const;
    juce::Rectangle<int> getSoloBounds (int index) const;
    juce::Rectangle<int> getMuteBounds (int index) const;
    juce::Rectangle<int> getCloseBounds (int index) const;
    juce::Rectangle<int> getVolumeBounds (int index) const;
    juce::Rectangle<int> getPanBounds (int index) const;
    juce::Rectangle<int> getSendBounds (int index) const;
    juce::Rectangle<int> getAddButtonBounds() const;

    juce::Rectangle<int> getLaneCanvasBounds (int index) const;
    juce::Rectangle<int> getLaneTargetBounds (int index, int targetIndex) const;
    juce::Rectangle<int> getLaneEnableBounds (int index) const;

    void drawCard (juce::Graphics&, int index);
    void drawLaneHeader (juce::Graphics&, int index);

    void syncLaneEditors();
    void updateLayout();

    RackModel& rack;

    std::vector<std::unique_ptr<AutomationLaneComponent>> laneEditors;
    std::vector<int> laneSlotIds;

    double playheadBeat = -1.0;

    /** Phase 17: pixels the card list is pushed up. */
    float  scrollY = 0.0f;

    /** Phase 27: how many slots the last change reported, so a GROWTH can be
        told apart from a rename. Only growth scrolls the new card into view. */
    int    lastSlotCount = 0;

    int  hoveredCard = -1;
    Part hoveredPart = Part::none;
    int  focusSlotId = -1;          // synth the selection plays on
    bool focusActive = false;
    bool selectionActive = false;   // Phase 23: a plain click may mount the selection
    bool hoveringAdd = false;

    Part dragPart = Part::none;
    int  dragCard = -1;

    /** Phase 52: the numeric chip is dragged RELATIVELY — the value at the
        press plus one semitone per three pixels. Mapping 1..36 absolutely
        across 66 px would make the control jump two semitones per pixel. */
    int   dragStartValue = 0;
    int   dragStartX     = 0;

    /** Phase 52: where a pointer went down on a sampler card, so a drag can be
        told from a click that is still just a click. */
    juce::Point<int> dragOrigin;
    bool  dragMayStart = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackPanel)
};

} // namespace atomic

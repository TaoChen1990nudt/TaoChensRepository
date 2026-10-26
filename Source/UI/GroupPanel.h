#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>
#include <vector>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"

namespace atomic
{

//==============================================================================
/**
    Phase 17: every note group in the session, in one list.

    The roll can already group notes and mount them on a synth; what it cannot
    do is give you the ten-thousand-foot view — "what groups do I have, and how
    loud is each one". This panel is that view, and nothing more:

      * **音量 / 声相** — a fader and a pan per group. Not a bus: the values are
        multiplied and added into each member's voice, the same way the layer
        fader works. There is deliberately no meter and no routing here.
      * **发送音源** — the same mount the group's own menu sets, shown as a pill
        so the answer to "where does this group go" is always on screen.
      * **折叠** — a folded group is a one-line title bar, so a project with
        twenty groups still fits in the panel.

    Groups are addressed by (layerIndex, groupId), never by pointer: the layer
    owns them in a vector, and any add/delete can move them.
*/
class GroupPanel final : public juce::Component,
                         public LayerStack::Listener,
                         public RackModel::Listener
{
public:
    static constexpr int headerHeight    = ui::px (46);
    static constexpr int rowHeight       = ui::px (94);
    static constexpr int collapsedHeight = ui::px (30);

    GroupPanel (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~GroupPanel() override;

    /** One entry per group, in layer order then creation order. */
    struct RowRef
    {
        int layerIndex = -1;
        int groupId    = -1;
    };

    std::vector<RowRef> getRows() const;
    int getNumRows() const;

    /** The group the host should act on when a row's menu is opened. */
    std::function<void (int layerIndex, int groupId, juce::Point<int> screenPos)> onGroupMenuRequested;

    /** The send pill was clicked: the host owns the rack, so it owns the menu. */
    std::function<void (int layerIndex, int groupId, juce::Point<int> screenPos)> onMountRequested;

    /** Draw-only spotlight, same synth id the roll and the rack use. */
    void setFocusSlotId (int slotId);
    int  getFocusSlotId() const noexcept { return focusSlotId; }

    /** True when this group's notes reach the focused synth. The self-test
        reads it to prove the panel and the roll agree. */
    bool isGroupFocused (int layerIndex, int groupId) const;

    /** Phase 35: mute a row, by row index. The chip and the keyboard/menu path
        both land here, so "the panel muted it" and "a test muted it" cannot be
        two different operations. Out-of-range is ignored. */
    void toggleMute (int rowIndex);

    /** Phase 35: where the mute chip sits, so a test can click the real target
        rather than a coordinate that drifts when the row is restyled. */
    juce::Rectangle<int> getMuteBounds (int rowIndex) const;

    //-- Scrolling ------------------------------------------------------------
    int  getContentHeight() const;
    int  getViewHeight() const;
    int  getScrollY() const noexcept { return (int) scrollY; }
    void setScrollY (int pixels);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    //-- Listeners -----------------------------------------------------------
    void layersChanged() override;

    /** Phase 21: grouping does not change the LAYER count, it changes the
        notes — so it arrives here, not in layersChanged(). Without this
        override a freshly made group did not paint until something else asked
        for a repaint (a mouse hover was the first thing that did), which read
        as "the group list is one gesture behind". */
    void notesChanged() override;

    void rackChanged() override { repaint(); }

private:
    /** Which control of which row the mouse is over (or dragging). */
    enum class Part { none, body, collapse, volume, pan, send, mute };

    juce::Rectangle<int> getViewArea() const;
    juce::Rectangle<int> getRowBounds (int rowIndex) const;
    juce::Rectangle<int> getCollapseBounds (int rowIndex) const;
    juce::Rectangle<int> getVolumeBounds (int rowIndex) const;
    juce::Rectangle<int> getPanBounds (int rowIndex) const;
    juce::Rectangle<int> getSendBounds (int rowIndex) const;

    NoteGroup* groupFor (const RowRef&) const;
    int  rowAt (juce::Point<int>, Part* partOut) const;
    void clampScroll();
    void setLevelFromMouse (NoteGroup&, juce::Rectangle<int>, float x);
    void setPanFromMouse (NoteGroup&, juce::Rectangle<int>, float x);

    void drawRow (juce::Graphics&, int rowIndex);

    LayerStack& stack;
    RackModel&  rack;

    float scrollY    = 0.0f;
    int   hoveredRow = -1;
    Part  hoveredPart = Part::none;

    int   dragRow  = -1;
    Part  dragPart = Part::none;

    int   focusSlotId = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GroupPanel)
};

} // namespace atomic

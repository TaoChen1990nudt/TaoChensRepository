#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>
#include <memory>
#include <vector>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "UI/AutomationLaneComponent.h"

namespace atomic
{

//==============================================================================
/**
    Left-hand sheet list, now also the layer mixer.

    One row per layer: colour chip, name, note count, an eye toggle, the output
    dropdown from Phase 3, and — the Phase 5 addition — solo / mute / volume /
    pan plus an unfoldable macro automation lane.

    The row height is a FUNCTION OF THE MODEL: rows below an unfolded lane are
    pushed down by it. That is why every hit test goes through getRowBounds()
    instead of assuming `headerHeight + index * rowHeight`; the moment a lane
    folds open, the assumption is wrong.
*/
class LayerPanel final : public juce::Component,
                         public LayerStack::Listener,
                         public RackModel::Listener
{
public:
    static constexpr int panelWidth   = ui::px (300);
    static constexpr int headerHeight = ui::px (52);
    static constexpr int rowHeight    = ui::px (74);
    static constexpr int laneHeight   = ui::px (92);
    static constexpr int footerHeight = ui::px (58);

    /** Phase 17: a folded layer is a title bar. Big enough to click, small
        enough that ten sheets fit without scrolling. */
    static constexpr int collapsedHeight = ui::px (32);

    LayerPanel (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~LayerPanel() override;

    /** Fed by the host: 0..1 peak for a layer index. */
    std::function<float (int layerIndex)> levelSource;

    /** Right-click on a layer row: the host owns the automation window, so it
        owns the "add track for this parameter" menu too. */
    std::function<void (int layerIndex, juce::Point<int> screenPos)> onAutomationRequested;

    /** Phase 12: the 音量 / 声相 chips under an open lane also create a track in
        the automation window. Without this the panel could show a curve that
        the window knows nothing about. */
    std::function<void (int layerIndex, int target)> onAutomationTargetChosen;

    /** "复位": drop every per-note synth override in this layer, so its notes
        play on whatever the layer's output is again. */
    std::function<void (int layerIndex)> onResetNoteRouting;

    /** Drives the playhead marker inside unfolded automation lanes. */
    void setPlayheadBeat (double beats);

    //-- Phase 16: the focused synth -----------------------------------------
    /** Highlights every sheet that feeds one synth: its own output, a group
        mounted on it, or a single note overriding onto it. -1 clears it.
        Draw-only; no routing changes. */
    void setFocusSlotId (int slotId);
    int  getFocusSlotId() const noexcept { return focusSlotId; }

    /** True when anything in this layer reaches the focused synth. Read by the
        self-test to prove the row highlight is not decoration. */
    bool isLayerAttachedToFocus (int index) const;

    /** How many notes in this layer bypass the layer's synth. Drives the
        "复位 N" pill, and the self-test reads it to prove the reset worked. */
    int getOverrideCount (int index) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    //-- Phase 17: scrolling --------------------------------------------------
    /** How tall the rows are in total, and how much of them the panel can show.
        Public because the self-test asserts a long list really scrolls. */
    int  getContentHeight() const;
    int  getViewHeight() const;
    int  getScrollY() const noexcept { return (int) scrollY; }
    void setScrollY (int pixels);

    //-- Listeners -------------------------------------------------------------
    void layersChanged() override;
    void rackChanged() override   { repaint(); }

private:
    enum class Part { none, body, eye, solo, mute, output, volume, pan, resetRouting, collapse };

    juce::Rectangle<int> getRowBounds (int index) const;
    juce::Rectangle<int> getBodyBounds (int index) const;
    juce::Rectangle<int> getEyeBounds (int index) const;
    juce::Rectangle<int> getSoloBounds (int index) const;
    juce::Rectangle<int> getMuteBounds (int index) const;
    juce::Rectangle<int> getOutputBounds (int index) const;
    juce::Rectangle<int> getVolumeBounds (int index) const;
    juce::Rectangle<int> getPanBounds (int index) const;
    juce::Rectangle<int> getResetRoutingBounds (int index) const;
    juce::Rectangle<int> getAddButtonBounds() const;
    juce::Rectangle<int> getCollapseBounds (int index) const;
    juce::Rectangle<int> getViewArea() const;

    bool isCollapsed (int index) const;
    void clampScroll();
    int  rowHeightAt (int index) const;

    juce::Rectangle<int> getLaneCanvasBounds (int index) const;
    juce::Rectangle<int> getLaneTargetBounds (int index, int targetIndex) const;
    juce::Rectangle<int> getLaneEnableBounds (int index) const;

    bool isExpanded (int index) const;

    void drawEye (juce::Graphics&, juce::Rectangle<float>, juce::Colour, bool open) const;
    void drawOutputPill (juce::Graphics&, int index, bool hovered) const;
    void drawRow (juce::Graphics&, int index);
    void drawLaneHeader (juce::Graphics&, int index);

    void showOutputMenu (int index);

    void syncLaneEditors();
    void updateLayout();

    LayerStack& stack;
    RackModel&  rack;

    std::vector<std::unique_ptr<AutomationLaneComponent>> laneEditors;

    double playheadBeat = -1.0;

    /** Phase 17: pixels the row list is pushed up. Rows are laid out in content
        space and offset here, so every hit test keeps working unchanged. */
    float  scrollY = 0.0f;

    int  hoveredRow  = -1;
    Part hoveredPart = Part::none;

    /** Phase 16: the synth the roll is spotlighting, -1 for none. */
    int  focusSlotId = -1;

    Part dragPart = Part::none;
    int  dragRow  = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerPanel)
};

} // namespace atomic

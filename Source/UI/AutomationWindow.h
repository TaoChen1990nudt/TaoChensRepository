#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include "Util/Theme.h"

#include <functional>
#include <map>

#include "Model/AutomationModel.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"
#include "Model/TempoModel.h"

namespace atomic
{

//==============================================================================
/**
    The dedicated automation window under the piano roll.

    Why a window instead of lanes folded into the panels: a layer can have a
    volume curve AND a pan curve, a synth a filter curve AND a level curve, and
    growing every panel row by 92 px to fit them makes the sheet list unusable
    at four layers. Here they are rows in one list, orderable, deletable, and
    all drawn on one shared timeline.

    Two hard rules:

    1. **Tracks reference owners by stable id**, resolved on every paint. A
       cached AutomationLane* would dangle the moment its layer is deleted —
       and the user can delete a layer with the window open.

    2. **The horizontal mapping is the piano roll's**, handed in by the host
       (`setTimeline` + `setScrollBeats`). If this window had its own zoom, the
       two views would disagree about where beat 8 is, which is precisely the
       kind of drift a DAW cannot afford.
*/
class AutomationWindow final : public juce::Component,
                               public AutomationModel::Listener,
                               public LayerStack::Listener,
                               public RackModel::Listener
{
public:
    static constexpr int headerHeight = ui::px (26);
    /** Exactly the roll's keyboard width (`ui::gutterWidth`). This is not a
        cosmetic alignment: the canvas starts where this column ends, so any
        difference is a horizontal offset between the curves and the grid they
        are edited against — 86 px of it, before the two columns were tied
        together. 74 design units is tight for two lines of text, which is why
        the row layout below is drawn for it (see drawList). */
    static constexpr int listWidth    = ui::gutterWidth;
    static constexpr int rowHeight    = ui::px (44);   // default; every row can be resized
    static constexpr int rulerHeight  = ui::px (18);

    //-- Phase 12: resizable rows ---------------------------------------------
    static constexpr int minRowHeight = ui::px (36);
    static constexpr int maxRowHeight = ui::px (220);

    /** The band at the bottom of a row that resizes it. Wide enough to hit with
        a mouse, narrow enough not to steal clicks from the curve. */
    static constexpr int resizeZone   = ui::px (6);

    AutomationWindow (LayerStack& layerStack, RackModel& rackToUse,
                      AutomationModel& automationModelToUse, MixerModel& mixerModelToUse,
                      TempoModel& tempoModelToUse);
    ~AutomationWindow() override;

    /** Fired after any edit that the audio engine must hear about. */
    std::function<void()> onChanged;

    /** Fired when the user asks for the view to follow a different beat range
        (currently unused — the roll owns horizontal scrolling). */
    std::function<void (double)> onScrollRequested;

    void setTimeline (double lengthBeats, double pixelsPerBeat);
    void setScrollBeats (double beats);
    void setPlayheadBeat (double beats);

    /** Phase 18: the roll's snap switch reaches here too. A breakpoint placed
        while the grid is on has to sit on the grid — otherwise the two windows
        disagree about where "a 1/16" is. */
    void setSnapEnabled (bool shouldSnap);
    bool isSnapEnabled() const noexcept { return snapEnabled; }

    /** Scrolls vertically so a track is on screen. Used right after a curve is
        created from the rack: a new row appended below the fold is the same as
        no row at all. */
    void revealTrack (int trackId);

    //-- Phase 12: row geometry ------------------------------------------------
    /** Height of a row in pixels. Stored per track id, not per index: dragging a
        row up carries its height with it, and ids survive reordering. */
    int  getTrackHeight (int index) const;
    void setTrackHeight (int index, int height);

    /** Total height of every row, which is what the scroll clamps against. */
    int  getContentHeight() const;

    /** Phase 80: the label a row resolves to, for the self-test. A row whose
        owner cannot be resolved draws as an empty line, which is exactly the
        defect a screenshot cannot tell apart from "no row at all". */
    juce::String getRowNameForTest (int index) const
    {
        const auto resolved = resolveTrackAt (index);
        return resolved.name.isNotEmpty() ? resolved.name + " / " + resolved.parameter
                                          : juce::String();
    }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    //-- Listeners -------------------------------------------------------------
    void automationTracksChanged() override;
    void layersChanged() override  { repaint(); }
    void notesChanged() override   {}
    void rackChanged() override    { repaint(); }

private:
    struct Resolved
    {
        AutomationLane* lane = nullptr;
        juce::String name;
        juce::Colour colour = theme::accent;
        juce::String parameter;      // "音量" / "声相" / "滤波"
    };

    Resolved resolve (const AutomationTrack&) const;
    Resolved resolveTrackAt (int index) const;

    juce::Rectangle<int> getHeaderArea() const;
    juce::Rectangle<int> getListArea() const;
    juce::Rectangle<int> getCanvasArea() const;
    juce::Rectangle<int> getCanvasRulerArea() const;
    juce::Rectangle<int> getRowsArea() const;

    juce::Rectangle<int> getListRowBounds (int index) const;
    int  rowTopAt (int index) const;
    juce::Rectangle<int> getResizeBounds (int index) const;
    juce::Rectangle<float> getCanvasRowBounds (int index) const;
    juce::Rectangle<int> getDeleteBounds (int index) const;
    juce::Rectangle<int> getEnableBounds (int index) const;

    juce::Point<float> beatToPoint (const juce::Rectangle<float>& row, double beat, float value) const;
    double beatAtX (float x) const;

    /** beatAtX, then rounded to the grid when snapping is on. Every breakpoint
        goes through here, so "snap off" cannot leave a stray rounded point. */
    double snappedBeatAtX (float x) const;
    float  valueAtY (const juce::Rectangle<float>& row, float y) const;

    int rowIndexAtY (float y) const;
    int pointIndexAt (int rowIndex, juce::Point<float> position) const;
    int visibleRowCount() const;

    void clampScroll();
    void drawHeader (juce::Graphics&);
    void drawList (juce::Graphics&);
    void drawCanvas (juce::Graphics&);
    void drawTrackRow (juce::Graphics&, int index, const AutomationTrack&, const Resolved&);

    void selectTrack (int index);
    void deleteTrack (int index);
    void toggleTrackEnabled (int index);

    LayerStack&        stack;
    RackModel&         rack;
    AutomationModel&   automation;
    MixerModel&        mixer;
    /** Phase 80: the tempo lane lives here, not in the mixer, so the window needs
        it handed in like the mixer is. */
    TempoModel&        tempo;

    double lengthBeats   = timeline::defaultLengthBeats;
    double pixelsPerBeat = 48.0;
    double scrollBeats   = 0.0;
    double playheadBeat  = -1.0;
    bool   snapEnabled   = true;
    int    scrollY       = 0;

    int hoveredRow    = -1;
    int hoveredPart   = 0;          // 0 none, 1 row, 2 delete, 3 enable, 4 point
    int hoveredPoint  = -1;

    bool draggingRow     = false;
    int  dragRowIndex    = -1;
    int  dragStartY      = 0;

    /** Phase 12: dragging a row's bottom edge. Kept apart from the reorder drag
        above — one gesture moves a row, the other changes how tall it is. */
    bool draggingResize  = false;
    int  resizeRowIndex  = -1;
    int  resizeStartY    = 0;
    int  resizeStartH    = 0;

    std::map<int, int> trackHeights;   // trackId -> pixels

    int dragPointRow   = -1;
    int dragPointIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationWindow)
};

} // namespace atomic

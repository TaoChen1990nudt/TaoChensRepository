#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include <functional>
#include <vector>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/Routing.h"

namespace atomic
{

//==============================================================================
/**
    The overlay piano roll.

    Phase 1 scope:
      - every visible layer draws into one shared grid
      - non-active layers are painted at GHOST_ALPHA (30%)
      - hit testing is restricted to the active layer, so ghost notes are
        click-through: you can never accidentally grab someone else's note

    Coordinate model: there is one shared "content" space (128 pitches tall,
    64 beats wide). scrollY / scrollBeats map content space to screen space.
    Every layer is drawn through the exact same mapping, which is what makes
    the sheets line up perfectly.
*/
class PianoRollComponent final : public juce::Component,
                                 public LayerStack::Listener,
                                 public RackModel::Listener,
                                 public juce::DragAndDropTarget
{
public:
    //-- Geometry -------------------------------------------------------------
    /** The keyboard is exactly the automation window's track-header column, so
        beat 0 starts at the same x in both panes. See ui::gutterWidth. */
    static constexpr int    keyboardWidth = ui::gutterWidth;
    static constexpr int    rulerHeight   = ui::px (32);
    static constexpr int    toolbarHeight = ui::px (28);
    static constexpr int    scrollbarHeight = ui::px (14);
    /** A pitch row. Grows with the UI scale: the notes get thicker, the music
        does not get wider — `beatWidth` below is deliberately left alone. */
    static constexpr float  keyHeight     = ui::sp (14.0f);

    /** Horizontal scale, in design pixels per beat. A VARIABLE, not a constant:
        Ctrl+wheel changes it, and everything measured in beats — notes, clips,
        the grid, the ruler — follows without any of that code knowing a zoom
        exists. That is the whole reason a clip stores `lengthBeats` and not a
        pixel width. */
    static constexpr float  defaultBeatWidth = 48.0f;
    static constexpr float  minBeatWidth     = 12.0f;
    static constexpr float  maxBeatWidth     = 180.0f;
    float beatWidth = defaultBeatWidth;

    float getBeatWidth() const noexcept { return beatWidth; }

    /** Sets the zoom, keeping the middle of the view where it is. Fires
        onScrollChanged so the automation window — the same timeline drawn
        below — stays in step. */
    void setBeatWidth (float pixels);

    static constexpr int    numPitches    = 128;
    /** The DEFAULT project grid — the length of an untouched session, and what
        the automation window lays out against. NOT a wall: the roll extends its
        own timeline in `beatChunk` steps as soon as you scroll past the end. */
    static constexpr double totalBeats    = 64.0;
    static constexpr double gridDivision  = 0.25;   // 1/16 in 4/4

    //-- Phase 11: the timeline grows on demand ------------------------------
    /** How much is added each time the view runs off the end. 64 beats = 16 bars,
        which is far enough that the growth is not a visible hiccup while
        dragging, and small enough that the scrollbar keeps meaning something. */
    static constexpr double beatChunk = 64.0;

    /** A ceiling so the transport maths stay finite. 10 000 beats is over five
        hours at 120 BPM — "unlimited" in any practical sense. */
    static constexpr double maxTimelineBeats = 10000.0;

    static constexpr int    numToolButtons = 8;      // 4 tools + copy-state + group + snap + ungroup

    //-- Phase 7: tools -------------------------------------------------------
    enum class Tool
    {
        select,     // drag notes, marquee
        pencil,     // click empty grid to create a note
        eraser,     // click a note to delete it
        hand        // drag anywhere to pan the view
    };

    //-- Opacity --------------------------------------------------------------
    static constexpr float ghostAlpha = 0.30f;      // non-active layers
    static constexpr float solidAlpha = 1.00f;      // active layer

    PianoRollComponent (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~PianoRollComponent() override;

    //-- Query (also used by the headless self-test in Main.cpp) --------------
    /** Id of the editable note under this point, or -1.

        Ghost notes are invisible to this function by design: if it returned
        them, rule "clicking a ghost must do nothing" would depend on the
        caller remembering to filter. */
    int getEditableNoteIdAt (juce::Point<float> position) const;

    /** Local-space bounds of a note, empty if that layer/note does not exist. */
    juce::Rectangle<float> getNoteScreenBounds (int layerIndex, int noteId) const;

    /** Phase 20: should this sheet appear on the canvas at all?

        The ACTIVE layer always does — the eye button hides GHOSTS, and hiding
        the sheet you are editing would be a switch that makes the editor look
        broken. Every other layer appears only when its eye is open. Public so
        the self-test can assert the rule the painter follows instead of
        guessing from pixels. */
    bool isLayerShown (int layerIndex) const;

    //-- Host wiring ----------------------------------------------------------
    /** Fired on double-click over an editable (active layer) note. */
    std::function<void (int layerIndex, int noteId)> onNoteDoubleClicked;

    /** Fired on any mouse-down in the canvas, used to dismiss floating panels. */
    std::function<void()> onCanvasMouseDown;

    /** Fired when the horizontal scroll position changes, so the automation
        window below can stay on the same beat range. */
    std::function<void()> onScrollChanged;

    /** Phase 12: fired when the timeline gets longer or is reset. The
        automation window shares this length, so a note dropped at beat 300 must
        widen the ruler under the roll as well — otherwise the two views would
        disagree about where the end of the project is. */
    std::function<void()> onTimelineChanged;

    /** Fired when a note-level edit happens inside the roll itself (the inline
        automation box), so the host can refresh the inspector and the meters. */
    std::function<void()> onNoteEdited;

    /** Fired whenever the selection changes. The rack uses it to highlight the
        synth the selected notes are actually playing on. */
    std::function<void()> onSelectionChanged;

    //-- Phase 7: tools and horizontal scrolling ------------------------------
    void setTool (Tool newTool);
    Tool getTool() const noexcept { return tool; }

    /** Phase 19: the hand-drawn pointer artwork, as an image plus its hotspot.

        Public and static so the dev harness can render it into a PNG: "the tip
        is off the body's centreline" is a visual claim, and it is exactly the
        kind of thing that ships unnoticed if nobody can look at it. */
    static juce::Image renderToolCursorImage (Tool which, juce::Point<int>& hotspot);

    /** Phase 18: the snap toggle belongs to the session, not to this component.
        Fired with the new state so the automation window can obey the same
        switch — one grid, one answer to "does it snap". */
    std::function<void (bool)> onSnapChanged;

    /** Phase 18: the user clicked the ruler. The transport belongs to the host,
        so the roll only reports which beat was asked for. */
    std::function<void (double)> onLocateRequested;

    /** Phase 19: this pitch should sound right now, on this synth.

        The roll knows which note is being drawn or dragged and which synth it
        resolves to; only the host knows a sound card exists. `retrigger` is
        false for the continuous part of a drag, so moving a note up three
        semitones is one sound sliding rather than three blips. */
    std::function<void (int slotId, int pitch, float velocity, bool retrigger)> onAuditionRequested;

    double getScrollBeats() const noexcept { return scrollBeats; }
    void   setScrollBeats (double beats);

    /** How many beats fit in the grid at the current width. */
    double getVisibleBeats() const;

    /** How long the roll currently is, in beats. Starts at totalBeats and grows
        in beatChunk steps as the view or a note reaches past the end. */
    double getTimelineBeats() const noexcept { return timelineBeats; }

    /** Drops the timeline back to its default length. Called when a session is
        built: the roll grew to fit the last project, and a new project must not
        inherit that — the ruler would show an hour of empty bars. */
    void resetTimeline();

    /** Last beat occupied by any note, on any layer. */
    double getContentEndBeat() const;

    /** The note area in local coordinates (no keyboard, ruler, toolbar or
        scrollbar). Public because the host and the self-test both need to know
        where "the music" starts. */
    juce::Rectangle<float> getGridBounds() const { return getGridArea(); }

    //-- Phase 8: the inline note automation box ------------------------------
    /** Every note carries a small "v" handle at its tail. Clicking it opens a
        curve editor UNDER the note, as wide as the note itself, with a menu at
        the bottom that selects which parameter that curve drives.

        Addressed by (layer, noteId), never by pointer: notes live in a vector
        and any add/remove can move them. */
    bool   isNoteAutomationOpen() const noexcept { return expandedNoteId >= 0; }
    int    getExpandedNoteId() const noexcept    { return expandedNoteId; }
    int    getExpandedLayerIndex() const noexcept { return expandedLayerIndex; }

    void closeNoteAutomation();

    /** Opens the inline box on a specific note (used by the demo snapshot and
        by anything that wants to jump the user straight to a curve). */
    void openNoteAutomation (int layerIndex, int noteId);

    /** Local-space geometry of the tail handle and the box it opens. Empty when
        that note does not exist, is not on the active layer, or is too narrow
        to carry a handle. */
    juce::Rectangle<float> getNoteAutomationHandleBounds (const NoteEvent&) const;
    juce::Rectangle<float> getNoteAutomationBoxBounds (const NoteEvent&) const;

    //-- Phase 8: settings the gear menu drives -------------------------------
    /** When off, drawing and dragging land on the raw mouse position instead of
        the 1/16 grid. */
    void setSnapEnabled (bool shouldSnap);
    bool isSnapEnabled() const noexcept { return snapEnabled; }

    /** 0.12 .. 0.75. Ghost layers are the one visual the user tunes per screen. */
    void  setGhostAlpha (float alpha);
    float getGhostAlpha() const noexcept { return ghostAlphaValue; }

    //-- Phase 11: draw with a copied state ----------------------------------
    /** Takes the note state (length, velocity, curve, curve target, synth
        override) from the first selected note and makes it the pencil's
        template. Pitch and position never come from the template — they come
        from where you click.

        This is the "画笔状态" button in the tool strip. Returns false when
        nothing is selected. */
    bool copyNoteStateFromSelection();

    /** Forgets the template: the pencil goes back to 1 beat at velocity 0.8. */
    void clearPencilState();

    bool hasPencilState() const noexcept { return hasPaintState; }

    /** The template, for UI readouts and the self-test. */
    double getPencilStateLengthBeats() const noexcept { return paintState.lengthBeats; }
    float  getPencilStateVelocity() const noexcept    { return paintState.velocity; }

    //-- Editing actions ------------------------------------------------------
    /** Creates a note at (beat, pitch) exactly as the pencil would: snapped,
        carrying the current paint state, and extending the timeline when it
        lands past the end. Returns the new id, or -1 when there is no editable
        layer. Used by mouseDown AND by the self-test, so the two cannot drift. */
    int addNoteAt (double beat, int pitch);

    /** Copies the selected notes, keeps their envelope, selects the copies.
        Returns the new note ids. offsetBeats = 0 duplicates in place. */
    std::vector<int> duplicateSelectedNotes (double offsetBeats = 1.0);

    void setSelection (std::vector<int> newSelection);
    std::vector<int> getSelection() const { return selection; }

    /** The synth the selection resolves to, or -1 when nothing is selected or
        the selection is split across synths. Reads the same routing::resolve()
        the engine uses, so the rack cannot highlight a synth that is not the
        one you hear. */
    int  getSelectedRoutingSlotId() const;

    /** True when every selected note sits on one synth AND at least one of them
        got there through a per-note override. */
    bool selectionUsesOverride() const;

    /** Re-points the whole selection at one synth (or back to the layer default
        when slotId < 0). */
    void routeSelectionTo (int slotId);

    //-- Phase 16: note groups ------------------------------------------------
    /** Turns the selection into a group. Nothing moves: a group is a new way to
        address notes that already exist, not a container they move into.

        Returns the new group id, or -1 when nothing is selected. */
    int  groupSelection();

    /** The group every selected note belongs to, or -1 when the selection is
        empty or spans more than one group. */
    int  getSelectedGroupId() const;

    /** Selects every member of a group, so "drag the group" is just "drag the
        selection" — one drag implementation, two ways to fill it. */
    void selectGroup (int groupId);

    /** Detaches the selected notes from their group(s). The group entity stays
        as long as it still has members. Returns true when anything changed. */
    bool ungroupSelection();

    /** Dissolves a whole group: every member detaches, the id disappears. */
    bool disbandGroup (int layerIndex, int groupId);

    /** Mounts the selection's group on a synth (or detaches it when slotId < 0).
        A selection of ungrouped notes is ignored: mounting is a group action,
        per-note routing already exists. */
    bool setGroupOutputForSelection (int slotId);

    /** Local-space label of a group, which is also its click target. Empty when
        the group has no member on screen. */
    juce::Rectangle<float> getGroupLabelBounds (int layerIndex, int groupId) const;

    /** The group whose label is under this point, or -1. */
    int getGroupLabelAt (juce::Point<float> position) const;

    /** Fired when a group disappears, so the host can drop its automation
        track. The model cannot do that itself: it does not own the window. */
    std::function<void (int groupId)> onGroupRemoved;

    /** Fired by the group's context menu. The host owns the automation window;
        `target` is a GroupAutomationTarget (0 = 音量, 1 = 声相), because a group
        now has two curves and the menu has to say which one it means. */
    std::function<void (int layerIndex, int groupId, int target)> onGroupAutomationRequested;

    /** Fired by the group's context menu when a synth is chosen: the host asks
        which synth the user picked and mounts the group on it. */
    std::function<void (int layerIndex, int groupId, juce::Point<int> screenPos)> onGroupMountRequested;

    //-- Phase 17: audio clips ------------------------------------------------
    /** A sample dropped anywhere on the grid becomes a block in the ACTIVE
        layer: the block covers a whole number of pitch lanes and its width is
        `lengthBeats * beatWidth`, so it scales with the grid by construction.

        Returns the new clip id, or -1 when there is no editable layer. A file
        that cannot be decoded is reported through onSampleLoadFailed and
        nothing is added. */
    int addSampleAt (double beat, int pitch, const juce::File& file);

    /** Local-space bounds of a clip block. Empty when it does not exist. */
    juce::Rectangle<float> getSampleScreenBounds (int layerIndex, int sampleId) const;

    /** The clip under this point on the ACTIVE layer, or -1. Ghost layers are
        as click-through for clips as they are for notes. */
    int getEditableSampleIdAt (juce::Point<float> position) const;

    /** How many clips the active layer holds. The self-test reads it. */
    int getActiveLayerSampleCount() const;

    /** Fired when a file could not be decoded, so the host can say so. */
    std::function<void (const juce::String& message)> onSampleLoadFailed;

    //-- DragAndDropTarget: files dropped from the browser --------------------
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    //-- Phase 16: the focused synth -----------------------------------------
    /** Lights up everything mounted on one synth: its notes, its groups, and
        (through the layer panel) the sheets that feed it. -1 clears it.

        Set by clicking a rack card. Draw-only state — no routing changes. */
    void setFocusSlotId (int slotId);
    int  getFocusSlotId() const noexcept { return focusSlotId; }

    /** How many notes currently resolve to the focused synth. The self-test
        reads this to prove the highlight means something. */
    int getFocusedNoteCount() const;

    /** True when a group's notes reach the focused synth, either through the
        group's own mount or through a note-level override inside it. */
    bool isGroupFocused (int layerIndex, int groupId) const;

    //-- Transport ------------------------------------------------------------
    /** Transport playhead, in beats. Negative hides it. */
    void setPlayheadBeat (double beats);

    /** Tempo, for the ruler's time column and the one-second grid. The roll
        still lays out in beats — that is what keeps the musical grid stable —
        but the second markers spread out or tighten as this changes. */
    void setBpm (double newBpm);
    double getBpm() const noexcept { return bpm; }

    /** Whether the playhead should drive the notes' envelope animation.
        True while playing and while paused; false after Stop, when the playhead
        is parked at zero and nothing is sounding. */
    void setTransportActive (bool shouldBeActive);
    bool isTransportActive() const noexcept { return transportActive; }

    /** How far the playhead has travelled through a note, 0..1, or -1 when it
        is outside it.

        Static and pure so the drawing code and the headless self-test cannot
        disagree about what "the note is sounding" means — the animation is
        driven by exactly the function the test checks. */
    static double envelopeProgress (const NoteEvent& note, double playheadBeat);

    /** Same thing for a note looked up by id, against the current playhead. */
    double getPlayheadProgress (int layerIndex, int noteId) const;

    //-- Component ------------------------------------------------------------
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    //-- Listeners -------------------------------------------------------------
    void layersChanged() override;
    void notesChanged() override;
    void rackChanged() override;

    /** Phase 20: local bounds of toolbar button `index`.

        Public because the self-test uses it to prove the row really fits: a
        button that overlaps its neighbour is invisible in a screenshot and
        obvious only when a click does the wrong thing. */
    juce::Rectangle<int>   getToolButtonBounds (int index) const;
    juce::Rectangle<int>   getToolbarArea() const;

private:
    enum class DragMode { none, moveNote, resizeNote, resizeNoteLeft, marquee, panView,
                          scrollThumb, automationPoint, moveSample, resizeSample,
                          rulerScrub, rulerZoom };

    //-- Geometry helpers -----------------------------------------------------
    juce::Rectangle<float> getGridArea() const;
    juce::Rectangle<int>   getRulerArea() const;
    juce::Rectangle<int>   getKeyboardArea() const;
    juce::Rectangle<int>   getScrollBarArea() const;
    juce::Rectangle<float> getScrollThumb() const;

    static float pitchToContentY (int pitch) noexcept;
    static int   contentYToPitch (float y) noexcept;
    float  screenYForPitch (int pitch) const noexcept;
    int    pitchAtScreenY (float y) const noexcept;
    float  screenXForBeat (double beat) const noexcept;
    double beatAtScreenX (float x) const noexcept;

    juce::Rectangle<float> getNoteBounds (const NoteEvent&) const;

    /** Only ever looks at the active, visible layer. Ghost notes are invisible
        to the mouse by construction, not by an if-check at the call site. */
    NoteEvent* hitTestActiveLayer (juce::Point<float> position) const;

    static double snapBeat (double beats) noexcept;
    static bool   isBlackKey (int pitch) noexcept;

    /** Phase 18: the pointer has to say what the button does. A pencil cursor
        over a pencil tool is the cheapest possible confirmation that the mode
        changed, and a hand over a hand tool stops "why did it pan?" questions. */
    void updateCursor();
    void rebuildToolCursors();

    //-- Phase 19: audition ---------------------------------------------------
    /** Sounds one note on whatever the routing says plays it. Silent when the
        host has not wired the callback (the headless self-test). */
    void auditionNoteOnActiveLayer (const NoteEvent& note, bool retrigger);

    //-- Selection ------------------------------------------------------------
    bool isSelected (int noteId) const;
    void toggleSelection (int noteId);

    //-- Drag ----------------------------------------------------------------
    void captureDragOriginals();

    //-- Painting -------------------------------------------------------------
    void drawPitchRows (juce::Graphics&);
    void drawTimeGrid (juce::Graphics&);
    void drawGridLines (juce::Graphics&);
    void drawNotes (juce::Graphics&);

    //-- Phase 17: audio clips ------------------------------------------------
    void drawSamples (juce::Graphics&);
    void drawSampleBlock (juce::Graphics&, int layerIndex, const SampleEvent&,
                          juce::Colour, float alpha);
    void drawDropPreview (juce::Graphics&);

    /** The clip the mouse is over, on the active layer. */
    SampleEvent* hitTestSample (juce::Point<float> position) const;

    /** Right-click on a clip: delete, duplicate, fit to source, trim. */
    void showSampleMenu (int sampleId, juce::Point<int> screenPos);
    void drawNote (juce::Graphics&, const NoteEvent&, juce::Colour,
                   float alpha, bool selected, bool hovered, double progress);

    //-- Phase 16: group marks and the focused-synth spotlight ----------------
    /** Behind the notes: a tinted band plus a bracket around every group. */
    void drawGroupBands (juce::Graphics&);
    /** Above the notes: the clickable name pill of every group. */
    void drawGroupLabels (juce::Graphics&);
    /** Above everything: a ring in the synth's colour around every note that
        resolves to the focused synth. Drawn last so the dim veil underneath
        cannot swallow it. */
    void drawFocusHighlights (juce::Graphics&);

    /** Bounding box of a group's visible members, local space, empty when none
        are on screen. The single source of the band, the label and the hit box. */
    juce::Rectangle<float> getGroupContentBounds (const Layer&, const NoteGroup&) const;

    void showGroupMenu (int layerIndex, int groupId, juce::Point<int> screenPos);
    /** Right-click anywhere else in the grid: group actions for the current
        selection, disabled when there is nothing they could act on. */
    void showCanvasMenu (juce::Point<int> screenPos);

    /** Which sheet owns a group. Group ids are session-unique, so this is a
        scan, not a map — and it can simply fail (a dissolved group). */
    int layerIndexOfGroup (int groupId) const;

    /** Drops every group that lost its last member. Deleting notes is allowed
        to empty a group; leaving the empty shell behind would put a nameless
        row in the automation window and a label nowhere. */
    void pruneEmptyGroups();
    void drawMarquee (juce::Graphics&);
    void drawPlayhead (juce::Graphics&);
    void drawRuler (juce::Graphics&);
    void drawKeyboard (juce::Graphics&);
    void drawToolbar (juce::Graphics&);
    void drawScrollBar (juce::Graphics&);
    void drawToolIcon (juce::Graphics&, int toolIndex, juce::Rectangle<float> area,
                       juce::Colour colour);

    /** Grows the timeline so `beats` fits, in beatChunk steps and never by more
        than four chunks per call (so one runaway scroll cannot allocate five
        hours of ruler). */
    void ensureTimelineExtendsTo (double beats);
    void drawNoteAutomationBox (juce::Graphics&);

    /** The parameter's icon, used by the narrow variant of the box's bottom
        row. Text does not fit in 12 px; a shape does. */
    void drawModTargetIcon (juce::Graphics&, NoteModTarget, juce::Rectangle<float>,
                            juce::Colour) const;
    void notifySelectionChanged();

    //-- Phase 8: the inline box ----------------------------------------------
    juce::Rectangle<float> getAutomationCurveArea (const NoteEvent&) const;
    juce::Rectangle<float> getAutomationMenuArea (const NoteEvent&) const;
    juce::Rectangle<float> getAutomationCloseArea (const NoteEvent&) const;

    /** The note the inline box is currently pointed at, or null. Never cached. */
    NoteEvent* getExpandedNote() const;

    void showModTargetMenu (const NoteEvent&, juce::Point<int> screenPosition);
    bool handleAutomationMouseDown (const juce::MouseEvent&, NoteEvent&);
    void dragAutomationPoint (NoteEvent&, juce::Point<float> position);
    int  findEnvelopePointAt (const NoteEvent&, juce::Point<float>) const;

    void drawCurveInBox (juce::Graphics&, const NoteEvent&, juce::Colour,
                         juce::Rectangle<float> inner);

    /** Notes the playhead was inside before the last move, so the frames where
        a note starts or stops sounding can repaint that whole block instead of
        leaving a stale highlight behind. */
    void collectSoundingNotes (double beat, std::vector<juce::int64>& out) const;

    void clampScroll();

    //-- State ----------------------------------------------------------------
    LayerStack& stack;
    RackModel&  rack;

    std::vector<int>       selection;      // note ids, always of the active layer
    std::vector<NoteEvent> dragOriginals;  // snapshot taken on mouseDown

    DragMode dragMode        = DragMode::none;
    double   dragAnchorBeat  = 0.0;
    int      dragAnchorPitch = 0;
    juce::Point<float> marqueeStart, marqueeCurrent;

    float  scrollY     = 0.0f;
    double scrollBeats = 0.0;
    double timelineBeats = totalBeats;
    int    hoveredNoteId   = -1;
    int    lastActiveLayer = -1;
    double playheadBeat    = -1.0;
    double bpm             = 120.0;
    bool   transportActive = false;

    //-- Phase 7 --------------------------------------------------------------
    Tool   tool = Tool::select;
    int    hoveredTool = -1;
    double scrollDragAnchorBeats = 0.0;
    float  scrollDragAnchorX     = 0.0f;
    juce::Point<float> panAnchor;
    float  panAnchorScrollY = 0.0f;
    double panAnchorBeats   = 0.0;

    //-- Phase 18 -------------------------------------------------------------
    /** Built lazily: a custom cursor is an OS handle, and creating three of
        them in a constructor that a headless --snapshot also runs is pointless
        work for a window nobody is pointing at. */
    juce::MouseCursor pencilCursor, eraserCursor, handCursor;
    bool   cursorsBuilt = false;
    float  rulerDragAnchorY   = 0.0f;
    double rulerDragAnchorBeatWidth = defaultBeatWidth;

    //-- Phase 19 -------------------------------------------------------------
    /** The pitch the current drag has already sounded. Dragging sideways with
        no pitch change must not machine-gun the same note. */
    int    dragAuditionPitch = -1;

    //-- Phase 8 --------------------------------------------------------------
    int    expandedNoteId     = -1;
    int    expandedLayerIndex = -1;
    int    dragEnvelopePoint  = -1;
    int    hoveredEnvelopePoint = -1;
    bool   hoveredAutomationMenu = false;
    bool   snapEnabled          = true;
    float  ghostAlphaValue      = ghostAlpha;

    //-- Phase 11 -------------------------------------------------------------
    /** What the pencil stamps. A whole NoteEvent minus its pitch and start: the
        value-type note makes "copy the state" a single assignment, exactly like
        duplicate does. */
    NoteEvent paintState;
    bool      hasPaintState = false;

    /** Note ids the playhead was inside at the last update, across all visible
        layers, packed as (layerIndex << 32 | noteId) — ids are only unique
        within a layer, so a bare id would collide between sheets. Only used to
        work out the dirty region. */
    std::vector<juce::int64> soundingNotes;

    //-- Phase 16 -------------------------------------------------------------
    /** The synth everything is lit up for, or -1. Deliberately NOT derived from
        the selection on every paint: clicking a card must keep working while
        nothing is selected, which is the whole point of inspecting a synth. */
    int focusSlotId = -1;
    int hoveredGroupId = -1;
    int hoveredGroupLayer = -1;

    //-- Phase 17 -------------------------------------------------------------
    /** The clip being moved or trimmed, and its state when the drag began. */
    int        dragSampleId   = -1;
    SampleEvent dragSampleOriginal;
    juce::Point<float> sampleDropPoint;
    bool       sampleDropActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};

} // namespace atomic

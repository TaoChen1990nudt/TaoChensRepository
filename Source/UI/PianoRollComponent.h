#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include <functional>
#include <vector>

#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/PluginParam.h"
#include "Model/Routing.h"

#include "Audio/ClipStretch.h"

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
                                 public juce::DragAndDropTarget,
                                 private juce::Timer
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

    /** Phase 31: sets the zoom while pinning ONE beat at ONE pixel column.

        `anchorX` is a local x; `anchorBeat` is the beat that was under it when
        the gesture began. Everything else follows: `scrollBeats` is solved so
        the beat lands back on the same column.

        The anchor has to be captured by the CALLER, before the width changes,
        for the same reason the pan and the scrollbar drags capture theirs: a
        scrub-able view has no stable reference point, so "where the mouse went
        down" must be a value, not a lookup. Re-deriving it from `anchorX` on
        every mouse-move would let a clamped scroll quietly walk the anchor
        across the timeline, one notch at a time. */
    void setBeatWidthAround (float pixels, double anchorBeat, float anchorX);

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

    static constexpr int    numToolButtons = 8;      // 4 tools + copy-state + group + ungroup + snap

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

    /** Phase 24: how the inline boxes find out what a HOSTED instrument can
        automate. The session owns the plugin host, so it supplies the answer;
        with no lister installed the menus simply list the built-in targets and
        the app behaves exactly as it did before this phase. */
    void setPluginParamLister (PluginParamLister lister) { listPluginParams = std::move (lister); }

    /** The parameters of the synth a slot holds, or empty when that slot is an
        internal synth / gone. Message thread only. */
    std::vector<PluginParamInfo> pluginParamsForSlot (int slotId) const;

    /** The synth slot a note plays through, resolved the same way the engine
        resolves it. -1 when the rack is empty. */
    int resolvedSlotForNote (const NoteEvent&) const;

    /** The synth slot a group is mounted on: its own mount when it has one,
        otherwise the layer default — the same two tiers routing::resolve uses
        for its members, one level up. -1 when the rack is empty. */
    int resolvedSlotForGroup (int layerIndex, int groupId) const;

    /** The name shown in a box's parameter cell: the plugin parameter's name
        when the curve drives one, otherwise the built-in target's label. */
    juce::String noteTargetLabel (const NoteEvent&) const;
    juce::String groupTargetLabel (const NoteGroup&) const;
    juce::String pluginParamName (int slotId, int paramIndex) const;

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

    //-- Phase 21: the same idea, for a group --------------------------------
    /** Every group's name pill carries a handle. Clicking it opens a curve
        editor BELOW the group, as wide as the group, driving either the group's
        level or its pan.

        Opening one box closes the other: two floating editors over the same
        staff would hide the notes they are both editing. */
    bool isGroupAutomationOpen() const noexcept { return expandedGroupId >= 0; }
    int  getExpandedGroupId() const noexcept    { return expandedGroupId; }
    int  getExpandedGroupLayerIndex() const noexcept { return expandedGroupLayerIndex; }
    int  getExpandedGroupTarget() const noexcept { return expandedGroupTarget; }

    void openGroupAutomation (int layerIndex, int groupId, int target = 0);
    void closeGroupAutomation();

    /** Local-space geometry of the pill's handle and the box it opens. Empty
        when the group is not on a shown layer or has no notes in view. */
    juce::Rectangle<float> getGroupAutomationHandleBounds (int layerIndex, int groupId) const;
    juce::Rectangle<float> getGroupAutomationBoxBounds (int layerIndex, int groupId) const;

    /** The curve area inside the box: the box minus its bottom row, with NO
        horizontal padding. The curve's first and last pixels are the box's
        first and last pixels, because a curve that stops short of the notes it
        shapes reads as "the automation ends here" when it does not. */
    juce::Rectangle<float> getGroupCurveBounds() const;

    //-- Phase 22: playable keyboard -----------------------------------------
    /** Clicking a key on the piano column sounds that pitch through the ACTIVE
        layer's output route — the same synth its notes play on, at the same
        velocity default. Dragging across the keys glissandos; releasing leaves
        the short preview envelope to finish on its own.

        `retrigger` = false keeps the voice's phase, which is what makes a
        glissando one sliding sound instead of a row of clicks. */
    void auditionPitchOnActiveLayer (int pitch, bool retrigger = true);

    /** The key currently held down, or -1. Draw-only state. */
    int getPressedKeyPitch() const noexcept { return pressedKeyPitch; }

    //-- Phase 22: the curve clipboard ---------------------------------------
    /** Which inline box a copy/paste would act on.

        "The box that is open" is NOT the same thing: a box stays open while you
        click around to compare curves, and copy has to have one unambiguous
        target. Clicking inside a box selects it; clicking the canvas drops the
        selection but leaves the box open. */
    enum class CurveSelection { none, noteEnvelope, groupLane };

    CurveSelection getCurveSelection() const noexcept { return curveSelection; }

    bool canCopyAutomationCurve() const;
    /** Copies the selected box's curve into the roll's clipboard, in a form that
        does not remember which box it came from: beat offsets from the source's
        start plus the source's length. One representation, so a note curve can
        be pasted onto a group and vice versa. */
    bool copyAutomationCurve();

    /** Cuts: copies, then flattens the selected lane (level 1, pan 0.5) —
        not zero, because "emptied" and "silent" are different states. */
    bool cutAutomationCurve();

    bool canPasteAutomationCurve() const;
    /** Pastes the clipboard into the selected box.

        `stretch` = true rescales the curve to the target's length. false keeps
        the source's length: a target that is shorter truncates the curve, a
        longer one plays the curve and then holds 0. */
    bool pasteAutomationCurve (bool stretch);

    bool   hasCurveClipboard() const noexcept { return ! curveClip.points.empty(); }
    double getCurveClipboardLengthBeats() const noexcept { return curveClip.lengthBeats; }
    int    getCurveClipboardPointCount() const noexcept { return (int) curveClip.points.size(); }

    /** "复制的曲线 8.00 拍 / 目标 4.00 拍" — what the stretch question is about. */
    juce::String describeCurvePaste() const;

    //-- Phase 22: the note clipboard (Edit menu) -----------------------------
    bool copyNotesToClipboard();
    bool cutNotesToClipboard();
    bool pasteNotesFromClipboard();
    bool canPasteNotes() const noexcept { return ! noteClip.empty(); }
    int  getNoteClipboardSize() const noexcept { return (int) noteClip.size(); }

    /** Phase 24: was the last copy a whole group (so a paste makes a second
        group) rather than a handful of notes? The self-test reads it; the edit
        menu's status line uses it to say which one happened. */
    bool isGroupClipboard() const noexcept { return groupClip.valid; }

    /** The copied group's name, or empty when the clipboard holds loose notes.
        Read by the self-test and by the edit menu's status line. */
    juce::String getGroupClipboardName() const { return groupClip.valid ? groupClip.name : juce::String(); }

    /** Phase 24: how many lanes the last pasted/copied group carried switched
        on. Not the point of the feature, but it is the one number that proves
        the group arrived with its curves and not just its notes. */
    int getGroupClipboardLiveLanes() const
    {
        int count = 0;

        for (bool on : groupClip.laneOn)
            count += on ? 1 : 0;

        return count;
    }

    /** A one-line message in the tool strip, gone after a moment. Covers the
        silent verbs: copy puts nothing on screen, and a paste that lands
        off-screen looks like nothing happened. */
    void flashStatus (const juce::String& text);

    /** Drawn in the tool strip right now, or empty. For the self-test. */
    juce::String getStatusText() const { return statusText; }

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

    //-- Phase 22: dragging a selection --------------------------------------
    /** How far a rigid multi-note drag may move.

        One rule, one home: the leftmost member decides where the block stops,
        and every member moves by the same delta so the intervals survive. The
        vertical clamp works the same way. Public because the self-test has to
        prove the rule without synthesising mouse events. */
    struct MoveClamp
    {
        double beats = 0.0;
        int    pitch = 0;
    };

    MoveClamp clampMove (const std::vector<NoteEvent>& originals, double rawBeats,
                         int rawPitch, bool snap,
                         const std::vector<SampleEvent>& clips = {}) const;

    /** Phase 27: the whole arithmetic of "move what is armed", in one place.

        A group can now hold clips as well as notes, and both have to move by
        the same delta or the bracket visibly tears apart mid-drag. Keeping the
        application next to the clamp means the self-test can drive a group move
        without synthesising mouse events, and there is no second copy of
        "startBeat + delta" to get out of step. */
    void applyMove (const MoveClamp& move);

    /** Phase 27: test hook — arms and performs a rigid move of a group. */
    bool simulateGroupMove (int layerIndex, int groupId, double deltaBeats, int deltaPitch);

    /** Phase 27: the frame a group draws around its members, notes AND clips.
        Public so a test can assert "the clip is inside the bracket" without
        synthesising the paint pass. Empty when the group has no visible member. */
    juce::Rectangle<float> groupFrameOf (int layerIndex, int groupId) const;

    /** Phase 24: the beat a group-curve point lands on.

        Public for the same reason clampMove is: "the curve snaps to the grid"
        is a claim about one function, and a test that has to synthesise mouse
        events to check it would be checking the mouse instead. */
    double snapCurveBeat (double beat, juce::Range<double> span) const;

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

    /** Phase 23: "put what I have selected on this synth", where the selection
        may be a whole group.

        A group is mounted as a GROUP — `group.outputSlotId`, every member's own
        override cleared — because the alternative (writing the same override
        onto twelve notes) leaves a group that visibly says it feeds synth A
        while half of it is still on synth B. Loose notes take the per-note
        override, which is what routeSelectionTo() already does.

        Returns true when something was re-routed. */
    bool mountSelectionTo (int slotId);

    //-- Phase 16: note groups ------------------------------------------------
    /** Turns the selection into a group. Nothing moves: a group is a new way to
        address notes that already exist, not a container they move into.

        Returns the new group id, or -1 when nothing is selected. */
    int  groupSelection();

    /** The group every selected note belongs to, or -1 when the selection is
        empty or spans more than one group. */
    int  getSelectedGroupId() const;

    /** Phase 24: does the selection cover EVERY member of this group?

        The difference between "the group" and "a few notes out of the group"
        decides whether an action is a group action. Mounting the whole group
        when only one of its eight notes is selected is precisely what makes
        "change this one note's synth, leave the rest alone" impossible — and
        copy/paste keys off the same question, because copying a group has to
        copy the group rather than its members. */
    bool isWholeGroupSelected (int groupId) const;

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

    /** Phase 25: test hook — one resize step of the gesture a drag would make,
        against the current selection. Exists because a synthetic MouseEvent
        cannot be built without a peer: this drives the same
        resizeTargetId()/applyNoteResize() pair the drag does. */
    bool simulateNoteResize (int noteId, double wantedBeat, bool fromTail = true);

    /** Phase 25: the group on the ACTIVE layer whose frame (the translucent
        bracket drawn around its members) contains this point, or -1. Notes are
        hit-tested first, so this only answers for the empty part of the frame —
        which is exactly where a user grabs to move the group as one object. */
    int getGroupFrameAt (juce::Point<float> position) const;

    /** Fired when a group disappears, so the host can drop its automation
        track. The model cannot do that itself: it does not own the window. */
    std::function<void (int groupId)> onGroupRemoved;

    /** Fired by the group's context menu. The host owns the automation window;
        `target` is a GroupAutomationTarget (0 = 音量, 1 = 声相, 2 = 音源参数),
        because a group has three curves and the menu has to say which one it
        means. */
    std::function<void (int layerIndex, int groupId, int target)> onGroupAutomationRequested;

    /** Fired by the group's context menu when a synth is chosen: the host asks
        which synth the user picked and mounts the group on it. */
    std::function<void (int layerIndex, int groupId, juce::Point<int> screenPos)> onGroupMountRequested;

    //-- Phase 50: bounce a scope into a sample block -------------------------
    /** What the user asked to render. A small POD instead of three overloads
        because the host's renderer needs the same three answers either way, and
        because "selection" is a list of ids that must be copied the moment the
        menu closes — the selection can change while the render runs. */
    struct RenderScopeRequest
    {
        int layerIndex = -1;
        int groupId    = -1;                 // >= 0 for "this group"
        bool wholeLayer = false;             // otherwise the id lists below
        std::vector<int> noteIds;            // only for the selection case
        std::vector<int> sampleIds;          // ditto — a mixed selection renders both

        bool isSelection() const noexcept { return ! wholeLayer && groupId < 0; }
    };

    /** Fired by the canvas / group menus: "render this to a block". The host
        owns the renderer, the sample library and the layer, so it does the
        work; the roll only says what was asked for. */
    std::function<void (const RenderScopeRequest&)> onRenderScopeRequested;

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

    /** Phase 27: the buffer a clip actually plays — its sampler's, or its own
        when it is not mounted. Routing::resolveSample is the rule; this is the
        UI's one call into it, so the drawn waveform cannot disagree with what
        the engine renders. */
    std::shared_ptr<const SampleBuffer> clipAudio (const Layer&, const SampleEvent&) const;

    /** The clip under this point on the ACTIVE layer, or -1. Ghost layers are
        as click-through for clips as they are for notes. */
    int getEditableSampleIdAt (juce::Point<float> position) const;

    /** How many clips the active layer holds. The self-test reads it. */
    int getActiveLayerSampleCount() const;

    /** Fired when a file could not be decoded, so the host can say so. */
    std::function<void (const juce::String& message)> onSampleLoadFailed;

    //-- Phase 28: selecting, moving, copying clips ---------------------------
    /** The clips selected on the active layer.

        Phase 29 replaced "clips or notes, never both" with "a click replaces
        the whole selection, a marquee may build a mixed one". A plain click on
        a block still lets go of the notes (and the other way round) because
        that is what a click means; the marquee and Shift-click are how you say
        "both", and every command that follows handles both. */
    void setSampleSelection (std::vector<int> ids);
    std::vector<int> getSampleSelection() const { return sampleSelection; }
    bool isSampleSelected (int sampleId) const;
    void toggleSampleSelection (int sampleId);

    /** Phase 38: "there is something selected" — the only correct question for
        a toolbar control that acts on the selection, and the one the self-test
        checks.

        Phase 35 taught 编组/解组 and the G key that a group is "the members",
        not "the notes", but the two toolbar BUTTONS kept asking about notes
        alone: with a clip selected they stayed greyed out and did nothing,
        while G actually worked. One predicate, four call sites (two clicks,
        two paints), so the pair cannot drift apart again. */
    bool hasAnySelection() const noexcept
    {
        return ! selection.empty() || ! sampleSelection.empty();
    }

    /** Phase 29: replaces BOTH selections in one go — what the marquee needs.
        Emptying one side here really means empty; see setSelection for why a
        plain click cannot use this. */
    void setMixedSelection (std::vector<int> noteIds, std::vector<int> sampleIds);

    /** Deletes every selected clip. Returns true when anything went. */
    bool deleteSelectedSamples();

    /** Phase 29: deletes every selected note. Returns true when anything went.

        Public because Delete and Cut are two callers and the pruning of emptied
        groups has to happen exactly once — a second implementation in the host
        is how a group bracket ends up surviving its last member. */
    bool deleteSelectedNotes();

    /** Phase 32: deletes every clip mounted on `slotId`, on every layer.
        The other half of the reference rule — the rack removed the sampler, so
        the blocks that have nowhere left to play go with it. Returns how many
        were removed. */
    int deleteSamplesMountedOn (int slotId);

    /** Phase 34: how many clips would `deleteSamplesMountedOn()` take.

        Phase 34 put a question in front of both cascade deletions, and a
        question has to state the cost. Same walk over the same three levels as
        the deleter, so the number in the dialog and the number of blocks that
        actually disappear cannot disagree. */
    int countSamplesMountedOn (int slotId) const;

    /** Phase 34: the names of the clips mounted on `slotId`, for that dialog.
        At most `limit` of them — a prompt that lists forty block names is a
        prompt nobody reads. */
    juce::StringArray namesOfSamplesMountedOn (int slotId, int limit) const;

    /** Phase 32: re-mounts the SELECTION and makes each block hold that
        sampler's sample.

        Phase 27 made the mount a routing-only decision: the block kept its own
        window while the sound changed. That was right for "listen to this file
        through that sampler", but it is not what the rack implies — a slot is a
        piece of gear that HOLDS a file, so pointing a block at another one has
        to hand the block that file. Name, path, length and offset follow;
        `startBeat` does not, because choosing another sample is not a request to
        move the music.

        `slotId` < 0 means "follow the layer output": the block keeps the audio
        it is heard through right now and stops naming a sampler. Returns false
        when there is nothing selected or the slot holds no file. */
    bool mountSelectedSamplesToSlot (int slotId);

    /** Phase 32: the sampler the selection is mounted on, -1 when nothing is
        selected or every selected block merely follows its layer. The rack uses
        it to light the card that owns the sample under the cursor. */
    int selectedSamplerSlotId() const;

    /** Fired after a clip was deleted or re-mounted.

        Phase 33: the host no longer TRUSTS this to be the moment the sweep runs.
        The rule "no clip, no sampler" is derived from the model now — every
        layersChanged / notesChanged notification sweeps — because nine tenths of
        the ways a block can disappear never call this. It is kept as an extra
        nudge for the two commands that change a mount without touching the layer
        contents, and it is harmless if it fires twice. */
    std::function<void()> onSampleReferencesChanged;

    /** Copies the selection in place, offset to the right — `offsetBeats` < 0
        means "by one clip length", which is what the context menu's "复制到后
        一小节" means. Selects the copies. */
    std::vector<int> duplicateSelectedSamples (double offsetBeats = -1.0);

    bool copySamplesToClipboard();
    bool cutSamplesToClipboard();
    bool pasteSamplesFromClipboard();
    bool canPasteSamples() const noexcept { return ! sampleClip.empty(); }
    int  getSampleClipboardSize() const noexcept { return (int) sampleClip.size(); }

    //-- Phase 29: the edit commands, over a possibly MIXED selection ---------
    /** Fills both clipboards from one gesture: notes into `noteClip`, clips into
        `sampleClip`, and empties whichever kind is not selected. Copying is the
        half that decides what a later paste can resurrect, so it must never
        leave the other kind's stale contents behind. */
    void copySelectionToClipboards();

    /** Pastes whatever both clipboards hold and leaves every pasted object
        selected. A copy of a mixed selection pastes as a mixed selection. */
    bool pasteEditClipboard();

    /** Ctrl+D over a mixed selection: notes shift by one beat, clips by their
        own length — each kind keeps the offset its own rule has always used. */
    void duplicateEditSelection();

    /** Phase 29: the longest this block may be — the source's remaining length
        after its start offset, in beats. Returns 0 when the audio is missing, in
        which case nothing is clamped (a block whose file is gone must still be
        draggable). */
    double maxSampleLengthBeats (const Layer&, const SampleEvent&) const;

    /** Phase 29: test hook — one trim step as the mouse would do it. Goes
        through the same code the drag uses, so the assertions cannot pass
        while the gesture is broken. */
    bool simulateSampleTrim (int sampleId, double wantedBeat, bool fromTail);

    //-- Phase 49: the clip's own automation box ------------------------------
    bool isSampleAutomationOpen() const noexcept { return expandedSampleId >= 0; }
    int  getExpandedSampleId() const noexcept    { return expandedSampleId; }
    void openSampleAutomation (int layerIndex, int sampleId);
    void closeSampleAutomation();

    /** Phase 49: test hook — adds (or drags onto) one point of the named
        clip's volume curve, normalised 0..1 across the block, through the
        same model path the box's gestures use. */
    bool simulateSampleCurveEdit (int sampleId, float t, float v);

    /** Phase 49: test hook — the box's local bounds, so the self-test can
        assert "as wide as the block" without re-deriving the layout. */
    juce::Rectangle<float> getSampleAutomationBoxForTest (int layerIndex,
                                                          int sampleId) const;

    /** Phase 50: test hook — the curve's own rectangle, so "the curve starts
        where the block starts" can be asserted instead of eyeballed. */
    juce::Rectangle<float> getSampleCurveAreaForTest (int layerIndex, int sampleId) const
    {
        return getSampleCurveArea (layerIndex, sampleId);
    }

    /** Phase 50: test hook — the same cut the menu item runs. Returns how many
        blocks were split. */
    int simulateSampleSplitAtPlayhead() { return splitSelectedSamplesAtPlayhead(); }

    /** Phase 50: test hook — the same reverse the menu item runs. */
    int simulateSampleReverse() { return toggleSelectedSampleReverse(); }

    /** Phase 30: pixels of a block that actually carry audio — the waveform's
        own width at the current zoom. Zero when the file is missing.

        This is the number the drawing code uses, exposed so the self-test can
        prove the waveform is CUT by a trim and not rescaled: the count depends
        on the file and the zoom, never on the block's length. */
    int sampleWaveColumnCount (int sampleId) const;

    /** Phase 30: puts the block back to "the whole file, starting where the audio
        starts". Restores the start beat the head trim ate as well, so the audio
        returns to the timeline position it had before the trim. Returns false
        when there is nothing to restore or no buffer to measure. */
    bool restoreSampleLength (int sampleId);

    /** Phase 31: the menu command proper — restores every selected block and
        returns how many it managed. A trim is a gesture on a 6 px band, so it is
        done to several blocks at once far more often than to one. */
    int restoreSelectedSampleLengths();

    //-- Phase 31: what a length change does to the sound ----------------------
    /** The two ratios this block's audio is played with right now, derived from
        its mode and its natural/drawn ratio exactly as the engine does it. One
        function, so the badge, the waveform and the renderer cannot disagree
        about how fast a block is running. */
    clipstretch::Playback samplePlayback (const Layer&, const SampleEvent&) const;

    /** Does this block's audio fill it edge to edge, or is it cut off?

        True for 变速变调 and 只变速不变调, where the whole remaining file is
        squeezed into the block, and false for the two modes that read the file
        at its own speed and therefore stop where the file ends. The waveform
        draws itself one way or the other, and this is the only place that
        decides which — a picture that "fits" while the sound is truncated (or
        the reverse) is the one thing a clip must never do. */
    bool sampleWaveFillsBlock (const Layer&, const SampleEvent&) const;

    /** Phase 31: test hook — sets one block's stretch mode through the same
        path the menu uses. */
    void setSampleStretchMode (int sampleId, StretchMode mode);

    /** Phase 31: applies a mode to the whole clip selection (or to one block
        when nothing is selected) and returns how many it changed. */
    int setSelectedSampleStretchMode (StretchMode mode);

    /** Phase 28: test hook — one rigid move step of the selected clips, through
        clampMove() + applyMove(), the same pair the drag uses. */
    bool simulateSampleMove (double deltaBeats, int deltaPitch);

    /** Phase 29: what a marquee over this rectangle grabs — notes in `.first`,
        clips in `.second`, both from the active layer. The mouse-up and the
        self-test call this same function, so a passing assertion is about the
        gesture and not about a copy of it. */
    std::pair<std::vector<int>, std::vector<int>>
        hitsInMarquee (juce::Rectangle<float> marquee) const;

    /** Phase 29: test hook — releases a marquee, i.e. selects everything in the
        rectangle. Returns true when it selected anything. */
    bool simulateMarquee (juce::Rectangle<float> marquee);

    /** Phase 31: test hook — one right-drag on the ruler, exactly as the mouse
        does it: capture the anchor from where the button went down, then apply
        the drag's own exponential step. The assertion is that the beat under
        `anchorX` does not move, so the hook must go through the same two calls
        the gesture does and not around them. */
    bool simulateRulerZoom (float anchorX, float totalDy);

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

    /** Phase 30: ends the spotlight if one is active. Returns true when it did.

        Called from the empty-grid click and from Esc. The veil is gone, but the
        highlight is still a mode, and a mode with no exit is a trap — this is
        the single function every exit runs through. */
    bool dismissSpotlight();

    /** How many notes currently resolve to the focused synth. The self-test
        reads this to prove the highlight means something. */
    int getFocusedNoteCount() const;

    /** True when a group's notes reach the focused synth, either through the
        group's own mount or through a note-level override inside it. */
    bool isGroupFocused (int layerIndex, int groupId) const;

    /** Phase 29: the way out of the spotlight.

        Clicking a rack card veils everything that is not on that synth. Until
        now nothing in the roll could undo it, so the view stayed grey until the
        user found the rack's own menu — and "the grid is dim and I cannot get
        out of it" is not a state a view should be able to get stuck in. The
        chip in the corner and Esc both go through here. */
    std::function<void()> onClearSpotlight;

    /** Where the "退出高亮" chip lives. Empty when nothing is spotlit. */
    juce::Rectangle<int> getSpotlightExitChipBounds() const;

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

    /** Phase 50: the two "render this to a block" items, in one place so the
        menu and the self-test read the same list. Ids 7 and 8 are the canvas
        menu's own numbering, and the selection item greys out when there is
        nothing selected.

        Public for the same reason as the toolbar bounds above: the self-test
        proves the items exist and that the first one disables itself with an
        empty selection, instead of trusting a screenshot nobody diffs. */
    static void addRenderScopeItems (juce::PopupMenu& menu, bool hasSelection);

private:
    enum class DragMode { none, moveNote, resizeNote, resizeNoteLeft, marquee, panView,
                          scrollThumb, automationPoint, resizeSample, resizeSampleLeft,
                          rulerScrub, rulerZoom, groupAutomationPoint, sampleCurvePoint };

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
    NoteEvent*       hitTestActiveLayer (juce::Point<float> position);
    const NoteEvent* hitTestActiveLayer (juce::Point<float> position) const;

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

    /** Phase 27: arms the group's clips for the same drag. Separate from
        captureDragOriginals because membership is stored in two places (on the
        note and on the clip) and the selection only ever holds note ids. */
    void captureGroupClips (int groupId);

    /** Phase 25: select a group and arm a rigid move of it. The pill and the
        frame both call this — one gesture, one implementation. */
    void armGroupMove (int groupId, juce::Point<float> position);

    /** Phase 25: which captured note a resize drag acts on. */
    int resizeTargetId() const;

    /** Phase 25: the ONE implementation of "this note now ends (or starts) at
        this beat". Both resize drags call it, and so does the self-test hook
        below — a second copy of this arithmetic is how a group's tails ended up
        in one line. */
    bool applyNoteResize (const NoteEvent& origin, double wantedBeat, bool fromTail);

    //-- Painting -------------------------------------------------------------
    void drawPitchRows (juce::Graphics&);
    void drawTimeGrid (juce::Graphics&);
    void drawGridLines (juce::Graphics&);
    void drawNotes (juce::Graphics&);

    //-- Phase 17: audio clips ------------------------------------------------
    void drawSamples (juce::Graphics&);
    void drawSampleBlock (juce::Graphics&, int layerIndex, const SampleEvent&,
                          juce::Colour, float alpha, bool selected);
    void drawDropPreview (juce::Graphics&);

    //-- Phase 49: the clip's own automation box ------------------------------
    /** The chevron on the block's body that opens the box. Same glyph, same
        "thing at the right end opens a curve" rule as the note's tail button
        and the group's pill handle. Empty when the block is too small to fit
        one, which is the note handle's own rule. */
    juce::Rectangle<float> getSampleAutomationHandleBounds (const SampleEvent&) const;

    /** The floating box: exactly as wide as the block, under it, flipped above
        when the block sits near the bottom of the view. */
    juce::Rectangle<float> getSampleAutomationBoxBounds (int layerIndex, int sampleId) const;

    /** Phase 50: the curve area ALIGNS with the block, pixel for pixel.

        P49 inset it inside the box by 10% on each side, which meant the curve's
        first point sat a few pixels right of the audio it shapes. At this zoom
        that is a beat of drift, and the one promise of an inline curve — "this
        pixel is this moment of the block" — stops being true. The area now takes
        its x and width from `getSampleScreenBounds`, so the mapping is exactly
        the block's own grid mapping. */
    juce::Rectangle<float> getSampleCurveArea (int layerIndex, int sampleId) const;
    juce::Rectangle<float> getSampleMenuArea (int layerIndex, int sampleId) const;
    juce::Rectangle<float> getSampleCloseArea (int layerIndex, int sampleId) const;

    /** Phase 50: the menu row has three parts now — the target button on the
        left, the enable switch on the right, the close cross in the corner. */
    juce::Rectangle<float> getSampleTargetArea (int layerIndex, int sampleId) const;
    juce::Rectangle<float> getSampleToggleArea (int layerIndex, int sampleId) const;

    SampleEvent*       getExpandedSample();
    const SampleEvent* getExpandedSample() const;

    int  findSampleCurvePointAt (int layerIndex, int sampleId,
                                 juce::Point<float> position) const;
    void dragSampleCurvePoint (int layerIndex, int sampleId, juce::Point<float> position);
    bool handleSampleAutomationMouseDown (const juce::MouseEvent& e,
                                          int layerIndex, int sampleId);
    void showSampleCurveMenu (int layerIndex, int sampleId, juce::Point<int> screenPosition);
    void drawSampleAutomationBox (juce::Graphics&);

    /** Phase 30: the maths behind the waveform, in one place.

        Pixels that carry audio = the source's remaining time divided by the
        time one pixel is worth AT THIS ZOOM. `blockWidth` only clamps it; it
        never scales it. That is the whole point: a shorter block hides the tail
        of the picture instead of squeezing the picture into itself. */
    int sampleWaveColumns (const Layer&, const SampleEvent&, float blockWidth) const;

    /** The clip the mouse is over, on the active layer. */
    SampleEvent*       hitTestSample (juce::Point<float> position);
    const SampleEvent* hitTestSample (juce::Point<float> position) const;

    /** Right-click on a clip: delete, duplicate, fit to source, trim. */
    void showSampleMenu (int sampleId, juce::Point<int> screenPos);

    //-- Phase 50: cut and reverse -------------------------------------------
    /** Splits every selected block that straddles the playhead into two.

        The right-hand half keeps the audio the left-hand half did not consume:
        the source offset advances by `cutSeconds * stretch.speed`, which is the
        same arithmetic the renderer reads with — so a cut block sounds exactly
        like the block it came from, with the playhead as the seam. Returns how
        many blocks were split (0 = the playhead was not inside any of them).

        Both halves get their curve split at the same normalised position, so a
        volume ramp stays a ramp instead of restarting on the second half. */
    int splitSelectedSamplesAtPlayhead();

    /** Flips `reversed` on every selected block. */
    int toggleSelectedSampleReverse();

    /** Phase 50: the block's window, in seconds of SOURCE — what a cut needs to
        hand the right-hand half. One function so the cut and the badge cannot
        disagree about where the block currently ends in the file. */
    double sampleWindowSeconds (const SampleEvent&, const Layer&) const;

    /** Phase 29: one trim step, shared by the drag, the test hook and both
        edges. `fromTail` keeps the head and moves the end; the other way keeps
        the end and moves the head, which is the only way "trim the front" does
        not also mean "make it end later". */
    bool applySampleTrim (const SampleEvent& origin, double wantedBeat, bool fromTail);

    /** Phase 28: arms a rigid move of the selected clips. Same clampMove() /
        applyMove() pair the group drag uses — a second "startBeat + delta"
        would be a second place for the clamp to disagree with the sound. */
    void armSampleMove (juce::Point<float> position);
    void captureSampleDragOriginals();

    /** Phase 29: everything that moves with a drag — notes AND clips. The note
        drag calls this one now, so a mixed selection keeps its shape. */
    void captureEditOriginals();

    /** Phase 28: the clip clipboard. Clips carry their buffer by shared_ptr, so
        a copy costs a pointer, not another decode. */
    std::vector<SampleEvent> sampleClip;
    double sampleClipFirstBeat = 0.0;
    double sampleClipLengthBeat = 1.0;

    /** Phase 37: where the NEXT paste in a streak of Ctrl+V must land, and the
        playhead that decision was taken against. A repeat has to start where
        the previous paste stopped — with a fixed anchor the second Ctrl+V
        lands exactly on the first, so "paste three times" produces one block
        at one beat. A moved playhead cancels the streak, because a moved
        playhead is the user naming a new place. -1 / -2 = no streak. */
    double samplePasteNextBeat     = -1.0;
    double samplePastePlayheadSeen = -2.0;

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

    /** Phase 29: the "退出高亮" chip, drawn after the veil so it is not dimmed
        by the very state it exists to end. */
    void drawSpotlightExitChip (juce::Graphics&);

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

    /** Phase 27: index of a layer reference already in hand. The group frame
        needs it because a clip's screen rectangle is addressed by layer INDEX
        (that is how the drawing code caches row geometry). */
    int layerIndexOf (const Layer* layer) const;

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
    NoteEvent*       getExpandedNote();
    const NoteEvent* getExpandedNote() const;

    void showModTargetMenu (const NoteEvent&, juce::Point<int> screenPosition);
    bool handleAutomationMouseDown (const juce::MouseEvent&, NoteEvent&);
    void dragAutomationPoint (NoteEvent&, juce::Point<float> position);
    int  findEnvelopePointAt (const NoteEvent&, juce::Point<float>) const;

    void drawCurveInBox (juce::Graphics&, const NoteEvent&, juce::Colour,
                         juce::Rectangle<float> inner);

    //-- Phase 21: the inline group automation box ----------------------------
    /** A group behaves like a note here: its name pill carries a small handle,
        and the handle opens a curve editor under the group, exactly as wide as
        the group itself, so the curve lines up with the notes it shapes.

        Addressed by (layer, groupId) for the same reason notes are addressed by
        id: groups live in a vector and any add/remove can move them. */
    juce::Rectangle<float> getGroupMenuArea() const;
    juce::Rectangle<float> getGroupCloseArea() const;

    /** The group the inline box is pointed at, or null. Never cached. */
    NoteGroup*       getExpandedGroup();
    const NoteGroup* getExpandedGroup() const;

    /** First and last beat covered by the group's members. Empty range when the
        group has no notes in this layer. */
    juce::Range<double> getGroupBeatSpan (const Layer&, const NoteGroup&) const;

    AutomationLane&       groupLaneFor (NoteGroup&, int target);
    const AutomationLane& groupLaneFor (const NoteGroup&, int target) const;

    void showGroupTargetMenu (juce::Point<int> screenPosition);
    bool handleGroupAutomationMouseDown (const juce::MouseEvent&);
    void dragGroupCurvePoint (juce::Point<float> position);
    int  findGroupCurvePointAt (juce::Point<float>) const;

    void drawGroupAutomationBox (juce::Graphics&);

    /** Phase 24: the piano roll's own grid, painted inside a curve area.

        Same step, same bar/beat/16th emphasis, same pixel rounding as
        `drawGridLines` — the box is a lens onto the same timeline, so its grid
        has to be the timeline's grid and not a decorative quarter-marker. */
    void drawGridInside (juce::Graphics&, juce::Rectangle<float> area) const;

    /** Phase 24: appends "音源参数" as a submenu of `slotId`'s plugin
        parameters. Returns false (and adds nothing) when that synth has no
        hosted plugin, so the caller can say so in words instead. */
    bool addPluginParamSubmenu (juce::PopupMenu& menu, int slotId, int currentIndex,
                                int firstItemId);

    //-- Phase 22: clipboard plumbing ----------------------------------------
    /** One curve in a box-agnostic form: beat offsets from the source's first
        beat, plus the source's length. A note envelope converts by
        `beat = time * lengthBeats`; a group lane by `beat = point.beat - start`.
        Both directions live here and nowhere else. */
    struct CurveClip
    {
        std::vector<AutomationPoint> points;
        double lengthBeats = 0.0;
        int    target      = 0;      // NoteModTarget or GroupAutomationTarget
        bool   fromGroup   = false;
    };

    /** The selected box's curve, in beat offsets. Empty when nothing is
        selected. */
    std::vector<AutomationPoint> readSelectedCurve (double& lengthBeats, int& target) const;    /** Writes a beat-offset curve into the selected box, stretched to fit or
        kept at its own length. */
    bool writeSelectedCurve (const std::vector<AutomationPoint>& points,
                             double sourceLength, int target, bool stretch);

    void dropCurveSelection();

    void timerCallback() override;

    /** Notes the playhead was inside before the last move, so the frames where
        a note starts or stops sounding can repaint that whole block instead of
        leaving a stale highlight behind. */
    void collectSoundingNotes (double beat, std::vector<juce::int64>& out) const;

    void clampScroll();

    //-- State ----------------------------------------------------------------
    LayerStack& stack;
    RackModel&  rack;

    std::vector<int>       selection;      // note ids, always of the active layer

    /** Phase 28: clip ids, also of the active layer.

        Phase 29: "never non-empty at the same time as `selection`" is no longer
        true — the marquee builds mixed selections, and Phase 35's "select the
        whole group" builds one on purpose. What is still true is that a CLICK
        produces one kind: setSelection and setSampleSelection each clear the
        other, so the only way into a mixed state is setMixedSelection. */
    std::vector<int>       sampleSelection;

    std::vector<NoteEvent> dragOriginals;  // snapshot taken on mouseDown

    /** Phase 27/28: the clips that are armed for THIS drag. Filled by a group
        move and by a clip move — both go through clampMove() + applyMove(), so
        there is one rigid-move implementation and the two cases cannot drift. */
    std::vector<SampleEvent> dragClipOriginals;

    DragMode dragMode        = DragMode::none;
    double   dragAnchorBeat  = 0.0;
    int      dragAnchorPitch = 0;

    /** Phase 25: the one note whose end band was grabbed. Both resize modes
        touch THIS note and no other — `dragOriginals` may hold a dozen notes
        (a selected group), and writing a shared `lengthBeats` to all of them
        lined every tail up with the pointer. */
    int      dragResizeNoteId = -1;
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
    /** Phase 31: the pixel column and the beat it held when the button went
        down. The zoom pins both, so the music under the pointer stays put —
        anchoring on the view centre (which is what a wheel does) made a
        right-drag at the left edge of the ruler throw the phrase there off
        screen. */
    float  rulerZoomAnchorX    = 0.0f;
    double rulerZoomAnchorBeat = 0.0;

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

    //-- Phase 21 -------------------------------------------------------------
    int    expandedGroupLayerIndex = -1;
    int    expandedGroupId         = -1;
    int    expandedGroupTarget     = 0;      // (int) GroupAutomationTarget
    int    dragGroupPoint          = -1;
    bool   hoveredGroupHandle      = false;
    int    hoveredHandleGroupId    = -1;
    int    hoveredHandleLayer      = -1;
    bool   hoveredGroupMenu        = false;

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

    //-- Phase 49: the clip's own automation box ------------------------------
    /** Mirrors the note box (Phase 8) and the group box (Phase 21): one
        floating editor at a time, opened by a chevron on the block. */
    int  expandedSampleId         = -1;
    int  expandedSampleLayerIndex = -1;
    int  dragSampleCurveIndex     = -1;
    int  hoveredSampleCurvePoint  = -1;
    bool hoveredSampleHandle      = false;
    int  hoveredSampleHandleId    = -1;
    bool hoveredSampleCurveMenu   = false;

    //-- Phase 22 -------------------------------------------------------------
    /** The key held down on the piano column, or -1 — both a paint state and
        the guard that stops a drag from re-auditioning the same pitch. */
    int pressedKeyPitch = -1;

    CurveSelection curveSelection = CurveSelection::none;
    CurveClip      curveClip;

    std::vector<NoteEvent> noteClip;
    double noteClipFirstBeat = 0.0;
    double noteClipEndBeat   = 0.0;

    /** Phase 24: a copied group, as an OBJECT.

        Copying a group and pasting its members back would make a copy that is
        not a copy: the notes land inside the SAME group (membership lives on
        the note), so the two halves become one larger group with one curve and
        one mount. What "copy 组 A" has to produce is a second group with 组 A's
        settings and its own notes.

        Deliberately NOT a `NoteGroup`: `id` is private and assigned by
        `Layer::addGroup`, and an id-carrying copy is exactly the kind of thing
        that gets pasted over a live group one day. This is the data, spelled
        out, with no identity in it. */
    struct GroupClip
    {
        bool         valid       = false;
        juce::String name;
        juce::Colour colour      { groupPalette::group1 };
        int          outputSlotId = -1;
        float        levelDb     = 0.0f;
        float        pan         = 0.0f;
        int          pluginParam = -1;
        double       startBeat   = 0.0;
        std::array<AutomationLane, numGroupAutomationTargets> lanes;
        std::array<bool, numGroupAutomationTargets>          laneOn { { false, false, false } };
    };

    GroupClip groupClip;

    /** Phase 24: how to ask what a hosted synth exposes. Null = none. */
    PluginParamLister listPluginParams;

    juce::String     statusText;
    juce::uint32     statusExpiryMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};

} // namespace atomic

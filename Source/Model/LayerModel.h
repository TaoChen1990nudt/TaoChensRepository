#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <vector>

#include "AutomationLane.h"
#include "Gain.h"
#include "Media/SampleLibrary.h"
#include "NoteEnvelope.h"

namespace atomic
{

//==============================================================================
/**
    A single note.

    Deliberately NOT a "MIDI message". In AtomicDAW a note owns its own state:
    later phases attach a per-note envelope curve here, and the note carries it
    wherever it is moved / copied. That is why this is a value type with a
    stable id instead of a transient event.
*/
struct NoteEvent
{
    int    id          = 0;
    int    pitch       = 60;       // MIDI note number, 0..127
    double startBeat   = 0.0;
    double lengthBeats = 1.0;
    float  velocity    = 0.8f;

    /** Phase 2: the note's own automation curve.

        Stored BY VALUE inside the note, on purpose. Move the note -> the curve
        moves with it. Copy the note (Layer::addNote takes a copy) -> the curve
        is duplicated with it. There is no map<noteId, curve> to keep in sync,
        which means there is no way for automation to get detached. */
    NoteEnvelope envelope;

    /** Phase 8: which parameter `envelope` drives.

        Stored by value next to the curve for the same reason the curve is: the
        target is part of the note's identity, not a property of whatever editor
        happened to be open when it was set. */
    NoteModTarget modTarget = NoteModTarget::level;

    /** Phase 24: when >= 0, `envelope` drives parameter `modPluginParam` of the
        HOSTED instrument this note resolves to, and `modTarget` is dormant.

        A separate field instead of extra enum values because a plugin's
        parameters are not a fixed set: they are a list the vendor defines, and
        an enum cannot name them. Keeping `modTarget` untouched means switching
        to a plugin parameter and back is lossless — the built-in choice you had
        before is still there.

        -1 means "drive the built-in target". A note mounted on an internal
        synth always has -1 in practice; routing can change afterwards, so this
        is a request, not a guarantee — the schedule publisher is what checks
        that the resolved synth actually has a plugin. */
    int modPluginParam = -1;

    /** Phase 3: the note's own output, bypassing its layer.

        -1 means "follow the layer's default output". Any other value is a
        SynthSlot id from the rack. Stored by value like the envelope, for the
        same reason: duplicating a note must duplicate its routing, otherwise
        the copy would quietly fall back to the layer default and play on the
        wrong synth.

        The override is only ever *consulted* through routing::resolve(), never
        read directly by the audio path. One resolver, one priority rule. */
    int outputSlotOverride = -1;

    /** Phase 16: the group this note belongs to, or -1 for none.

        Membership is stored HERE, on the note, and nowhere else. A group holds
        no list of members, so deleting a note cannot leave a dangling entry,
        and copying a note (duplicate, pencil, project save) copies its
        membership for free — exactly the rule the envelope and the output
        override already follow.

        The group id is only meaningful inside the layer that owns the note. */
    int groupId = -1;

    /** Phase 58: WHICH appearance of that group this note belongs to.

        A group is one definition — one name, one colour, one mount, one trim —
        and a song may want several copies of it: the same eight bars repeated
        at the chorus, each one dragged around on its own. That is what this
        number separates. Notes with the same (groupId, groupOccurrence) move
        together; two occurrences of the same group are independent objects that
        merely agree about everything in NoteGroup.

        Always 0 for a group that has never been reused, which is every project
        written before Phase 58 — so the field is absent from disk until it has
        something to say. */
    int groupOccurrence = 0;

    bool followsLayerOutput() const noexcept { return outputSlotOverride < 0; }
    bool belongsToGroup() const noexcept     { return groupId >= 0; }
    bool drivesPluginParam() const noexcept  { return modPluginParam >= 0; }
    void clearOutputOverride() noexcept      { outputSlotOverride = -1; }

    double endBeat() const noexcept { return startBeat + lengthBeats; }

    /** How far a musical position has travelled through this note: 0 at the
        head, 1 at the tail, negative when the position is outside it.

        This is the single definition of "this note is sounding right now". The
        playhead uses it to animate the note's envelope; the self-test checks
        the same function, so the picture and the assertion cannot diverge. */
    double progressAt (double beat) const noexcept
    {
        if (lengthBeats <= 0.0)
            return -1.0;

        const double u = (beat - startBeat) / lengthBeats;

        return (u < 0.0 || u > 1.0) ? -1.0 : juce::jlimit (0.0, 1.0, u);
    }
};

//==============================================================================
/** Phase 31: what dragging a clip's edge does to the AUDIO inside it.

    Phase 30 settled that a block is a window onto a file — trimming cuts the
    sound instead of squeezing it. That left "make the block twice as long"
    meaning "show some more silence", which is right for a window and wrong for
    a sampler. These are the four answers a sampler user actually wants, and the
    choice belongs to the CLIP, not the app: one project may want a tape-style
    squeeze next to a pitch-preserved stretch.

    Stored as an int in the project file, so a reader that does not know
    `pitch` yet still gets a valid value for every older project (0 = none). */
enum class StretchMode
{
    none     = 0,   // 原速窗口: nothing but where the audio stops (Phase 30)
    resample = 1,   // 变速变调: tape — twice as fast is also an octave up
    tempo    = 2,   // 只变速不变调: the whole file squeezed in, pitch kept
    pitch    = 3    // 不变速只变调: same speed, transposed by the length ratio
};

//==============================================================================
/** Phase 50: what a clip's automation box edits.

    A block has two curves, both normalised across the block, and exactly one of
    them is on screen at a time. Making the target a stored property of the
    block (rather than "whatever the panel last showed") means reopening the box
    tomorrow shows the same curve the user left, and a copied block carries its
    own answer — the same rule the note's `modTarget` follows.

    Kept in the model header because the renderer and the project file have to
    name the same two things the menu does. */
enum class SampleAutomationTarget
{
    volume = 0,   // 0..1 multiplies the clip's gain, 1.0 = untouched
    pan    = 1    // 0..1 offsets the clip's pan, 0.5 = centre
};

/** ASCII key used in the project file. */
inline const char* sampleAutomationTargetKey (SampleAutomationTarget target) noexcept
{
    switch (target)
    {
        case SampleAutomationTarget::pan: return "pan";
        case SampleAutomationTarget::volume:
        default:                          return "volume";
    }
}

inline SampleAutomationTarget sampleAutomationTargetFromKey (const juce::String& key) noexcept
{
    return key == "pan" ? SampleAutomationTarget::pan : SampleAutomationTarget::volume;
}

/** The neutral value each target idles at — the one number "reset to a flat
    line" and the disabled curve both have to agree on. */
inline float sampleAutomationNeutral (SampleAutomationTarget target) noexcept
{
    return target == SampleAutomationTarget::pan ? 0.5f : 1.0f;
}

//==============================================================================
/**
    Phase 17: an audio clip lying in the roll.

    A sample is NOT a note. It has no pitch that the synth plays, no envelope and
    no velocity: it is a piece of recorded time that occupies a rectangle on the
    grid. The two share the coordinate system and nothing else, which is why
    this is its own value type instead of a NoteEvent with a flag.

    The rectangle is described in musical units, so it follows the grid instead
    of fighting it:

      * `startBeat` / `lengthBeats` set the width. Because the width is measured
        in beats, every horizontal zoom picks the block up for free — the pixel
        width is `lengthBeats * beatWidth`, always.
      * `pitch` + `laneSpan` set the height, and `laneSpan` is a whole number of
        pitch lanes. A block that was 3 lanes tall stays exactly 3 lanes tall:
        it never straddles a lane boundary, so it can be lined up against notes.

    `audio` is an immutable shared buffer, not a file handle. The renderer reads
    it on the audio thread; re-decoding or reloading behind its back would be
    the same sin as letting it read a Layer. A project that names a file which
    has since moved loads with `audio == nullptr` and draws as "missing" — a
    visible problem instead of a silent one.
*/
struct SampleEvent
{
    int          id          = 0;
    juce::String name;                 // file name, drawn on the block
    juce::String path;                 // absolute path, so a reload can find it
    double       startBeat   = 0.0;
    double       lengthBeats = 4.0;
    int          pitch       = 60;     // the BOTTOM lane the block covers
    int          laneSpan    = 4;      // how many pitch lanes tall, >= 1
    float        gainDb      = 0.0f;

    //-- Phase 27: what plays it, and who it belongs to ----------------------
    /** Sampler slot this clip is mounted on, or -1 for "the layer's output".

        A clip names a SAMPLER, not an instrument: the sampler holds the file,
        so re-pointing this number changes the sound of the block while leaving
        its rectangle where it was. -1 means the clip plays its own `audio`
        through the layer's default bus, which is what every pre-Phase-27
        project does. */
    int          outputSlotId = -1;

    /** Group this clip belongs to, or -1. A clip can sit in a group with notes
        (Phase 27) so that moving the group moves it too — but membership says
        nothing about routing. See routing::resolveSample: a group's mount is a
        statement about notes, and a clip keeps its own sampler regardless. */
    int          groupId      = -1;

    /** Phase 58: which appearance of that group, exactly like
        NoteEvent::groupOccurrence — a block and the notes beside it belong to
        the same occurrence, and two occurrences move independently. */
    int          groupOccurrence = 0;

    //-- Phase 29: the window onto the file -----------------------------------
    /** Seconds into the source where this block starts.

        The block is a WINDOW onto the file, not a copy of it: trimming the head
        advances this by exactly the time the block lost, so the audio that is
        left stays where it was on the timeline. Without it a "trim" would be
        indistinguishable from "cut the tail and restart the file here", which
        is the one thing a trim must not sound like. */
    double sourceOffsetSeconds = 0.0;

    //-- Phase 49: the block's own volume curve -------------------------------
    /** Normalised 0..1 across the block, by value, exactly like a note's
        envelope: move the block and the curve moves with it, copy the block and
        the curve is duplicated, and no map<clipId, curve> can ever detach.

        The value multiplies the clip's gain, so 1.0 is "the file, untouched" —
        which is also what the disabled idle curve evaluates to. The old
        fade-in/fade-out ramps (Phase 29) were a two-point special case of this
        and are gone; a reader meets them in ProjectIO and migrates them onto
        this curve, so an old project sounds the same without keeping two ways
        to shape a block alive in the model. */
    NoteEnvelope volumeCurve = NoteEnvelope::makeIdle();

    //-- Phase 50: the second curve, and which one the box is editing ---------
    /** Normalised 0..1 across the block; 0.5 is centre, so the idle curve is a
        flat 0.5 line. Added as a separate member rather than a
        map<target, curve> for the same reason the volume curve is a member:
        copying a block must copy both shapes, and there is nothing to detach.

        Like the volume curve it is an OFFSET applied on top of the layer / synth
        / group pan, because that is what the layer's own pan lane does — one
        arithmetic for "pan is a sum of statements" instead of two. */
    NoteEnvelope panCurve = NoteEnvelope::makeIdle (0.5f);

    /** Which curve the floating box shows. Always written to the project file:
        P42's lesson was that "the last lane read wins" is not a selection rule,
        it is an accident of element order. */
    SampleAutomationTarget automationTarget = SampleAutomationTarget::volume;

    NoteEnvelope& curveFor (SampleAutomationTarget target) noexcept
    {
        return target == SampleAutomationTarget::pan ? panCurve : volumeCurve;
    }

    const NoteEnvelope& curveFor (SampleAutomationTarget target) const noexcept
    {
        return target == SampleAutomationTarget::pan ? panCurve : volumeCurve;
    }

    /** The curve the box edits right now. */
    NoteEnvelope&       activeCurve()       noexcept { return curveFor (automationTarget); }
    const NoteEnvelope& activeCurve() const noexcept { return curveFor (automationTarget); }

    //-- Phase 50: playing the file backwards --------------------------------
    /** True = play the window right to left.

        A flag, not a reversed copy of the buffer: the buffer belongs to the
        sampler slot and is shared by every block mounted on it, so reversing
        one block by rewriting audio would reverse the others and the file the
        user dropped. The renderer mirrors its read position instead, which
        also keeps a reversed block a pure function of the clock (Phase 31). */
    bool reversed = false;

    //-- Phase 31: what a length change does to the audio --------------------
    /** How this block answers "make me longer". `none` keeps Phase 30's window
        behaviour; the other three rescale the sound. A block's own property,
        because two blocks of the same file may want different answers. */
    StretchMode stretchMode = StretchMode::none;

    //-- Phase 52: how far 只变调 is allowed to transpose ----------------------
    static constexpr int defaultPitchRangeSemitones = 24;
    static constexpr int minPitchRangeSemitones     = 1;
    static constexpr int maxPitchRangeSemitones     = 36;

    /** Semitones, 1..36, applied only when `stretchMode == pitch`.

        Pitch-only mode derives its transposition from the length ratio, and
        that ratio is unbounded in practice: squashing a four-bar loop into one
        beat asks for +24 semitones and sounds like a chipmunk. This is the
        ceiling the user sets on the sampler card — a RANGE, not an offset,
        because the offset is already the block's length. Outside the range the
        pitch stops moving and the block simply plays the window at its own
        rate, which is the honest outcome of "I asked for at most an octave".

        Default 24 (two octaves): wide enough that nothing a user did before
        Phase 52 changes character, small enough to be a limit at all. Files
        older than v23 read it missing → 24, which is why the default is also
        the migration value. */
    int pitchRangeSemitones = defaultPitchRangeSemitones;

    std::shared_ptr<const SampleBuffer> audio;

    /** The roll is 128 lanes tall, so no block can be taller than that. */
    static constexpr int numPitchLanes = 128;

    double endBeat() const noexcept { return startBeat + lengthBeats; }

    /** Phase 29: how much of the file is left after the start offset. Zero when
        the buffer is missing — callers treat that as "no limit", because a block
        whose file could not be found must still be editable. */
    double remainingSourceSeconds() const noexcept
    {
        return audio != nullptr ? juce::jmax (0.0, audio->lengthSeconds() - sourceOffsetSeconds)
                                : 0.0;
    }

    /** Highest lane covered, inclusive. */
    int topPitch() const noexcept { return pitch + juce::jmax (1, laneSpan) - 1; }

    bool isLoaded() const noexcept { return audio != nullptr; }

    /** The length the source file would take at this tempo, in beats. Used when
        dropping a file and by the "fit to source" menu item. */
    static double beatsForSeconds (double seconds, double bpm) noexcept
    {
        return bpm > 0.0 ? seconds * bpm / 60.0 : seconds;
    }

    /** Phase 31: natural length / drawn length — the ONE number the three
        stretch modes are derived from. 1.0 means this block is exactly as long
        as the audio it holds, so:
          * a head trim cannot change it (offset and length move together),
          * a tail trim changes it, which is the whole point,
          * "恢复原始长度" puts it back to exactly 1.0.
        Measured against what is LEFT of the file after `sourceOffsetSeconds`,
        because that is what the block is a window onto. Zero when the buffer is
        missing; callers treat that as 1.0. */
    double stretchRatio (double bpm) const noexcept
    {
        const double naturalBeats = beatsForSeconds (remainingSourceSeconds(), bpm);

        return (naturalBeats > 0.0 && lengthBeats > 0.0) ? naturalBeats / lengthBeats : 1.0;
    }
};

//==============================================================================
/**
    Phase 16: a named set of notes treated as one object.

    Two things a group can do that a single note cannot:

      * **挂载音源** — one output for every member, sitting between the note's
        own override and the layer default. `outputSlotId` is -1 ("follow the
        layer"), otherwise a SynthSlot id, never an index.
      * **宏观自动化** — `AutomationLane`s in ABSOLUTE beats: one multiplies
        the level of every member, one adds to their pan, and (Phase 24) one
        writes a parameter of the hosted instrument the group is mounted on.
        Same time base as a layer curve, because a group curve is a property of
        the project, not of one note.

    Membership is NOT stored here. It lives on the note (`NoteEvent::groupId`),
    so there is exactly one place that can be wrong, and deleting a note cannot
    leave a stale member id behind. Everything "how many notes" / "which notes"
    is derived by scanning the layer.
*/
class NoteGroup
{
public:
    NoteGroup (int groupId, juce::String groupName, juce::Colour groupColour);

    int getId() const noexcept                    { return id; }
    const juce::String& getName() const noexcept  { return name; }
    juce::Colour getColour() const noexcept       { return colour; }

    /** Synth slot this group is mounted on, or -1 for "whatever the layer
        uses". A dangling id is tolerated: routing::resolve() falls back to the
        layer and reports it, rather than silently dropping the group. */
    int  getOutputSlotId() const noexcept         { return outputSlotId; }
    void setOutputSlotId (int slotId) noexcept    { outputSlotId = slotId; }
    bool followsLayerOutput() const noexcept      { return outputSlotId < 0; }

    /** Phase 24: which parameter of the mounted HOSTED instrument the `plugin`
        lane drives. -1 = none chosen yet, in which case that lane is inert.

        Index into `AudioProcessor::getParameters()` of whatever plugin the
        group's resolved synth holds. Stored as an int for the same reason a
        note's override is: a plugin may be uninstalled between sessions, and an
        index that no longer resolves has to be survivable, not fatal. */
    int  getPluginParam() const noexcept          { return pluginParam; }
    void setPluginParam (int index) noexcept      { pluginParam = index; }
    bool drivesPluginParam() const noexcept       { return pluginParam >= 0; }

    //-- Phase 17: the group's own trim --------------------------------------
    /** Volume and pan, the same pair every other mixer strip has.

        These are NOT a bus: a group has no signal path of its own, so there is
        nothing to route and nothing to meter. They are multiplied into the
        level of each member at the voice stage, next to the layer fader — which
        is why the group list is a list of trims, not a console. */
    float getLevelDb() const noexcept              { return levelDb; }
    void  setLevelDb (float db) noexcept           { levelDb = juce::jlimit (-48.0f, 6.0f, db); }

    float getPan() const noexcept                  { return pan; }
    void  setPan (float p) noexcept                { pan = juce::jlimit (-1.0f, 1.0f, p); }

    float getGainLinear() const noexcept           { return gain::fromDb (levelDb); }

    //-- Phase 35: the group's mute ------------------------------------------
    /** Silent, without touching the fader.

        The distinction is the whole reason this exists rather than "drag the
        level to -48": a mute has to be reversible with one click and has to
        leave the trim where the user put it. It lives on the group — not on
        the members — for the same reason the level does: muting a group is one
        decision, and copying it to sixty notes is sixty places to get wrong.

        Unlike the layer's mute there is no solo counterpart. A group's mute is
        a property of its notes AND its sample blocks (Phase 35 folded both into
        GroupMix::gainAt), and a solo over a subset of the members would only
        describe one of the two. */
    bool isMuted() const noexcept                  { return muted; }
    void setMuted (bool shouldBeMuted) noexcept    { muted = shouldBeMuted; }

    /** View state: the group list draws a one-line title bar when this is set.
        It lives on the model so every row-layout calculation stays a pure
        function of the model — the same rule Layer::automationExpanded follows. */
    bool isCollapsed() const noexcept              { return collapsed; }
    void setCollapsed (bool shouldCollapse) noexcept { collapsed = shouldCollapse; }

    AutomationLane&       getLevelAutomation() noexcept       { return automationLanes[0]; }
    const AutomationLane& getLevelAutomation() const noexcept { return automationLanes[0]; }

    /** Phase 20: the two macro lanes, indexed by GroupAutomationTarget, for the
        same reason Layer keeps both resident — the automation window lists
        "组 A · 音量" and "组 A · 声相" as independent tracks that are both live
        at once, and neither of them has to be "the selected one". */
    AutomationLane& automationFor (GroupAutomationTarget target) noexcept
    {
        return automationLanes[(size_t) target];
    }

    const AutomationLane& automationFor (GroupAutomationTarget target) const noexcept
    {
        return automationLanes[(size_t) target];
    }

    AutomationLane&       getPanAutomation() noexcept       { return automationFor (GroupAutomationTarget::pan); }
    const AutomationLane& getPanAutomation() const noexcept { return automationFor (GroupAutomationTarget::pan); }

    /** Phase 24: the lane that drives NoteGroup::getPluginParam(). Same value
        range and same absolute-beat time base as the other two. */
    AutomationLane&       getPluginAutomation() noexcept       { return automationFor (GroupAutomationTarget::plugin); }
    const AutomationLane& getPluginAutomation() const noexcept { return automationFor (GroupAutomationTarget::plugin); }

private:
    int          id = 0;
    juce::String name;
    juce::Colour colour;
    int          outputSlotId = -1;
    int          pluginParam  = -1;

    float levelDb   = 0.0f;       // a group is a trim: unity unless moved
    float pan       = 0.0f;
    bool  collapsed = false;
    bool  muted     = false;      // Phase 35: silences notes AND sample blocks

    /** 0..1 multiplier over project time, indexed by GroupAutomationTarget.
        Disabled means "identity": 1.0 for level, 0.0 for pan, and for the
        plugin lane the parameter simply is not written at all. */
    std::array<AutomationLane, numGroupAutomationTargets> automationLanes;

    JUCE_LEAK_DETECTOR (NoteGroup)
};

//==============================================================================
/**
    One overlay sheet of the piano roll. Owns its notes, its colour, its
    visibility and (Phase 16) its note groups. Layer -> rack routing lives on
    `outputSlotId`.
*/
class Layer
{
public:
    Layer (juce::String layerName, juce::Colour layerColour, int outputSlotId = 0);

    /** Stable identity, assigned on construction and never reused.

        Layers are addressed by INDEX by the piano roll (a layer is a sheet of
        paper in a stack, and the stack order is the index). Automation tracks,
        however, outlive index changes, so they bind to this id instead. Same
        rule the rack already follows for synth slots. */
    int  getId() const noexcept                     { return id; }
    void setId (int newId) noexcept                 { id = newId; }

    const juce::String& getName() const noexcept  { return name; }
    juce::Colour        getColour() const noexcept { return colour; }

    /** Only project load changes a layer's colour; the palette assigns the rest. */
    void setColour (juce::Colour newColour) noexcept { colour = newColour; }

    /** Phase 20: "show this sheet as a ghost while it is NOT the active one".

        It used to mean "this layer exists at all", which made a fresh project
        draw every sheet at 30% and left the eye button looking decorative.
        Now the ACTIVE layer is always drawn and always editable — you cannot
        draw on a sheet you cannot see — and this flag only decides whether the
        other sheets are visible behind it. False by default, so a new project
        shows exactly the one layer the user is working on. */
    bool isVisible() const noexcept                { return visible; }
    void setVisible (bool shouldBeVisible) noexcept { visible = shouldBeVisible; }

    /** Rack slot id (not an index!) that this layer's notes go to by default. */
    int  getOutputSlotId() const noexcept          { return outputSlotId; }
    void setOutputSlotId (int slotId) noexcept     { outputSlotId = slotId; }

    //-- Phase 5: the channel strip ------------------------------------------
    /** Fader, in dB. -48 is treated as -inf by the gain conversion, so a fader
        dragged to the bottom is genuinely silent rather than merely quiet. */
    float getLevelDb() const noexcept              { return levelDb; }
    void  setLevelDb (float db) noexcept           { levelDb = juce::jlimit (-48.0f, 6.0f, db); }

    /** -1 hard left, 0 centre, +1 hard right. */
    float getPan() const noexcept                  { return pan; }
    void  setPan (float p) noexcept                { pan = juce::jlimit (-1.0f, 1.0f, p); }

    bool isMuted() const noexcept                  { return muted; }
    void setMuted (bool m) noexcept                { muted = m; }

    bool isSoloed() const noexcept                 { return soloed; }
    void setSoloed (bool s) noexcept               { soloed = s; }

    /** Fader gain in linear terms, mute NOT included — mute/solo are resolved
        by the schedule publisher, which is the only place that can see every
        layer at once. */
    float getGainLinear() const noexcept
    {
        return gain::fromDb (levelDb);
    }

    /** Macro automation for this layer's volume or pan.

        Phase 7 keeps BOTH lanes resident, indexed by target, instead of one
        lane whose meaning depends on a selector. That is what lets the
        automation window list "Layer 3 · 音量" and "Layer 3 · 声相" as two
        independent tracks that are both live at the same time. */
    AutomationLane&       automationFor (LayerAutomationTarget t) noexcept
    {
        return automationLanes[(size_t) t];
    }

    const AutomationLane& automationFor (LayerAutomationTarget t) const noexcept
    {
        return automationLanes[(size_t) t];
    }

    /** The lane the left panel is currently pointed at. */
    AutomationLane&       getAutomation() noexcept       { return automationFor (automationTarget); }
    const AutomationLane& getAutomation() const noexcept { return automationFor (automationTarget); }

    LayerAutomationTarget getAutomationTarget() const noexcept { return automationTarget; }
    void setAutomationTarget (LayerAutomationTarget t) noexcept { automationTarget = t; }

    /** Whether the lane is unfolded in the panel. View state, but it lives with
        the layer so the row layout stays a pure function of the model. */
    bool isAutomationExpanded() const noexcept     { return automationExpanded; }
    void setAutomationExpanded (bool e) noexcept   { automationExpanded = e; }

    /** Phase 17: the whole row shrinks to a one-line title bar. Same rationale
        as automationExpanded — the row height is computed from the model, so
        the flag has to live here or the two would drift. */
    bool isCollapsed() const noexcept              { return collapsed; }
    void setCollapsed (bool shouldCollapse) noexcept { collapsed = shouldCollapse; }

    std::vector<NoteEvent>&       getNotes() noexcept       { return notes; }
    const std::vector<NoteEvent>& getNotes() const noexcept { return notes; }

    NoteEvent* findNote (int noteId) noexcept;
    const NoteEvent* findNote (int noteId) const noexcept;

    /** Adds the note, assigns it a fresh layer-unique id, returns that id. */
    int  addNote (NoteEvent noteToAdd);
    void removeNote (int noteId);

    //-- Phase 16: note groups ------------------------------------------------
    const std::vector<NoteGroup>& getGroups() const noexcept { return groups; }

    NoteGroup*       findGroup (int groupId) noexcept;
    const NoteGroup* findGroup (int groupId) const noexcept;

    /** Creates an empty group with a fresh layer-unique id, returns that id.
        The caller assigns notes with assignNotesToGroup(). */
    int addGroup (juce::String name, juce::Colour colour);

    /** Dissolves the group: every member is detached, the entity is dropped.
        The notes themselves stay exactly where they are. */
    void removeGroup (int groupId);

    /** Moves these notes into a group (they leave whatever group they were in).
        Passing groupId < 0 just detaches them. Unknown ids are ignored.

        `occurrence` says WHICH appearance of that group they join (Phase 58).
        Default 0 is the original one — which is what every pre-58 caller means,
        and exactly right for a group that was just created. */
    void assignNotesToGroup (const std::vector<int>& noteIds, int groupId, int occurrence = 0);

    /** Group of a note, or -1. Cheap single lookup, used by every readout. */
    int getGroupIdOfNote (int noteId) const noexcept;

    /** Which appearance of that group the note belongs to (0 when ungrouped). */
    int getGroupOccurrenceOfNote (int noteId) const noexcept;

    /** Members, in note order (the order the roll draws them in). */
    std::vector<int> getGroupNoteIds (int groupId) const;
    int countNotesInGroup (int groupId) const noexcept;

    //-- Phase 58: the appearances of one group -------------------------------
    /** Every occurrence number this group currently has, ascending.

        Derived, never stored: the same "membership lives on the member" rule
        the group itself obeys. A group with no members has no appearances, and
        an occurrence that loses its last member simply stops existing — there
        is no empty bracket to clean up. */
    std::vector<int> getGroupOccurrences (int groupId) const;

    /** The number a NEW appearance of this group should take: one past the
        highest in use, or 0 for a group that has never been placed. */
    int nextGroupOccurrence (int groupId) const;

    std::vector<int> getOccurrenceNoteIds (int groupId, int occurrence) const;
    int countNotesInOccurrence (int groupId, int occurrence) const noexcept;

    //-- Phase 27: clips as group members -------------------------------------
    /** A clip can be grouped with notes. It stays a clip: membership means
        "move and select with the rest", never "play what the group plays" —
        the routing question is answered by routing::resolveSample and nowhere
        else. */
    void assignSamplesToGroup (const std::vector<int>& sampleIds, int groupId, int occurrence = 0);
    int  getGroupIdOfSample (int sampleId) const noexcept;
    int  getGroupOccurrenceOfSample (int sampleId) const noexcept;
    std::vector<int> getGroupSampleIds (int groupId) const;
    int  countSamplesInGroup (int groupId) const noexcept;

    std::vector<int> getOccurrenceSampleIds (int groupId, int occurrence) const;
    int  countSamplesInOccurrence (int groupId, int occurrence) const noexcept;

    /** Replaces the group list (project load). Ids are preserved and the id
        counter is raised past them, exactly like restoreNotes(). */
    void restoreGroups (std::vector<NoteGroup> restoredGroups);

    /** Every note id in this layer, in model order. */
    std::vector<int> getAllNoteIds() const;

    //-- Phase 17: audio clips ------------------------------------------------
    /** Clips live on the layer for the same reason notes do: deleting a sheet
        must take its content with it, and a clip has to be drawn in the same
        coordinate space as the notes around it. */
    const std::vector<SampleEvent>& getSamples() const noexcept { return samples; }

    SampleEvent*       findSample (int sampleId) noexcept;
    const SampleEvent* findSample (int sampleId) const noexcept;

    /** Adds the clip, assigns it a fresh layer-unique id, returns that id. */
    int  addSample (SampleEvent sampleToAdd);
    void removeSample (int sampleId);

    /** Replaces the clip list (project load); ids and the id counter are
        recomputed exactly like restoreNotes(). */
    void restoreSamples (std::vector<SampleEvent> restoredSamples);

    int  getSampleCount() const noexcept { return (int) samples.size(); }

    /** Replaces the whole note list with restored notes (project load).

        The id counter is recomputed from the restored ids, so a note added
        after loading can never be handed an id that already exists. Without
        that, addNote() would start again at 1 and the piano roll's selection
        and the inspector would both bind to the wrong note. */
    void restoreNotes (std::vector<NoteEvent> restoredNotes);

    /** Highest beat occupied by any note (0 if empty). Used for scroll clamping. */
    double getEndBeat() const noexcept;

private:
    juce::String name;
    juce::Colour colour;
    bool visible      = false;   // Phase 20: ghosts are opt-in, per layer
    int  outputSlotId = 0;
    int  id           = 0;
    std::vector<NoteEvent> notes;
    int nextNoteId = 1;

    std::vector<SampleEvent> samples;
    int nextSampleId = 1;

    std::vector<NoteGroup> groups;

    float levelDb = -3.0f;
    float pan     = 0.0f;
    bool  muted   = false;
    bool  soloed  = false;

    std::array<AutomationLane, 2> automationLanes;
    LayerAutomationTarget automationTarget = LayerAutomationTarget::volume;
    bool                  automationExpanded = false;
    bool                  collapsed = false;

    JUCE_LEAK_DETECTOR (Layer)
};

//==============================================================================
/**
    Owns every layer plus which one is "active".

    The active layer is the only one that accepts mouse input. Everything else
    is painted as a ghost. This single rule is what makes overlapping sheets
    usable instead of a minefield of mis-clicks.
*/
class LayerStack
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** Layer added / removed / renamed / re-ordered / visibility / active changed. */
        virtual void layersChanged() {}

        /** Note geometry or content changed (moves, resizes, deletions). */
        virtual void notesChanged() {}
    };

    LayerStack() = default;

    int    getNumLayers() const noexcept;
    Layer* getLayer (int index) noexcept;
    const Layer* getLayer (int index) const noexcept;

    /** Lookup by stable id. Returns nullptr for an id whose layer is gone —
        callers (the automation window) must handle that rather than assume. */
    Layer*       findLayerById (int layerId) noexcept;
    int          indexOfLayerId (int layerId) const noexcept;

    int    getActiveLayerIndex() const noexcept { return activeIndex; }
    void   setActiveLayerIndex (int newIndex);
    Layer* getActiveLayer() noexcept;
    const Layer* getActiveLayer() const noexcept;

    /** Appends a layer, makes it active, returns its index. */
    int  addLayer (juce::String name, juce::Colour colour);
    void removeLayer (int index);
    void setLayerVisible (int index, bool shouldBeVisible);

    /** Replaces every layer in one step (project load).

        One notification instead of one per layer: a load must not publish a
        half-restored schedule for every layer it rebuilds. */
    void restoreLayers (std::vector<std::unique_ptr<Layer>> restoredLayers,
                        int activeLayerIndex);

    /** Changes the layer's default output. Goes through the stack so every
        view and the audio engine get told in one place. */
    void setLayerOutputSlot (int index, int slotId);

    //-- Phase 5 mix controls -------------------------------------------------
    void setLayerLevelDb (int index, float db);
    void setLayerPan (int index, float pan);
    void setLayerMuted (int index, bool shouldBeMuted);
    void setLayerSoloed (int index, bool shouldBeSoloed);
    void setLayerAutomationTarget (int index, LayerAutomationTarget target);
    void setLayerAutomationExpanded (int index, bool expanded);

    /** Announces an in-place edit (an automation handle drag) without the
        panel needing to know which setter to call. */
    void touchLayers();

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    /** Call after mutating notes so views refresh. */
    void notifyNotesChanged();

private:
    void notifyLayersChanged();

    std::vector<std::unique_ptr<Layer>> layers;
    juce::ListenerList<Listener> listeners;
    int activeIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerStack)
};

//==============================================================================
/** Shared palette so panel and roll never drift apart. */
namespace palette
{
    // P48: desaturated per DESIGN.md 3.5 (S x0.80, V x0.88 in HSB) — identity
    // colour must not compete with accent/warn/record for attention. layer1 is
    // left alone: it shares the accent hue and big solid uses, accent keeps the
    // thin-line uses, so the two stay distinguishable by SHAPE not value.
    const juce::Colour layer1 { 0xff4fd1c5 };   // teal (unchanged)
    const juce::Colour layer2 { 0xffd99a4d };   // amber (was f6ad55)
    const juce::Colour layer3 { 0xff8a6ccb };   // violet (was 9f7aea)
    const juce::Colour layer4 { 0xffcf5a91 };   // pink (was ed64a6)
    const juce::Colour layer5 { 0xff599ecf };   // blue (was 63b3ed)

    inline juce::Colour nextLayerColour (int index)
    {
        static const juce::Colour cycle[] { layer1, layer2, layer3, layer4, layer5 };
        return cycle[index % 5];
    }
}

/** Phase 16: group colours.

    Deliberately a DIFFERENT set from the layer palette. A group mark has to say
    "these notes are one object"; borrowing the layer's own colour would say
    "this note belongs to its layer", which is already obvious. */
namespace groupPalette
{
    // P48: desaturated per DESIGN.md 3.5 (S x0.80, V x0.88). NOTE: values are
    // serialised into .atomic files (colour attribute), so existing projects
    // keep their saved colours — only NEW layers/groups get these.
    const juce::Colour group1 { 0xffdfb95d };   // gold (was ffd166)
    const juce::Colour group2 { 0xff7bcb88 };   // mint (was 8ce99a)
    const juce::Colour group3 { 0xffdf8c6e };   // coral (was ff9f7a)
    const juce::Colour group4 { 0xff68a8d9 };   // sky (was 74c0fc)
    const juce::Colour group5 { 0xffb6a9df };   // lilac (was d0bfff)

    inline juce::Colour nextGroupColour (int index)
    {
        static const juce::Colour cycle[] { group1, group2, group3, group4, group5 };
        return cycle[juce::jmax (0, index) % 5];
    }

    /** "组 A", "组 B", ... "组 Z", "组 AA". Short enough to fit the label pill
        on a 0.5-beat note. */
    inline juce::String defaultName (int index)
    {
        const int i = juce::jmax (0, index);
        juce::String suffix;
        suffix << juce::juce_wchar ('A' + (i % 26));

        if (i >= 26)
            suffix << juce::String (i / 26);

        return juce::String::fromUTF8 (u8"组 ") + suffix;
    }
}

} // namespace atomic

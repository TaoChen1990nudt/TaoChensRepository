#pragma once

#include <juce_core/juce_core.h>

#include "Audio/PluginHost.h"
#include "AutomationModel.h"
#include "HardwareRack.h"
#include "LayerModel.h"
#include "LoopRegion.h"
#include "Meter.h"
#include "MixerModel.h"
#include "TempoModel.h"

namespace atomic
{

//==============================================================================
/**
    Reading and writing .atomic project files.

    The format is XML on purpose: a project is a tree of named things, and XML
    keeps the file inspectable with a text editor when something looks wrong.
    Every field the model owns is written — layer mix, rack mix, both kinds of
    automation curve, each note's envelope and its output override — because a
    "save" that quietly drops half the state is worse than no save at all.

    Two rules the loader follows, both learned the hard way in DAWs:

    1. **Parse into temporaries, commit at the end.** A malformed file must
       leave the open session untouched, not half-wiped. Nothing is applied to
       the LayerStack / RackModel until the entire document has been read and
       validated.

    2. **Repair instead of rejecting.** Points out of order, a curve with one
       point, a pitch of 300: all are clamped into shape. Only a structurally
       wrong document (no root element, no layers) is refused.
*/
namespace project
{

/** Bumped when the schema changes in a way an older reader cannot handle.

    v2 (Phase 7): layers carry a stable id, each owner stores BOTH automation
    lanes, and the file gained a mixer and an automation track list. A v1 file
    still loads — the missing pieces default to "one lane, no inserts".

    v3 (Phase 8): a note may name the parameter its curve drives
    (`mod="level|pan|pitch"`, absent = level).

    v4 (Phase 9): a rack slot or a mixer insert may name a hosted VST3. The
    plugin's description is embedded as a `<Plugin>` child, so a project opens
    with the same instruments it was saved with — provided they are installed.
    A missing plugin leaves the slot in place, named and silent, which is far
    better than refusing the file.

    v5 (Phase 11): a rack slot may be sent to a console channel
    (`mixer="0..7"`, absent = straight to master). Layers no longer imply a
    channel, so an older file's channel inserts load, but stay inaudible until
    an instrument is routed to that channel.

    v6 (Phase 16): a layer may own note groups. Each `<Group>` carries its own
    output mount and its own level curve, and each note names its group with
    `group="id"` (absent = ungrouped). An automation track may name a group as
    its owner (`owner="group"`). Older files read as "no groups anywhere".

    v7 (Phase 17): a layer may own audio clips (`<Sample>` with the file path,
    the grid rectangle and a gain), a group carries its own volume and pan, and
    the three collapsible lists remember which rows were folded
    (`collapsed="1"`). A missing sample file is NOT an error: the clip keeps its
    place and draws as "missing", because a project that refuses to open
    because one snare has moved is not a feature.

    v8 (Phase 18): the mixer remembers how many strips it had (`<Mixer
    channels="N">`, 1..8) and the master bus finally has curves of its own
    (`<MasterAutomation>`, two `<Automation>` children named level and pan).
    An automation track may name the master as its owner (`owner="master"`,
    always `ownerId="0"`). Older files read as "eight strips, master
    automation off".

    v9 (Phase 20): a note group can carry a pan curve as well as a level one,
    so `<Group>` may hold two `<Automation>` children whose `target` says which
    is which. A v8 group had exactly one, unnamed, and level is the only thing
    it could have been — so older files read with pan untouched.

    v10 (Phase 23): a console strip remembers which strip it feeds
    (`<Channel out="N">`, 8 = the master, also the default). Older files have
    no `out` and read back as a flat console, which is exactly what they were.

    v11 (Phase 24): a curve can drive a HOSTED instrument's parameter. A note
    names it with `pluginparam="N"` (absent = the built-in `mod` target still
    wins), and a group may carry a third `<Automation target="plugin">` lane
    plus `pluginparam="N"`. `N` is an index into the plugin's parameter list;
    a file naming a parameter of a plugin that is no longer installed loads
    fine and simply has nothing to drive. Older files read with no plugin
    targets at all.

    v12 (Phase 25): 127 addressable strips (the master sentinel moved from 8 to
    127, so `out="8"` in a v11-and-earlier file is translated to the master),
    and per-strip mono monitoring (`<Channel mono="1">`).

    v13 (Phase 26): the master strip's own mono button (`<Mixer masterMono="1">`).
    Absent means off, which is what an older project was doing anyway.

    v14 (Phase 27): sampler slots and clip mounts.

      * `<Synth kind="sampler" sample="C:\\...\\clip.wav">` — a rack slot that
        holds one file instead of an oscillator. The file is re-decoded on load;
        a missing one leaves the slot in place with no buffer, and the card says
        so rather than the project refusing to open.
      * `<Sample slot="7">` — the sampler this clip plays through. Absent = the
        layer's output, which is what every v13 clip did.
      * `<Sample group="3">` — clips can be group members now. Membership says
        "move with the group"; it never says what to play, so re-mounting a
        group leaves its clips pointing at the samplers they were on.

    v15 (Phase 29): a clip is a WINDOW onto its file.

      * `<Sample offset="1.25">` — seconds into the source where the block
        starts. Trimming the head writes it, so the rest of the audio stays on
        the timeline instead of restarting at the new head. Absent = 0, which is
        what every v14 clip did.
      * `<Sample fadein="0.5" fadeout="1">` — fade lengths in BEATS from each
        end. Absent = no fade, i.e. the hard edge a v14 clip had.

    v16 (Phase 31): a clip says what a length change does to its audio.

      * `<Sample stretch="0|1|2|3">` — 原速窗口 / 变速变调 / 只变速不变调 /
        不变速只变调. Absent = 0, which is the window behaviour every v15 clip
        already had, so an old project opens sounding exactly as it did. A
        reader that does not know a mode number must keep it as an int rather
        than guess a flag — the number IS the contract.

    v17 (Phase 35): a group can be muted, and it means its clips too.

      * `<Group mute="1">` — absent = not muted, which is what every v16 group
        meant. The field is on the GROUP, not on each member, so one attribute
        survives a paste, a duplicate and a merge; a v16 reader ignores it and
        opens the group at its fader value, which is the closest thing it can
        honestly do.

    v18 (Phase 42): a slot can automate a parameter of its own hosted instrument.

      * `<Slot pluginparam="N">` — which parameter of the plugin the third lane
        writes. Absent = none chosen, which is what every v17 slot meant.
      * `<Automation target="plugin">` — the third lane, same element as the
        other two, so a v17 reader that only knows "filter" and "level" simply
        skips it and opens the project with its existing two curves intact.

    v19 (Phase 43): the rest of the strip became automatable.

      * `<Automation target="pan|mute|solo">` — three more lanes on a slot,
        appended AFTER the existing three so the numbers in the file's `target`
        text and the model's enum keep agreeing. A v18 reader skips them and
        opens the project with its two/three curves intact and the strip at its
        fader value, which is what those files meant.

    v20 (Phase 47): inserts carry every knob.

      * `params="..."` — all fx::maxParams knob values, comma-separated, 0..1
        each. The EQ grew from one tilt knob to nine (3 bands x freq/gain/Q),
        the compressor to six, the reverb to five. p1/p2 stay written, and the
        reader still accepts them, so v19 files open with their two knobs
        intact and everything else at its default.

    v21 (Phase 49): a clip's fades became a volume curve.

      * `<Sample>` gains an `<Envelope>` child — the same element a `<Note>`
        has always carried, normalised over the block. The two `fadein` /
        `fadeout` attributes are no longer written; a reader that meets them
        (v14-v20) converts the ramps onto the curve, so an old project opens
        sounding exactly as it saved.

      Nothing else changed: no attribute was renamed, no meaning was redefined.

    v22 (Phase 50): a block's pan curve, which curve its box shows, and reversal.

      * `<Sample>` gains `<PanEnvelope>` — a second envelope under its own tag,
        because a reader that took "the first <Envelope> child" would be reading
        whichever one happened to be written first. That is the element-order
        mistake Phase 42 made with automation lanes, and this is how it is not
        made twice.
      * `autotarget="volume|pan"` is ALWAYS written: which curve the box was
        showing is state, not something to infer from which child exists.
      * `reversed="1"` when the block plays its window right to left.

    v23 (Phase 52): the range that 只变调 may transpose within.

      * `<Sample>` gains `pitchrange` (1..36 semitones), written only when it
        differs from the default 24 — a file that never touched the control
        keeps the same `<Sample>` line it had in v22.

    v24 (Phase 58): a group can be placed more than once.

      * `<Note>` and `<Sample>` gain `groupocc` — which appearance of that
        group this member belongs to. Written only when non-zero, so a project
        that never reused a group keeps every line it had in v23, and an older
        reader sees exactly what it always saw.

    v25 (Phase 62): the appearances of a group share one shape.

      * `<Group>` gains `<Occurrence index="n" start="beat"/>` children — where
        each appearance sits. Written only for appearances that have one, so a
        group placed once keeps the exact `<Group>` element it had in v24.

        Only the ORIGIN travels. The members are on the timeline with absolute
        beats, and a v24 file (no `<Occurrence>`) reads perfectly: the first
        time the origin is needed it is inferred from the earliest member and
        stored, so no project moves on load.

    v26 (Phase 82): curves become a bank — a note, a clip and a group may each
    hold several, each pointed somewhere or nowhere.

      * `<Curve id="n" name="…" target="…" pluginparam="n" enabled="0|1">` with
        `<Point>` children, plus one `<CurveState active="id"/>` beside them.
        The tag replaces three different shapes that each said their own fixed
        set: a note's `<Envelope>`, a clip's `<Envelope>`/`<PanEnvelope>` pair,
        and a group's `<Automation target="…">` lanes.

      * **No `target` means parked**: the curve keeps its shape and drives
        nothing. That is a state the old tags could not express at all, so it is
        written by leaving the attribute out rather than by inventing a word for
        it — an unknown word would read as "level" in a v25 reader, and "空" must
        never silently become "音量".

      * Every older tag still reads, through a migration branch per object:
        `<Envelope>` becomes the curve driving level (note) or volume (clip),
        `<PanEnvelope>` the one driving pan, `<Automation target="…">` the lane
        driving that target, and a note's `mod` / `pluginparam` attributes say
        where its single curve points. A v25 project therefore opens sounding
        exactly as it did; it is only re-WRITTEN in the new shape.

    v28 (Phase 87): the transport remembers a loop region.

      * `<Transport loop="1" loopStart="8" loopEnd="16"/>` — a child of the
        root, written only when the loop has been turned on at least once.
        Absent means "loop the whole project", which is exactly what every
        earlier version did, so an old file opens with the same behaviour it
        saved with.

        The two beats are written even when `loop="0"`: switching the loop off
        must not forget where it was, or the next click puts it back at the
        default four beats and the user has to find their eight bars again.

    v27 (Phase 86): a path may be relative to the project file.

      * `<Sample path="…">` and `<Synth sample="…">` are now written RELATIVE
        to the project's own folder whenever that is possible, so a project
        folder can be renamed, copied or zipped and still find its audio. It is
        only possible when the sample sits on a path that, resolved back against
        the project folder, lands on the same file; otherwise (another drive,
        the sample lives elsewhere entirely) the absolute path is written
        exactly as before.

      * **No new attribute.** The value itself says which it is: a path that is
        absolute is absolute, a path that is not is relative to the document.
        Both v27 and every earlier file therefore read with the same rule, and a
        v26 reader meeting a v27 file opens it with the samples it cannot
        resolve drawn as "missing" — degraded, but never mis-pointed, which is
        the reason this is a version bump at all.

    v29 (Phase 91): `<Transport beatsPerBar>` carries the metre.

      * The project's bar length used to be a `constexpr 4.0` compiled into the
        ruler, into "复制到后一小节" and into the metronome's accent. It is now
        a value that travels with the song.

      * **A v28 file has no such attribute and reads as 4/4** — the metre every
        earlier build drew — so there is no migration and nothing to guess. The
        element is written even when the loop never was, so "4/4, no loop" and
        "3/4, no loop" are different documents rather than the same one twice.

    v30 (Phase 95): a group can be one TAKE of a set.

      * `<Group takeset="3" take="2">` — which set, and which take of it. Both
        attributes are written only for a group that belongs to a set, so a
        project with no multi-pass recording keeps every line it had in v29 and
        an older reader sees a group it understands.
      * `takeactive="0"` — this is not the take you hear. Absent = it is, which
        is what every v29 group meant and what an ordinary group still means.
      * A set whose file says "two active" or "none" is repaired on load (the
        highest take wins); the document cannot be asked which pass the user
        intended, and guessing per-read would make two readers disagree.

    v31 (Phase 96): a take can be a COMP — spliced from the other takes.

      * `<Group takecomp="1">` — this take is the one the splices went into.
        Written only for a group that is both a take and a comp, so nothing
        changes for a project that never spliced, and a v30 reader simply sees
        another take.
      * Absent = an ordinary take. The flag cannot be inferred from the members
        (a comp's blocks look like any others), and it is not a playback fact:
        inferring it would mean guessing, and a wrong guess forks a second comp
        the next time the user splices.

    v32 (Phase 98): a layer can carry MIDI controller curves.

      * `<CC number="1" enabled="1">` with the same `<Point beat value>`
        children an `<Automation>` has. Three lanes are resident in the model
        (CC 1 / 11 / 64) but a lane that was never drawn is NOT written, so a
        song that predates this version re-saves to exactly the bytes it came
        from.

        Keyed by `number` rather than by a semantic name, which is the one
        place this file departs from its own convention — deliberately. A
        reader can check `number="11"` against the MIDI specification without
        this source tree, whereas `target="expression"` would only mean
        something to us.

    v33 (Phase 99): a strip can send a copy of itself to another strip.

      * `<Send to="3" db="-6" pre="1" on="1"/>`, up to two per `<Channel>`, and
        only the ones that are switched on are written. A song with no sends
        re-saves to the bytes it came from.

      * `to` is a channel index and is REQUIRED to be one; a send cannot reach
        the master (`-1` / missing means "not assigned"), because that is what
        the strip's own `out` already does and the two together would double the
        signal.

      * `pre` means the copy is taken BEFORE the strip's insert chain. Absent =
        after, which is the overwhelmingly common case and the one a v32 reader
        would draw if it ignored the attribute entirely.

    v34 (Phase 100): a strip's compressor can listen to another strip.

      * `<Channel key="2">` — this strip's detector is fed by strip 3's
        finished signal. Written only when a key is patched in, so a song with
        no sidechain re-saves to the bytes it came from.

      * The attribute lives on the LISTENING strip and names the DONOR, which
        is the direction the user thinks in ("duck the bass to the kick") and
        the direction the dependency runs at render time. Absent = the detector
        listens to the signal it is compressing, i.e. an ordinary compressor —
        which is also what a v33 reader draws, since it ignores the attribute
        and every strip starts un-keyed. */
inline constexpr int formatVersion = 34;

/** Including the dot, for FileChooser filters. */
inline constexpr const char* fileExtension = ".atomic";

/** Serialises the whole session to an XML document. Mixer and automation track
    list are optional so callers that only care about notes (and the older
    tests) do not have to build them. */
juce::String toXmlText (const LayerStack& stack, const RackModel& rack, double bpm,
                        const MixerModel* mixer = nullptr,
                        const AutomationModel* automation = nullptr,
                        const TempoModel* tempo = nullptr,
                        const juce::File* projectFile = nullptr,
                        const LoopRegion* loop = nullptr,
                        int beatsPerBar = meter::defaultBeatsPerBar);

/** Parses a document produced by toXmlText(). Returns false with `error` set
    when the document is not a usable project; the targets are then untouched.

    `projectFile` is what a relative sample path is relative TO. It must be the
    project the document belongs to, which is not always the file being read:
    a crash-recovery copy lives in the rescue folder but describes a project
    somewhere else, and resolving its relative paths against its own location
    would point every clip at the wrong disk. Pass null for a document with no
    home (a session that has never been saved) — absolute paths still work. */
bool fromXmlText (const juce::String& xmlText,
                  LayerStack& stack, RackModel& rack,
                  double& bpm, juce::String& error,
                  MixerModel* mixer = nullptr,
                  AutomationModel* automation = nullptr,
                  PluginHost* plugins = nullptr,
                  TempoModel* tempo = nullptr,
                  const juce::File* projectFile = nullptr,
                  LoopRegion* loop = nullptr,
                  int* beatsPerBar = nullptr);

bool save (const juce::File& file,
           const LayerStack& stack, const RackModel& rack, double bpm,
           juce::String& error,
           const MixerModel* mixer = nullptr,
           const AutomationModel* automation = nullptr,
           const TempoModel* tempo = nullptr,
           const LoopRegion* loop = nullptr,
           int beatsPerBar = meter::defaultBeatsPerBar);

bool load (const juce::File& file,
           LayerStack& stack, RackModel& rack,
           double& bpm, juce::String& error,
           MixerModel* mixer = nullptr,
           AutomationModel* automation = nullptr,
           PluginHost* plugins = nullptr,
           TempoModel* tempo = nullptr,
           LoopRegion* loop = nullptr,
           int* beatsPerBar = nullptr);

} // namespace project

} // namespace atomic

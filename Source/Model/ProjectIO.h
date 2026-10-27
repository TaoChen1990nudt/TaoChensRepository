#pragma once

#include <juce_core/juce_core.h>

#include "Audio/PluginHost.h"
#include "AutomationModel.h"
#include "HardwareRack.h"
#include "LayerModel.h"
#include "MixerModel.h"

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
        keeps the same `<Sample>` line it had in v22. */
inline constexpr int formatVersion = 23;

/** Including the dot, for FileChooser filters. */
inline constexpr const char* fileExtension = ".atomic";

/** Serialises the whole session to an XML document. Mixer and automation track
    list are optional so callers that only care about notes (and the older
    tests) do not have to build them. */
juce::String toXmlText (const LayerStack& stack, const RackModel& rack, double bpm,
                        const MixerModel* mixer = nullptr,
                        const AutomationModel* automation = nullptr);

/** Parses a document produced by toXmlText(). Returns false with `error` set
    when the document is not a usable project; the targets are then untouched. */
bool fromXmlText (const juce::String& xmlText,
                  LayerStack& stack, RackModel& rack,
                  double& bpm, juce::String& error,
                  MixerModel* mixer = nullptr,
                  AutomationModel* automation = nullptr,
                  PluginHost* plugins = nullptr);

bool save (const juce::File& file,
           const LayerStack& stack, const RackModel& rack, double bpm,
           juce::String& error,
           const MixerModel* mixer = nullptr,
           const AutomationModel* automation = nullptr);

bool load (const juce::File& file,
           LayerStack& stack, RackModel& rack,
           double& bpm, juce::String& error,
           MixerModel* mixer = nullptr,
           AutomationModel* automation = nullptr,
           PluginHost* plugins = nullptr);

} // namespace project

} // namespace atomic

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
    it could have been — so older files read with pan untouched. */
inline constexpr int formatVersion = 9;

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

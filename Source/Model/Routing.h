#pragma once

#include "HardwareRack.h"
#include "LayerModel.h"

namespace atomic
{

//==============================================================================
/**
    Routing: which synth a note is played by.

    This is the ONLY place the priority rule exists. The audio engine and every
    UI readout call these functions; none of them re-implements the rule. If the
    rule ever needs to change (say, a third tier for a group bus), it changes
    here and everything follows — the panel can never disagree with what you
    hear, because there is nothing to disagree with.

    Priority:
        1. note.outputSlotOverride   (越级 / note-level bypass)
        2. note's group.outputSlotId (编组挂载音源, Phase 16)
        3. layer.outputSlotId        (图层默认输出)
        4. rack's first slot         (only when 3 is unassigned or dangling)

    The group tier sits *below* the note and *above* the layer because that is
    the only order that makes both features mean what they say: a hand-placed
    exception on one note still wins, and mounting a whole group on a synth
    moves every member that has not been singled out.
*/
namespace routing
{

/** Where a layer's notes go when they do not override anything. Never -1
    unless the rack is completely empty. */
inline int layerSlotId (const Layer& layer, const RackModel& rack)
{
    if (const auto* slot = rack.findSlot (layer.getOutputSlotId());
        slot != nullptr && ! slot->isSampler())
        return layer.getOutputSlotId();

    // Phase 27: fall back to the first INSTRUMENT, not simply the first slot.
    // A rack can now legitimately start with a sampler, and a sampler has no
    // oscillator — landing there would mute the whole sheet for a reason the
    // user cannot see anywhere on screen.
    return rack.getFirstInstrumentSlotId();
}

/** Result of resolving one note. Carries *why* it ended up where it did, so the
    UI can say "越级" instead of just printing a name. */
struct Resolution
{
    int slotId = -1;

    /** True only when the note's override actually took effect. */
    bool overridden = false;

    /** True when the note reached slotId through its group's mount rather than
        through its layer. Mutually exclusive with `overridden` — the note's own
        choice, when it has one, always wins. */
    bool viaGroup = false;

    /** The note asked for a synth that no longer exists, so it fell back to the
        layer default. Worth surfacing: silently losing an override is exactly
        the kind of thing that wastes an afternoon. */
    bool danglingOverride = false;

    /** Same, one tier down: the group names a synth that is gone. */
    bool danglingGroup = false;

    bool isValid() const noexcept { return slotId >= 0; }
};

inline Resolution resolve (const Layer& layer, const NoteEvent& note, const RackModel& rack)
{
    Resolution result;
    result.slotId = layerSlotId (layer, rack);

    if (note.outputSlotOverride >= 0)
    {
        if (rack.findSlot (note.outputSlotOverride) != nullptr)
        {
            result.slotId     = note.outputSlotOverride;
            result.overridden = true;
        }
        else
        {
            result.danglingOverride = true;
        }

        // A note that carries its own (live) override is answered; a dangling
        // one still lets the group speak, because "the synth I named is gone"
        // should not also cost the note its group's mount.
        if (result.overridden)
            return result;
    }

    const auto* group = layer.findGroup (note.groupId);

    if (group == nullptr || group->followsLayerOutput())
        return result;

    if (rack.findSlot (group->getOutputSlotId()) != nullptr)
    {
        result.slotId   = group->getOutputSlotId();
        result.viaGroup = true;
    }
    else
    {
        result.danglingGroup = true;
    }

    return result;
}

//==============================================================================
/** Where a BARE key goes — the audition voice, the typing keyboard, and the
    piano roll's own key column all ask this one question.

        1. the focused (spotlit) slot, when it is an instrument
        2. the layer's default output (`layerSlotId`, which skips samplers)

    Phase 43: this rule used to exist twice. `MainComponent::getLiveTargetSlot()`
    implemented it for the computer keyboard; `PianoRollComponent` skipped the
    focus tier and called `layerSlotId` directly for its key column. The two only
    disagreed in one situation — a VST card focused — and that situation is
    exactly the one the user hit: the keyboard played the plugin, the piano
    roll's keys played the layer's internal synth, and both felt like bugs.

    A sampler is skipped for the same reason `layerSlotId` skips one: it has no
    oscillator, so "play this pitch" would be silent. */
inline int auditionSlotId (int focusedSlotId, const Layer& layer, const RackModel& rack)
{
    if (const auto* slot = rack.findSlot (focusedSlotId))
        if (! slot->isSampler())
            return focusedSlotId;

    return layerSlotId (layer, rack);
}

//==============================================================================
/** Where a clip plays and WHAT it plays, resolved in one place.

    A clip is not a note, so it does not walk the note tiers. Its own mount is
    the whole rule above the layer:

        1. sample.outputSlotId, when it names a live SAMPLER slot
        2. the layer's default output (same resolver the notes use)

    Deliberately absent: the group tier. A group's mount is a statement about
    the NOTES in it — "these notes are played by that instrument" — and a clip
    is not played by an instrument, it is played by a sampler. Making a clip
    follow its group would mean that re-mounting a group silently changes which
    audio file a clip plays, which is the one thing Phase 27 promised it would
    not do.

    `audio` is what the renderer should read. A clip mounted on a sampler takes
    that sampler's buffer; an unmounted one keeps its own, so projects written
    before samplers existed still sound like themselves. */
struct SampleResolution
{
    int    slotId = -1;                  // the bus this clip feeds
    std::shared_ptr<const SampleBuffer> audio;
    bool   viaSampler    = false;
    bool   danglingMount = false;        // named a sampler that is gone
    bool   missingAudio  = false;        // nothing to play at all
};

inline SampleResolution resolveSample (const Layer& layer, const SampleEvent& sample,
                                       const RackModel& rack)
{
    SampleResolution result;

    if (sample.outputSlotId >= 0)
    {
        if (const auto* slot = rack.findSlot (sample.outputSlotId); slot != nullptr)
        {
            if (slot->isSampler())
            {
                result.slotId = slot->id;
                result.viaSampler = true;
                result.audio = slot->sampleBuffer != nullptr ? slot->sampleBuffer
                                                             : sample.audio;
            }
            else
            {
                // Pointed at an instrument. Instruments do not hold clips, so
                // the mount carries no audio and the clip keeps its own — but
                // it still feeds that slot's bus, because "where it comes out"
                // and "what it plays" are separate questions.
                result.slotId = slot->id;
                result.audio  = sample.audio;
            }
        }
        else
        {
            result.danglingMount = true;    // falls through to the layer below
        }
    }

    if (result.slotId < 0)
        result.slotId = layerSlotId (layer, rack);

    if (result.audio == nullptr)
        result.audio = sample.audio;

    result.missingAudio = result.audio == nullptr;
    return result;
}

} // namespace routing
} // namespace atomic

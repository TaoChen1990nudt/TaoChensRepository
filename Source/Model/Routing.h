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
    if (rack.findSlot (layer.getOutputSlotId()) != nullptr)
        return layer.getOutputSlotId();

    return rack.getFirstSlotId();
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

} // namespace routing
} // namespace atomic

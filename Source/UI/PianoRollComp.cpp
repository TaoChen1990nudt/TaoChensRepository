/*  PianoRollComp.cpp  —  Phase 96

    Comping: "take 3 for bars 5-8, take 1 for the rest", with a crossfade at
    every seam, ending in one group.

    Phase 95 put several passes in one container and made exactly one of them
    sound. That answered "which performance" and left the other question alone:
    the best performance of eight bars is usually spread over four passes, and
    the only way to hear it was to pick one whole take. This is the splice.

    The interesting part is what a splice must NOT be. It must not be a script
    replayed from the first seam (that would need the segment list to survive
    a save, and the model has no set entity to hang it on — P95 made the
    container derived on purpose). It is instead one verb: REPLACE THIS RANGE
    WITH THAT TAKE'S MATERIAL. Apply it three times in any order and the result
    is the same comp. So nothing here remembers provenance; it only knows which
    group the splices go into, and that is a flag on the group.

    The second thing it must not be: a butt join. Cutting both takes at the same
    beat steps the signal between two performances in one sample, which is a
    click, and a click at a seam is the one defect a comp is judged on. The
    incoming take is therefore grown four milliseconds on each side and the
    neighbours fade over the same window, so both performances are present while
    their gains cross.
*/

#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Model/Comp.h"

namespace atomic
{
namespace
{
    /** Phase 96: the take that was playing, copied into a new take.

        The first splice has to leave the take it was told to copy from alone:
        "use take 3 for bars 5-8" is not "destroy take 3". So the comp starts as
        a copy of the one you were hearing, becomes the take that sounds, and
        every later splice edits it in place — three seams, one extra take.

        Everything is read into locals BEFORE the group is created. `groups` is a
        vector, `addGroup` pushes into it, and the `NoteGroup*` we started from
        would be pointing at freed storage by the time we wanted the name off
        it — the same trap Phase 95 hit with takes and `addGroup`'s neighbours. */
    int forkTakeAsComp (Layer& layer, int setId, int baseGroupId)
    {
        const auto* base = layer.findGroup (baseGroupId);

        if (base == nullptr)
            return -1;

        std::vector<NoteEvent>   notesToCopy;
        std::vector<SampleEvent> clipsToCopy;

        for (const auto& n : layer.getNotes())
            if (n.groupId == baseGroupId)
                notesToCopy.push_back (n);

        for (const auto& s : layer.getSamples())
            if (s.groupId == baseGroupId)
                clipsToCopy.push_back (s);

        const auto   name        = base->getName() + utf8 (" 拼接");
        const int    mount       = base->getOutputSlotId();
        const float  level       = base->getLevelDb();
        const float  pan         = base->getPan();
        const bool   muted       = base->isMuted();
        const int    groupIndex  = (int) layer.getGroups().size();

        const int groupId = layer.addGroup (name, groupPalette::nextGroupColour (groupIndex));

        if (layer.findGroup (groupId) == nullptr)
            return -1;

        if (auto* made = layer.findGroup (groupId))
        {
            made->setComp (true);
            made->setOutputSlotId (mount);
            made->setLevelDb (level);
            made->setPan (pan);
            made->setMuted (muted);
        }

        // One past the highest index in the set — the same arithmetic
        // `joinTakeSet` uses for a fresh pass, for the same reason: an index that
        // repeated would make the badge count takes that are not there.
        int nextIndex = 1;

        for (const int id : layer.getTakeSetGroupIds (setId))
            if (const auto* g = layer.findGroup (id))
                nextIndex = juce::jmax (nextIndex, g->getTakeIndex() + 1);

        std::vector<int> noteIds, clipIds;

        for (auto note : notesToCopy)
        {
            note.groupId         = -1;
            note.groupOccurrence = 0;
            noteIds.push_back (layer.addNote (note));
        }

        for (auto clip : clipsToCopy)
        {
            clip.groupId         = -1;
            clip.groupOccurrence = 0;

            const int id = layer.addSample (clip);

            if (id >= 0)
                clipIds.push_back (id);
        }

        layer.assignNotesToGroup (noteIds, groupId, 0);
        layer.assignSamplesToGroup (clipIds, groupId, 0);

        if (auto* made = layer.findGroup (groupId))
            made->setTakeMembership (setId, nextIndex);

        // Splice a copy that does not sound and nothing appears to happen; the
        // comp is the thing you are editing from here on.
        layer.setActiveTake (groupId);

        return groupId;
    }

    /** Phase 96: takes the range OUT of one block of the comp.

        Blocks are trimmed rather than deleted because the material on either
        side of a seam is still the comp's: the tail has to end exactly where the
        incoming take begins, and the head has to resume exactly where it ends,
        or the splice either overlaps or leaves a hole. Cutting the block is
        what produces those two edges — and cutting is already the knife's job,
        so this asks the knife (`splitSampleBlocksRaw`) instead of doing the
        source-window arithmetic a second time. */
    void cutRangeOutOfBlock (PianoRollComponent& roll, Layer& layer, int id, const comping::Range& range)
    {
        constexpr double eps = 1.0e-9;

        // At most two cuts, plus one pass to notice there is nothing left to do.
        for (int guard = 0; guard < 4; ++guard)
        {
            const auto* s = layer.findSample (id);

            if (s == nullptr)
                return;

            const double b0 = s->startBeat;
            const double b1 = s->endBeat();

            const bool insideHead = b0 >= range.start - eps;
            const bool insideTail = b1 <= range.end + eps;

            if (insideHead && insideTail)          // wholly inside: it goes
            {
                layer.removeSample (id);
                return;
            }

            if (! insideHead && insideTail)        // wholly before: it stays, and
                return;                            // the fade pass will see it

            std::vector<int> made;

            if (insideHead)                        // starts inside, runs past the end
            {
                if (roll.splitSampleBlocksRaw ({ id }, range.end, made) == 0)
                    return;                        // cannot cut: leave it whole

                layer.removeSample (made.front()); // the piece that was inside
                return;
            }

            // Straddles the start: cut at the seam and carry on with the right
            // half, which now begins exactly where the range does.
            if (roll.splitSampleBlocksRaw ({ id }, range.start, made) == 0)
                return;

            id = made.back();
        }
    }

    /** Phase 96: clips a copied block to the range plus one crossfade each side.

        The crossfade is the reason for the `fade` margin. Four milliseconds of
        the incoming performance must sound WHILE the outgoing one is still
        sounding, or the two ramps meet at zero and the seam is a hole rather
        than a crossfade.

        Answers the id that survived (a trim can leave the piece under a new id),
        or -1 when nothing did. */
    int trimBlockToRange (PianoRollComponent& roll, Layer& layer, int id,
                          const comping::Range& range, double fade)
    {
        constexpr double eps = 1.0e-9;

        const double lo = range.start - fade;
        const double hi = range.end + fade;

        for (int guard = 0; guard < 3; ++guard)
        {
            const auto* s = layer.findSample (id);

            if (s == nullptr)
                return -1;

            if (s->startBeat >= lo - eps)
                break;

            std::vector<int> made;

            if (roll.splitSampleBlocksRaw ({ id }, lo, made) == 0)
                break;

            layer.removeSample (made.front());     // the head before the fade
            id = made.back();
        }

        if (const auto* s = layer.findSample (id))
        {
            if (s->endBeat() > hi + eps)
            {
                std::vector<int> made;

                if (roll.splitSampleBlocksRaw ({ id }, hi, made) > 0)
                    layer.removeSample (made.back());     // the tail after the fade
            }
        }

        return layer.findSample (id) != nullptr ? id : -1;
    }

    /** Phase 96: does this block's edge sit ON the seam, or on the fade that the
        seam pushed it to? The two answers are different verbs — a tail that ENDS
        at the seam fades out, a head that STARTS at the seam fades in — and a
        block that merely happens to start nearby is not a seam at all, which is
        why the head test looks inside the fade window and the tail test at the
        seam itself. */
    void applySeamFades (Layer& layer, int groupId, const comping::Range& range, double fade)
    {
        constexpr double eps = 1.0e-6;

        std::vector<int> ids;

        for (const auto& s : layer.getSamples())
            if (s.groupId == groupId)
                ids.push_back (s.id);

        for (const int id : ids)
        {
            auto* s = layer.findSample (id);

            if (s == nullptr)
                continue;

            const float frac = comping::fadeFraction (fade, juce::jmax (1.0e-9, s->lengthBeats));

            const bool headAtStart = s->startBeat >= range.start - fade - eps
                                       && s->startBeat <= range.start + eps;
            const bool headAtEnd   = std::abs (s->startBeat - range.end) <= eps;
            const bool tailAtStart = std::abs (s->endBeat() - range.start) <= eps;
            const bool tailAtEnd   = s->endBeat() >= range.end - eps
                                       && s->endBeat() <= range.end + fade + eps;

            auto& volume = s->ensureCurveDriving (SampleAutomationTarget::volume);

            if (headAtStart || headAtEnd)
                comping::addEdgeFade (volume, true, frac);

            if (tailAtStart || tailAtEnd)
                comping::addEdgeFade (volume, false, frac);
        }
    }
} // namespace


//==============================================================================
int PianoRollComponent::compTakeRange (int takeGroupId, double from, double to)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    const comping::Range range { from, to };

    if (! comping::isUsable (range))
        return -1;

    const auto* source = layer->findGroup (takeGroupId);

    if (source == nullptr || ! source->isTake())
        return -1;

    const int  setId   = source->getTakeSetId();
    const auto members = layer->getTakeSetGroupIds (setId);

    // One take is not a set. Forking a comp out of the only pass would leave two
    // identical copies and a seam with no second performance to fade into.
    if (members.size() < 2)
        return -1;

    //-- 1. where the splice goes -------------------------------------------
    int compId = -1;

    for (const int id : members)
        if (const auto* g = layer->findGroup (id); g != nullptr && g->isComp())
            compId = id;

    if (compId < 0)
    {
        int baseId = members.front();

        for (const int id : members)
            if (const auto* g = layer->findGroup (id); g != nullptr && g->isActiveTake())
                baseId = id;

        compId = forkTakeAsComp (*layer, setId, baseId);

        if (compId < 0)
            return -1;
    }
    else
    {
        layer->setActiveTake (compId);
    }

    // Four milliseconds in beats, at the tempo the roll already cuts blocks
    // with: the seam must be the same length as the knife's idea of a beat.
    const double fade = comping::fadeBeats (60.0 / juce::jmax (1.0, bpm));

    //-- 2. the comp loses the range ----------------------------------------
    // Notes that START inside go; a note that began before the seam and still
    // sounds is left alone, because cutting it in half would turn a legato line
    // into two attacks. Blocks are trimmed, never taken whole.
    {
        std::vector<int> doomed;

        for (const auto& n : layer->getNotes())
            if (n.groupId == compId && comping::noteJoins (n.startBeat, range))
                doomed.push_back (n.id);

        for (const int id : doomed)
            layer->removeNote (id);
    }

    {
        std::vector<int> touched;

        for (const auto& s : layer->getSamples())
            if (s.groupId == compId && comping::blockJoins (s.startBeat, s.lengthBeats, range))
                touched.push_back (s.id);

        for (const int id : touched)
            cutRangeOutOfBlock (*this, *layer, id, range);
    }

    //-- 3. the source's material arrives ------------------------------------
    {
        std::vector<NoteEvent> arriving;

        for (const auto& n : layer->getNotes())
            if (n.groupId == takeGroupId && comping::noteJoins (n.startBeat, range))
                arriving.push_back (n);

        std::vector<int> fresh;

        for (auto note : arriving)
        {
            note.groupId         = -1;
            note.groupOccurrence = 0;
            fresh.push_back (layer->addNote (note));
        }

        if (! fresh.empty())
            layer->assignNotesToGroup (fresh, compId, 0);
    }

    {
        std::vector<SampleEvent> arriving;

        for (const auto& s : layer->getSamples())
            if (s.groupId == takeGroupId && comping::blockJoins (s.startBeat, s.lengthBeats, range))
                arriving.push_back (s);

        std::vector<int> fresh;

        for (auto clip : arriving)
        {
            clip.groupId         = -1;
            clip.groupOccurrence = 0;

            const int id = layer->addSample (clip);

            if (id < 0)
                continue;

            const int survivor = trimBlockToRange (*this, *layer, id, range, fade);

            if (survivor >= 0)
                fresh.push_back (survivor);
        }

        if (! fresh.empty())
            layer->assignSamplesToGroup (fresh, compId, 0);
    }

    //-- 4. both seams get their fades ---------------------------------------
    applySeamFades (*layer, compId, range, fade);

    //-- 5. the group's origin and the announcement --------------------------
    // The origin is the earliest member, exactly as after any other shape edit:
    // a new splice can extend the comp backwards, and an origin that stayed put
    // would make the next drag of this group move by the wrong amount.
    layer->pruneOccurrenceStarts (compId);

    bool   any    = false;
    double origin = 0.0;

    for (const auto& n : layer->getNotes())
        if (n.groupId == compId)
        {
            origin = any ? juce::jmin (origin, n.startBeat) : n.startBeat;
            any    = true;
        }

    for (const auto& s : layer->getSamples())
        if (s.groupId == compId)
        {
            origin = any ? juce::jmin (origin, s.startBeat) : s.startBeat;
            any    = true;
        }

    if (any)
        layer->setOccurrenceStart (compId, 0, origin);

    stack.notifyNotesChanged();
    repaint();

    return compId;
}

} // namespace atomic

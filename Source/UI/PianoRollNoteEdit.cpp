#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Util/Metrics.h"
#include "Util/SharedRules.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"
#include "Util/Theme.h"

#include <limits>
#include <map>
#include <set>

namespace atomic
{
using namespace roll;


//==============================================================================
int PianoRollComponent::addNoteAt (double beat, int pitch)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return -1;

    // The template is a whole NoteEvent, so the copy carries everything: the
    // length, the velocity, the curve inside it, the parameter that curve drives
    // and the synth override. Pitch and position are the only things the click
    // decides, because those are what the user is pointing at.
    NoteEvent note = hasPaintState ? paintState : NoteEvent {};
    note.pitch       = juce::jlimit (0, numPitches - 1, pitch);
    note.lengthBeats = juce::jmax (gridDivision, note.lengthBeats);
    note.velocity    = juce::jlimit (0.0f, 1.0f, note.velocity);

    const double raw = snapEnabled ? snapBeat (beat) : beat;
    note.startBeat = juce::jmax (0.0, raw);

    // Landing past the end is how the timeline grows while drawing.
    ensureTimelineExtendsTo (note.startBeat + note.lengthBeats + 4.0);
    note.startBeat = juce::jmin (note.startBeat,
                                 juce::jmax (0.0, timelineBeats - gridDivision));

    return active->addNote (note);
}


//==============================================================================
bool PianoRollComponent::copyNoteStateFromSelection()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // First selected note in model order, not in click order: "the state I
    // copied" must not depend on which note happened to be clicked last.
    for (const auto& note : active->getNotes())
    {
        if (! isSelected (note.id))
            continue;

        paintState = note;
        hasPaintState = true;

        // Select/tool switch is the caller's business, but the strip has to
        // repaint either way: the button lights up and the hint text changes.
        repaint();
        return true;
    }

    return false;
}


void PianoRollComponent::clearPencilState()
{
    if (! hasPaintState)
        return;

    hasPaintState = false;
    paintState = NoteEvent {};
    repaint();
}


std::vector<int> PianoRollComponent::duplicateSelectedNotes (double offsetBeats)
{
    std::vector<int> newIds;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return newIds;

    // Snapshot before mutating: addNote() push_backs into the same vector we
    // would otherwise be iterating.
    std::vector<NoteEvent> originals;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            originals.push_back (note);

    for (auto copy : originals)
    {
        copy.startBeat = juce::jlimit (0.0, juce::jmax (0.0, timelineBeats - copy.lengthBeats),
                                       copy.startBeat + offsetBeats);

        // Whole-struct copy: geometry, velocity AND the envelope. This one
        // line is why automation cannot get detached from its note.
        newIds.push_back (active->addNote (copy));
    }

    if (! newIds.empty())
    {
        setSelection (newIds);
        stack.notifyNotesChanged();
    }

    return newIds;
}


PianoRollComponent::MoveClamp PianoRollComponent::clampMove (const std::vector<NoteEvent>& originals,
                                                            double rawBeats, int rawPitch,
                                                            bool snap,
                                                            const std::vector<SampleEvent>& clips) const
{
    MoveClamp move;
    move.beats = rawBeats;
    move.pitch = rawPitch;

    if (originals.empty() && clips.empty())
        return move;

    // Phase 27: the block is notes AND clips. Both are measured into the same
    // min/max, because a group that clamps against its notes alone would let a
    // clip slide off the top of the grid while its bracket stopped moving.
    bool   haveAny  = false;
    double minStart = 0.0;
    int    minPitch = 0;
    int    maxPitch = 0;

    const auto measure = [&] (double start, int lowPitch, int highPitch)
    {
        if (! haveAny)
        {
            haveAny  = true;
            minStart = start;
            minPitch = lowPitch;
            maxPitch = highPitch;
            return;
        }

        minStart = juce::jmin (minStart, start);
        minPitch = juce::jmin (minPitch, lowPitch);
        maxPitch = juce::jmax (maxPitch, highPitch);
    };

    for (const auto& origin : originals)
        measure (origin.startBeat, origin.pitch, origin.pitch);

    for (const auto& clip : clips)
        measure (clip.startBeat, clip.pitch, clip.topPitch());

    // Snapping applies to the BLOCK, anchored on its leftmost member. Snapping
    // each one separately would round a deliberately off-grid member into place
    // and quietly rewrite the spacing of the thing being dragged.
    if (snap)
        move.beats = snapBeat (minStart + move.beats) - minStart;

    // The leftmost member is the one that decides: it stops at beat 0 and takes
    // everyone with it. Same on the way right, and same vertically — the block
    // keeps its shape and stops.
    move.beats = juce::jmax (move.beats, -minStart);

    const double latestStart = juce::jmax (0.0, timelineBeats - gridDivision);

    if (latestStart > minStart)
        move.beats = juce::jmin (move.beats, latestStart - minStart);

    move.pitch = juce::jlimit (-minPitch, numPitches - 1 - maxPitch, move.pitch);

    return move;
}


void PianoRollComponent::captureDragOriginals()
{
    dragOriginals.clear();
    dragClipOriginals.clear();
    dragAppliedMoveBeats = 0.0;      // Phase 62: a new gesture measures from zero

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            dragOriginals.push_back (note);
}


void PianoRollComponent::captureGroupClips (int groupId, int occurrence)
{
    // Phase 65: kept as the "one appearance" spelling of the capture, but the
    // gesture now arms EVERY selected block. A click on a group label is a
    // click on one group, yet what moves is the selection — narrowing it to the
    // group under the pointer is exactly what made "select two groups, drag one"
    // drop the other.
    juce::ignoreUnused (groupId, occurrence);

    captureSampleDragOriginals();
}


void PianoRollComponent::applyMove (const MoveClamp& move)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& origin : dragOriginals)
        if (auto* note = active->findNote (origin.id))
        {
            note->startBeat = origin.startBeat + move.beats;
            note->pitch     = origin.pitch + move.pitch;
        }

    for (const auto& origin : dragClipOriginals)
        if (auto* clip = active->findSample (origin.id))
        {
            clip->startBeat = juce::jmax (0.0, origin.startBeat + move.beats);

            // A block keeps its whole lane span, so its BOTTOM lane is what is
            // clamped — the same rule the single-clip drag uses. Clamping the
            // top instead would let a tall block hang off the bottom row.
            clip->pitch = juce::jlimit (0, numPitches - juce::jmax (1, clip->laneSpan),
                                        origin.pitch + move.pitch);

            ensureTimelineExtendsTo (clip->endBeat());
        }

    applyOccurrenceOriginsForMove (move.beats, active);

    // ...and the shape half, live: dragging one member of an appearance is an
    // edit of the group, so the other appearances move with the pointer instead
    // of snapping into place on mouse-up.
    syncDragShapes();
}


bool PianoRollComponent::syncDragShapes()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || (dragOriginals.empty() && dragClipOriginals.empty()))
        return false;

    std::vector<int> noteIds, clipIds;

    for (const auto& note : dragOriginals)
        noteIds.push_back (note.id);

    for (const auto& clip : dragClipOriginals)
        clipIds.push_back (clip.id);

    const auto whole    = wholeGroupOccurrencesInDrag();
    const auto touched  = groupOccurrencesOf (noteIds, clipIds);

    bool changed = false;

    for (const auto& entry : touched)
    {
        // A carry of the whole appearance is a change of position, not of shape.
        if (std::find (whole.begin(), whole.end(), entry) != whole.end())
            continue;

        changed = layer->syncGroupShape (entry.first, entry.second) || changed;
    }

    if (changed)
        stack.notifyNotesChanged();

    return changed;
}


void PianoRollComponent::applyOccurrenceOriginsForMove (double movedBeats, Layer* layer)
{
    // `move.beats` is measured from the ORIGINALS, not from the last mouse-move,
    // so the origin has to be moved by the difference — adding the whole delta
    // on every mouse-move would walk the appearance off the timeline.
    const double step = movedBeats - dragAppliedMoveBeats;

    dragAppliedMoveBeats = movedBeats;

    if (layer == nullptr || std::abs (step) < 1.0e-12)
        return;

    //-- Only an appearance carried WHOLE keeps its shape ---------------------
    // Dragging all of it is a change of POSITION: the origin travels with it and
    // the shape (member offsets from that origin) is untouched. Dragging part of
    // it is a change of SHAPE, which is Phase 62's other half — the siblings get
    // rebuilt from it at the end of the gesture, and this origin must stay put
    // so that "the shape moved inside its own box" is what gets copied.
    for (const auto& entry : wholeGroupOccurrencesInDrag())
    {
        const double origin = layer->occurrenceStart (entry.first, entry.second);
        layer->setOccurrenceStart (entry.first, entry.second, origin + step);
    }
}


std::vector<std::pair<int, int>> PianoRollComponent::wholeGroupOccurrencesInDrag() const
{
    std::vector<std::pair<int, int>> whole;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return whole;

    const auto addIfWhole = [&] (int groupId, int occurrence)
    {
        if (groupId < 0)
            return;

        int carried = 0;

        for (const auto& note : dragOriginals)
            if (note.groupId == groupId && note.groupOccurrence == occurrence)
                ++carried;

        for (const auto& clip : dragClipOriginals)
            if (clip.groupId == groupId && clip.groupOccurrence == occurrence)
                ++carried;

        const int members = layer->countNotesInOccurrence (groupId, occurrence)
                              + layer->countSamplesInOccurrence (groupId, occurrence);

        if (members > 0 && carried == members
              && std::find (whole.begin(), whole.end(),
                            std::make_pair (groupId, occurrence)) == whole.end())
            whole.push_back ({ groupId, occurrence });
    };

    for (const auto& note : dragOriginals)
        addIfWhole (note.groupId, note.groupOccurrence);

    for (const auto& clip : dragClipOriginals)
        addIfWhole (clip.groupId, clip.groupOccurrence);

    return whole;
}


std::vector<std::pair<int, int>> PianoRollComponent::groupOccurrencesOf (const std::vector<int>& noteIds,
                                                                        const std::vector<int>& clipIds) const
{
    std::vector<std::pair<int, int>> occurrences;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return occurrences;

    const auto add = [&occurrences] (int groupId, int occurrence)
    {
        if (groupId < 0)
            return;

        const auto entry = std::make_pair (groupId, occurrence);

        if (std::find (occurrences.begin(), occurrences.end(), entry) == occurrences.end())
            occurrences.push_back (entry);
    };

    for (int id : noteIds)
        if (const auto* note = layer->findNote (id))
            add (note->groupId, note->groupOccurrence);

    for (int id : clipIds)
        if (const auto* clip = layer->findSample (id))
            add (clip->groupId, clip->groupOccurrence);

    return occurrences;
}


bool PianoRollComponent::syncGroupShapesAfterEdit (const std::vector<std::pair<int, int>>& touched)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || touched.empty())
        return false;

    bool changed = false;

    for (const auto& entry : touched)
        changed = layer->syncGroupShape (entry.first, entry.second) || changed;

    if (changed)
        stack.notifyNotesChanged();

    return changed;
}


void PianoRollComponent::collectTwinsForEdit (std::vector<int>& twinNotes,
                                              std::vector<int>& twinClips) const
{
    twinNotes.clear();
    twinClips.clear();

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    for (const auto& entry : groupOccurrencesOf (selection, sampleSelection))
    {
        const int groupId    = entry.first;
        const int occurrence = entry.second;

        // An appearance the selection carries WHOLE is a per-copy decision:
        // deleting the chorus copy must not delete the verse. Only a PART of an
        // appearance is a change of shape.
        //
        // Phase 63: asked per appearance. `isWholeGroupSelected` answers "no"
        // as soon as a second group is in the selection, which turned every
        // multi-group edit into "sync to all my twins".
        if (isWholeOccurrenceSelected (groupId, occurrence))
            continue;

        // Ask per appearance: `collectTwinMembers` pairs by ordinal, and the
        // ordinal only means something inside one appearance.
        std::vector<int> sourceNotes, sourceClips;

        for (int id : selection)
            if (const auto* note = layer->findNote (id); note != nullptr
                  && note->groupId == groupId && note->groupOccurrence == occurrence)
                sourceNotes.push_back (id);

        for (int id : sampleSelection)
            if (const auto* clip = layer->findSample (id); clip != nullptr
                  && clip->groupId == groupId && clip->groupOccurrence == occurrence)
                sourceClips.push_back (id);

        std::vector<int> twinsOfNotes, twinsOfClips;
        layer->collectTwinMembers (sourceNotes, sourceClips, twinsOfNotes, twinsOfClips);

        for (int id : twinsOfNotes)
            if (std::find (twinNotes.begin(), twinNotes.end(), id) == twinNotes.end())
                twinNotes.push_back (id);

        for (int id : twinsOfClips)
            if (std::find (twinClips.begin(), twinClips.end(), id) == twinClips.end())
                twinClips.push_back (id);
    }
}


bool PianoRollComponent::simulateGroupMove (int layerIndex, int groupId,
                                            double deltaBeats, int deltaPitch,
                                            int occurrence)
{    if (stack.getActiveLayerIndex() != layerIndex)
        return false;
    auto* active = stack.getActiveLayer();

    if (active == nullptr || active->findGroup (groupId) == nullptr)
        return false;

    // Same rule as armGroupMove: a selection that already holds this appearance
    // whole is kept, so the self-test drives the gesture the user gets.
    if (! isWholeOccurrenceSelected (groupId, occurrence))
        selectGroup (groupId, occurrence);

    captureDragOriginals();
    captureGroupClips (groupId, occurrence);

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return false;

    const auto move = clampMove (dragOriginals, deltaBeats, deltaPitch, false, dragClipOriginals);
    applyMove (move);
    stack.notifyNotesChanged();

    return true;
}


juce::Rectangle<float> PianoRollComponent::groupFrameOf (int layerIndex, int groupId,
                                                         int occurrence) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    return group != nullptr ? getGroupContentBounds (*layer, *group, occurrence)
                            : juce::Rectangle<float>();
}


//==============================================================================
//  Phase 73: opening a group for member-level editing
//==============================================================================
void PianoRollComponent::enterGroupEdit (int groupId)
{
    if (groupId < 0 || groupId == editingGroupId)
        return;

    // Only a group that still exists can be opened. A double click can arrive
    // one message loop after an undo removed the group — the panel list is not
    // the only way in, so the id is checked here rather than trusted.
    if (layerIndexOfGroup (groupId) < 0)
        return;

    editingGroupId = groupId;

    // Opening is an edit-session change, not a selection change: whatever was
    // selected stays selected (the caller narrows it when the double click
    // named a specific member). The paint is the announcement.
    repaint();
}


void PianoRollComponent::exitGroupEdit()
{
    if (editingGroupId < 0)
        return;

    editingGroupId = -1;
    repaint();
}


void PianoRollComponent::armGroupMove (int groupId, int occurrence, juce::Point<float> position,
                                       bool additive)
{
    // Selecting is not moving: a group whose only gesture is "become the
    // selection" reads as broken the moment you try to drag it. So the same
    // click arms a rigid move of every member — the rules are unchanged, it is
    // still clampMove() over dragOriginals, exactly like dragging one of its
    // notes.
    //
    // Phase 58: ONE appearance, not the group. Grabbing the chorus copy must
    // leave the verse copy standing — that is the whole point of a reusable
    // group, and it is why the occurrence travels down into the capture.
    //
    // Phase 65: ...unless this appearance is ALREADY selected whole, which is
    // what a multi-selection looks like. Then grabbing its label means "drag
    // what I have selected", and replacing the selection with this one group
    // would drop the other groups on the floor at the moment the pointer moves.
    // "Select two groups, drag one" moves both; a click on a group that is not
    // part of the selection still narrows to it, exactly as before.
    //
    // Phase 74: Shift/Ctrl is the exception to that narrowing. It means "and
    // also this one" everywhere else in the app, and a modifier that quietly
    // threw the previous selection away would make "select three groups" cost
    // three separate drags. The whole appearance joins, never a lone member —
    // a member in the selection is its group in the selection.
    if (! isWholeOccurrenceSelected (groupId, occurrence))
    {
        if (additive)
            addOccurrenceToSelection (groupId, occurrence);
        else
            selectGroup (groupId, occurrence);
    }

    captureDragOriginals();
    captureGroupClips (groupId, occurrence);

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return;      // the group has no member on this sheet

    dragMode         = DragMode::moveNote;
    dragAnchorBeat   = beatAtScreenX (position.x);
    dragAnchorPitch  = pitchAtScreenY (position.y);

    if (! dragOriginals.empty())
        dragAuditionPitch = dragOriginals.front().pitch;

    dragResizeNoteId = -1;
}


int PianoRollComponent::resizeTargetId() const
{
    // The note whose end band was grabbed. Falls back to the first captured
    // note so a programmatic resize (a test arming the mode by hand) still
    // resizes something instead of silently doing nothing.
    for (const auto& origin : dragOriginals)
        if (origin.id == dragResizeNoteId)
            return dragResizeNoteId;

    return dragOriginals.empty() ? -1 : dragOriginals.front().id;
}


bool PianoRollComponent::applyNoteResize (const NoteEvent& origin, double wantedBeat, bool fromTail)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* note = active->findNote (origin.id);

    if (note == nullptr)
        return false;

    if (fromTail)
    {
        // The head does not move, so this is one subtraction with a floor — and
        // the floor is a grid step, not zero: a note that can be shrunk to
        // nothing is a note you can lose with a twitch. Phase 69: that step is
        // the VISIBLE one when it is finer than the 1/16, so a 1/64 grid can
        // produce a 1/64 note.
        note->lengthBeats = juce::jmax (minimumNoteLength(), wantedBeat - origin.startBeat);
        syncShapeOfMember (origin.id);
        return true;
    }

    // Trim the front: the tail stays where it was, which is what makes "make
    // this start later" not also mean "make this end later".
    const double end      = origin.startBeat + origin.lengthBeats;
    const double newStart = juce::jlimit (0.0,
                                          juce::jmax (0.0, end - minimumNoteLength()),
                                          wantedBeat);

    note->startBeat   = newStart;
    note->lengthBeats = juce::jmax (minimumNoteLength(), end - newStart);
    syncShapeOfMember (origin.id);
    return true;
}


bool PianoRollComponent::syncShapeOfMember (int noteId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    const auto* note = layer->findNote (noteId);

    if (note == nullptr || note->groupId < 0)
        return false;

    const int groupId = note->groupId;
    const int occurrence = note->groupOccurrence;

    if (! layer->syncGroupShape (groupId, occurrence))
        return false;

    stack.notifyNotesChanged();
    return true;
}


bool PianoRollComponent::syncShapesOfMembers (const std::vector<int>& noteIds,
                                             const std::vector<int>& clipIds)
{
    return syncGroupShapesAfterEdit (groupOccurrencesOf (noteIds, clipIds));
}


bool PianoRollComponent::syncShapeOfClip (int clipId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    const auto* clip = layer->findSample (clipId);

    if (clip == nullptr || clip->groupId < 0)
        return false;

    const int groupId = clip->groupId;
    const int occurrence = clip->groupOccurrence;

    if (! layer->syncGroupShape (groupId, occurrence))
        return false;

    stack.notifyNotesChanged();
    return true;
}


bool PianoRollComponent::simulateNoteResize (int noteId, double wantedBeat, bool fromTail)
{
    captureDragOriginals();
    dragResizeNoteId = noteId;

    const int target = resizeTargetId();
    bool changed = false;

    for (const auto& origin : dragOriginals)
        if (origin.id == target)
        {
            changed = applyNoteResize (origin, wantedBeat, fromTail);
            break;
        }

    dragOriginals.clear();
    dragResizeNoteId = -1;
    stack.notifyNotesChanged();
    return changed;
}


void PianoRollComponent::captureEditOriginals()
{
    // Notes first: captureDragOriginals() owns the clearing of both lists, so
    // this is "everything that moves", stated once instead of in each caller.
    captureDragOriginals();
    captureSampleDragOriginals();
}


std::pair<std::vector<int>, std::vector<int>>
PianoRollComponent::hitsInMarquee (juce::Rectangle<float> marquee) const
{
    std::pair<std::vector<int>, std::vector<int>> hits;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return hits;

    // Phase 29: the band grabs BOTH kinds. "Everything in this rectangle" is
    // what a marquee means, and making the user sweep twice — once for notes,
    // once for blocks — was the marquee answering a question nobody asked.
    const int index = stack.getActiveLayerIndex();

    // Phase 97: a band that started in the velocity lane sweeps BARS, not
    // rectangles — and therefore catches no blocks, because a block has no
    // velocity to show. The y of the band is still meaningful there: the bars
    // stand at different heights, so "the loud half of this bar" is a sweep
    // somebody may actually want.
    if (marqueeInLane)
    {
        for (const auto& note : active->getNotes())
            if (velocityBarFor (note).intersects (marquee))
                hits.first.push_back (note.id);

        return hits;
    }

    for (const auto& note : active->getNotes())
        if (getNoteBounds (note).intersects (marquee))
            hits.first.push_back (note.id);

    for (const auto& clip : active->getSamples())
        if (getSampleScreenBounds (index, clip.id).intersects (marquee))
            hits.second.push_back (clip.id);

    return hits;
}


bool PianoRollComponent::simulateMarquee (juce::Rectangle<float> marquee)
{
    auto hits = hitsInMarquee (marquee);

    if (hits.first.empty() && hits.second.empty())
    {
        // A sweep that catches nothing must not throw away what an additive one
        // was adding to — that is the difference between "nothing here" and
        // "forget everything".
        if (! marqueeAdditive)
            setMixedSelection ({}, {});

        return false;
    }

    // Phase 74: a band that clips ONE member has selected the GROUP. Without
    // this, a sweep over a corner of a group left six-sevenths of it dark and
    // the next Delete tore a member out of formation — the exact accident the
    // closed group exists to prevent. Groups open for editing are untouched:
    // there the member is the object the band was aimed at.
    expandToWholeOccurrences (hits.first, hits.second);

    if (marqueeAdditive)
    {
        // What was already selected leads, so a second sweep appends instead of
        // reshuffling a list the panels are showing.
        auto mergedNotes = selection;
        auto mergedClips = sampleSelection;

        for (int id : hits.first)
            if (std::find (mergedNotes.begin(), mergedNotes.end(), id) == mergedNotes.end())
                mergedNotes.push_back (id);

        for (int id : hits.second)
            if (std::find (mergedClips.begin(), mergedClips.end(), id) == mergedClips.end())
                mergedClips.push_back (id);

        hits.first  = std::move (mergedNotes);
        hits.second = std::move (mergedClips);
    }

    setMixedSelection (std::move (hits.first), std::move (hits.second));
    return true;
}


bool PianoRollComponent::simulateRulerZoom (float anchorX, float totalDy)
{
    const auto ruler = getRulerArea().toFloat();

    if (! ruler.contains (anchorX, ruler.getCentreY()))
        return false;

    // Exactly what mouseDown captures...
    dragMode                 = DragMode::rulerZoom;
    rulerDragAnchorBeatWidth = beatWidth;
    rulerZoomAnchorX         = anchorX;
    rulerZoomAnchorBeat      = beatAtScreenX (anchorX);

    // ...and exactly what mouseDrag does with the total travel. `totalDy` is the
    // signed distance the pointer moved, which is what the drag algorithm
    // measures; passing it once is the same as a drag with one mouse-move.
    setBeatWidthAround ((float) (rulerDragAnchorBeatWidth * std::pow (1.006, (double) totalDy)),
                        rulerZoomAnchorBeat, rulerZoomAnchorX);

    dragMode = DragMode::none;
    return true;
}
} // namespace atomic

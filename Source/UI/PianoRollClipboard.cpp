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
//  Phase 22: the note clipboard
//==============================================================================
namespace
{
    /** " · 3 个音符 · 2 个采样" — never a count of zero, because the numbers are
        what tell the user whether this clipboard is going to paste a group or a
        handful of notes. */
    juce::String membersDescription (int notes, int clips)
    {
        juce::String out;

        if (notes > 0)
            out = utf8 (" · ") + juce::String (notes) + utf8 (" 个音符");

        if (clips > 0)
            out += utf8 (" · ") + juce::String (clips) + utf8 (" 个采样");

        return out;
    }
}


bool PianoRollComponent::copyNotesToClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    if (selection.empty() && sampleSelection.empty())
        return false;

    // Phase 63: this fills ALL THREE clipboards — the objects, the loose notes
    // and the loose blocks — in one pass, because "which member of this
    // selection travels inside a copied object, and which travels on its own"
    // is one question with one answer. Filling them in separate passes (this
    // function, then copySamplesToClipboard) meant the block clipboard got a
    // list that included the blocks the group clipboard had already claimed:
    // one copy, two descriptions of it, and a paste that wrote both.
    noteClip.clear();
    noteClipFirstBeat = std::numeric_limits<double>::max();
    noteClipEndBeat   = 0.0;

    sampleClip.clear();
    sampleClipFirstBeat  = std::numeric_limits<double>::max();
    sampleClipLengthBeat = 1.0;

    // A fresh copy starts a fresh streak: keeping the old next-beat would send
    // the first paste of a new clipboard to the tail of the previous one.
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    groupClips.clear();

    //-- 1. Sort the selection into slices, one per appearance ----------------
    // Phase 63: the selection is split by (groupId, occurrence), not by kind.
    // Two groups selected together used to be one undifferentiated pile of
    // notes, which is why the copy could only be "a group" or "some notes" —
    // never "these two groups".
    using SliceKey = std::pair<int, int>;

    std::map<SliceKey, std::vector<NoteEvent>>   noteSlices;
    std::map<SliceKey, std::vector<SampleEvent>> clipSlices;
    std::vector<NoteEvent>   looseNotes;
    std::vector<SampleEvent> looseClips;

    for (const auto& note : active->getNotes())
    {
        if (! isSelected (note.id))
            continue;

        // A member of a group that is gone is a loose note; the dangling id is
        // the model's business, not the clipboard's.
        if (note.groupId < 0 || active->findGroup (note.groupId) == nullptr)
        {
            looseNotes.push_back (note);
            continue;
        }

        noteSlices[{ note.groupId, note.groupOccurrence }].push_back (note);
    }

    for (const auto& clip : active->getSamples())
    {
        if (! isSampleSelected (clip.id))
            continue;

        if (clip.groupId < 0 || active->findGroup (clip.groupId) == nullptr)
        {
            looseClips.push_back (clip);
            continue;
        }

        clipSlices[{ clip.groupId, clip.groupOccurrence }].push_back (clip);
    }

    //-- 2. A slice that IS a whole appearance becomes one clipboard object ---
    std::set<SliceKey> keys;

    for (const auto& entry : noteSlices)
        keys.insert (entry.first);

    for (const auto& entry : clipSlices)
        keys.insert (entry.first);

    for (const auto& key : keys)
    {
        const auto noteIt = noteSlices.find (key);
        const auto clipIt = clipSlices.find (key);

        auto* group = active->findGroup (key.first);
        const bool whole = group != nullptr && isWholeOccurrenceSelected (key.first, key.second);

        if (! whole)
        {
            // Part of an appearance, or no appearance at all: these travel as
            // members of nothing. Half a group is not a copyable object — the
            // paste would have to invent where the other half was.
            if (noteIt != noteSlices.end())
                for (const auto& note : noteIt->second)
                    looseNotes.push_back (note);

            if (clipIt != clipSlices.end())
                for (const auto& clip : clipIt->second)
                    looseClips.push_back (clip);

            continue;
        }

        // Phase 62: the appearance's PINNED origin, not the earliest member —
        // an appearance that lost its earliest note to an earlier edit still
        // sits where it was told to sit, and its curves are absolute to that.
        const double start = active->occurrenceStart (key.first, key.second);

        auto object = captureGroupClip (*group, start, key.second);

        if (noteIt != noteSlices.end())
            object.notes = noteIt->second;

        if (clipIt != clipSlices.end())
            object.clips = clipIt->second;

        groupClips.push_back (std::move (object));
    }

    //-- 3. The leftovers are the loose clipboard ----------------------------
    noteClip   = std::move (looseNotes);
    sampleClip = std::move (looseClips);

    for (const auto& note : noteClip)
    {
        noteClipFirstBeat = juce::jmin (noteClipFirstBeat, note.startBeat);
        noteClipEndBeat   = juce::jmax (noteClipEndBeat, note.startBeat + note.lengthBeats);
    }

    if (noteClip.empty())
        noteClipFirstBeat = 0.0;

    double clipEnd = 0.0;

    for (const auto& clip : sampleClip)
    {
        sampleClipFirstBeat = juce::jmin (sampleClipFirstBeat, clip.startBeat);
        clipEnd             = juce::jmax (clipEnd, clip.endBeat());
    }

    if (sampleClip.empty())
        sampleClipFirstBeat = 0.0;
    else
        sampleClipLengthBeat = juce::jmax (gridDivision, clipEnd - sampleClipFirstBeat);

    //-- 4. Say what happened ------------------------------------------------
    if (! groupClips.empty())
    {
        int noteCount = 0;
        int clipCount = 0;

        for (const auto& object : groupClips)
        {
            noteCount += (int) object.notes.size();
            clipCount += (int) object.clips.size();
        }

        flashStatus (utf8 ("已复制 ") + getGroupClipboardName()
                       + membersDescription (noteCount, clipCount));
        return true;
    }

    if (noteClip.empty() && sampleClip.empty())
        return false;

    if (noteClip.empty())
    {
        flashStatus (utf8 ("已复制 ") + juce::String ((int) sampleClip.size()) + utf8 (" 个采样"));
        return true;
    }

    flashStatus (utf8 ("已复制 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符"));
    return true;
}


juce::String PianoRollComponent::getGroupClipboardName() const
{
    if (groupClips.empty())
        return {};

    // One object answers with its name; two or more answer with a count. Naming
    // the first one would read as "you copied 组 A" when the clipboard also
    // holds 组 B.
    return groupClips.size() == 1 ? groupClips.front().name
                                  : juce::String ((int) groupClips.size()) + utf8 (" 个组");
}


bool PianoRollComponent::isWholeOccurrenceSelected (int groupId, int occurrence) const
{

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0 || occurrence < 0)
        return false;

    // Counted off the MODEL, not off the selection: the question is "did the
    // user get all of it", and a selection that also holds half of the next
    // group must not change the answer.
    int members = 0;
    int chosen  = 0;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId && note.groupOccurrence == occurrence)
        {
            ++members;

            if (isSelected (note.id))
                ++chosen;
        }

    for (const auto& clip : layer->getSamples())
        if (clip.groupId == groupId && clip.groupOccurrence == occurrence)
        {
            ++members;

            if (isSampleSelected (clip.id))
                ++chosen;
        }

    return members > 0 && chosen == members;
}


double PianoRollComponent::clipboardFirstBeat() const
{
    double first = std::numeric_limits<double>::max();

    for (const auto& object : groupClips)
        first = juce::jmin (first, object.startBeat);

    if (! noteClip.empty())
        first = juce::jmin (first, noteClipFirstBeat);

    if (! sampleClip.empty())
        first = juce::jmin (first, sampleClipFirstBeat);

    if (first == std::numeric_limits<double>::max())
        return 0.0;

    return juce::jmax (0.0, first);
}


double PianoRollComponent::clipboardLastBeat() const
{
    double last = 0.0;

    for (const auto& object : groupClips)
    {
        for (const auto& note : object.notes)
            last = juce::jmax (last, note.startBeat + note.lengthBeats);

        for (const auto& clip : object.clips)
            last = juce::jmax (last, clip.endBeat());
    }

    if (! noteClip.empty())
        last = juce::jmax (last, noteClipEndBeat);

    for (const auto& clip : sampleClip)
        last = juce::jmax (last, clip.endBeat());

    return last;
}


void PianoRollComponent::pasteLooseNotesInto (Layer& layer, double delta,
                                              std::vector<int>& newIds)
{
    // Loose notes arrive loose. The NoteEvent they were copied from still
    // carries its groupId, and pasting it unchanged would quietly drop the copy
    // back into the appearance the user copied it out of.
    for (auto note : noteClip)
    {
        note.startBeat       = juce::jmax (0.0, note.startBeat + delta);
        note.id              = 0;
        note.groupId         = -1;
        note.groupOccurrence = 0;

        if (const int newId = layer.addNote (note); newId > 0)
            newIds.push_back (newId);
    }
}


void PianoRollComponent::pasteLooseSamplesInto (Layer& layer, double delta,
                                                std::vector<int>& newIds)
{
    for (auto clip : sampleClip)
    {
        clip.startBeat = juce::jmax (0.0, clip.startBeat + delta);
        clip.id        = 0;

        clip.groupId         = -1;
        clip.groupOccurrence = 0;

        // A CUT takes its sampler with it, so the slot this clipboard holds may
        // already be gone.
        ensureClipSlot (clip);

        if (const int newId = layer.addSample (std::move (clip)); newId > 0)
        {
            newIds.push_back (newId);

            if (const auto* created = layer.findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }
}


bool PianoRollComponent::cutNotesToClipboard()
{
    if (! copyNotesToClipboard())
        return false;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // Phase 56: copying a GROUP now puts its blocks on the clipboard too, so a
    // cut of that group has to take them off the timeline as well — otherwise
    // "cut a group" leaves its audio behind and the paste creates a second copy
    // of blocks nobody moved.
    //
    // Phase 63: every selected block leaves, not just the ones the block
    // clipboard happens to describe. Since the copy now splits its blocks
    // between "member of a copied object" and "loose", reading the cut list off
    // one of the two clipboards left the other half on the timeline.
    for (int id : selection)
        active->removeNote (id);

    for (int id : sampleSelection)
        active->removeSample (id);

    selection.clear();
    sampleSelection.clear();

    hoveredNoteId = -1;
    notifySelectionChanged();
    pruneEmptyGroups();
    stack.notifyNotesChanged();

    flashStatus ((! groupClips.empty()
                    ? utf8 ("已剪切 ") + getGroupClipboardName()
                    : utf8 ("已剪切 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符")));
    return true;
}


PianoRollComponent::GroupClip PianoRollComponent::captureGroupClip (const NoteGroup& group,
                                                                     double startBeat,
                                                                     int occurrence) const
{
    GroupClip clip;

    clip.sourceGroupId    = group.getId();
    clip.sourceOccurrence = juce::jmax (0, occurrence);
    clip.name             = group.getName();
    clip.colour       = group.getColour();
    clip.outputSlotId = group.getOutputSlotId();
    clip.levelDb      = group.getLevelDb();
    clip.pan          = group.getPan();
    clip.pluginParam  = group.getPluginParam();
    clip.startBeat    = startBeat;

    for (int i = 0; i < numGroupAutomationTargets; ++i)
    {
        const auto target = (GroupAutomationTarget) i;
        clip.lanes[(size_t) i]  = group.automationFor (target);
        clip.laneOn[(size_t) i] = group.automationFor (target).isEnabled();
    }

    return clip;
}


int PianoRollComponent::addGroupCopy (Layer& layer, const GroupClip& source, double deltaBeats)
{
    // Created before the members so they can be born inside it: a note whose
    // groupId points at a group that does not exist yet would be repaired to
    // "ungrouped" by the model, and the copy would paste as loose notes.
    //
    // Phase 57: a PASTE no longer comes through here when the source group is
    // still alive (it reuses that group instead). This is now the branch for
    // "the source is gone" — a cut swept it away, or the paste landed on
    // another sheet — plus the deliberate copy that "复制到后一小节" makes.
    const int newId = layer.addGroup (uniqueGroupName (layer, source.name), source.colour);

    auto* copy = layer.findGroup (newId);

    if (copy == nullptr)
        return -1;

    copy->setOutputSlotId (source.outputSlotId);
    copy->setLevelDb (source.levelDb);
    copy->setPan (source.pan);
    copy->setPluginParam (source.pluginParam);
    copy->setCollapsed (false);

    for (int i = 0; i < numGroupAutomationTargets; ++i)
    {
        auto& lane = copy->automationFor ((GroupAutomationTarget) i);

        // A group lane is ABSOLUTE time, so a copy that lands later has to move
        // its curve with it — otherwise pasting four bars later would keep the
        // shape at the old beats and the notes would arrive at a flat line.
        auto points = source.lanes[(size_t) i].getPoints();

        for (auto& point : points)
            point.beat = juce::jmax (0.0, point.beat + deltaBeats);

        lane.loadPoints (std::move (points));
        lane.setEnabled (source.laneOn[(size_t) i]);
    }

    return newId;
}


void PianoRollComponent::insertGroupMembers (Layer& layer, int groupId, int occurrence,
                                             double delta,
                                             const std::vector<NoteEvent>& notes,
                                             const std::vector<SampleEvent>& clips,
                                             std::vector<int>& newNotes,
                                             std::vector<int>& newClips)
{
    // Phase 62: where this appearance sits, measured on the members that are
    // about to land. Stored before anything can be deleted, because from now on
    // the origin is what "same shape, other place" is measured from — deriving
    // it later would follow the earliest surviving member and drift.
    double origin = std::numeric_limits<double>::max();

    for (const auto& note : notes)
        origin = juce::jmin (origin, juce::jmax (0.0, note.startBeat + delta));

    for (const auto& clip : clips)
        origin = juce::jmin (origin, juce::jmax (0.0, clip.startBeat + delta));

    if (origin < std::numeric_limits<double>::max())
        layer.setOccurrenceStart (groupId, occurrence, origin);

    for (auto note : notes)
    {
        note.startBeat       = juce::jmax (0.0, note.startBeat + delta);
        note.id              = 0;
        note.groupId         = groupId;
        note.groupOccurrence = occurrence;

        if (const int newId = layer.addNote (note); newId > 0)
            newNotes.push_back (newId);
    }

    for (auto clip : clips)
    {
        clip.startBeat = juce::jmax (0.0, clip.startBeat + delta);
        clip.id        = 0;

        // Phase 56: a pasted block belongs to the group the paste targets, never
        // -1 — that was the bug: a group with blocks in it pasted as a group
        // plus loose audio beside it. Phase 58: and to the same APPEARANCE as
        // the notes beside it, which is what keeps the copy independent.
        clip.groupId         = groupId;
        clip.groupOccurrence = occurrence;

        ensureClipSlot (clip);

        if (const int newId = layer.addSample (std::move (clip)); newId > 0)
        {
            newClips.push_back (newId);

            if (const auto* created = layer.findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }
}


void PianoRollComponent::ensureClipSlot (SampleEvent& clip)
{
    // Phase 33: a CUT takes its sampler with it — the sweep runs the moment the
    // last block leaves, so the slot a clipboard holds may be gone by the time
    // it is pasted. Re-registering the file is what makes cut-then-paste a move
    // instead of a block pointed at nothing (which would quietly play the
    // layer's instrument instead of the sample).
    if (clip.outputSlotId >= 0 && rack.findSlot (clip.outputSlotId) == nullptr && clip.audio != nullptr)
        clip.outputSlotId = rack.addSampler (clip.audio, clip.path, clip.name);
}


bool PianoRollComponent::pasteNotesFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // Phase 57: a group on the clipboard is ONE object with ONE paste, so every
    // entry point goes through pasteGroupFromClipboard. This function used to
    // mint a second group for it — the very thing the user asked to stop — and
    // two paths deciding "does a paste create a group" is how they drift.
    //
    // Phase 63: "a group" became "however many objects the clipboard holds",
    // and the loose notes are pasted by that same transaction now, so a mixed
    // selection lands with one delta instead of two.
    if (! groupClips.empty())
        return pasteGroupFromClipboard();

    if (noteClip.empty())
        return false;

    // Paste at the playhead when there is one — that is the only landmark on
    // this timeline that means "here". With the playhead parked exactly on the
    // block the copy came from, shift by the block's own length, because a
    // paste that lands on top of its source looks like nothing happened.
    double anchor = playheadBeat >= 0.0 ? playheadBeat : noteClipFirstBeat;

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - noteClipFirstBeat) < 1.0e-9)
        anchor = noteClipFirstBeat + juce::jmax (minimumNoteLength(), noteClipEndBeat - noteClipFirstBeat);

    // Same rounding as a drag: the anchor is snapped once and every note keeps
    // its offset, so the block arrives with its internal timing intact.
    const double delta = anchor - noteClipFirstBeat;

    // Loose notes only: a group clipboard returned above, so there is no group
    // to hand these to (Phase 57). Phase 62: and they must ARRIVE loose — the
    // NoteEvent they were copied from still carries its groupId, and pasting it
    // unchanged would quietly drop the copy back into the appearance the user
    // copied it out of (one bracket around two stretches, which is the thing
    // Phase 58 exists to stop).
    std::vector<int> newIds;
    pasteLooseNotesInto (*active, delta, newIds);

    if (newIds.empty())
        return false;

    ensureTimelineExtendsTo (anchor + juce::jmax (minimumNoteLength(), noteClipEndBeat - noteClipFirstBeat) + 4.0);
    setSelection (newIds);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已粘贴 ") + juce::String ((int) newIds.size()) + utf8 (" 个音符"));

    repaint();
    return true;
}


bool PianoRollComponent::pasteGroupFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || groupClips.empty())
        return false;

    //-- The anchor ----------------------------------------------------------
    // The whole clipboard gets ONE anchor — the earliest beat anything on it
    // stands at. A block-only group has no notes to read a start from, and a
    // group whose notes begin later than its first block would otherwise paste
    // shifted by that difference.
    //
    // Phase 63: one anchor for N objects is what keeps their SPACING. Anchoring
    // each group on its own start would stack "two groups four beats apart" on
    // a single beat and look like the merge this phase exists to undo.
    const double sourceFirst = clipboardFirstBeat();
    const double sourceEnd   = clipboardLastBeat();

    double anchor = playheadBeat >= 0.0 ? playheadBeat : sourceFirst;

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - sourceFirst) < 1.0e-9)
        anchor = sourceFirst + juce::jmax (minimumNoteLength(), sourceEnd - sourceFirst);

    const double delta = anchor - sourceFirst;

    std::vector<int> newNotes;
    std::vector<int> newClips;

    for (const auto& object : groupClips)
    {
        //-- Phase 57: a paste reuses the group it was copied from ------------
        // It used to mint a SECOND group ("Synth A 2"), so copying a group and
        // pasting it a bar later split the very thing the user copied into two
        // rows of the group list — two names, two trims to keep in sync, two
        // mounts. The clipboard carries the source id and the paste lends the
        // new members to that same group: the copy IS the group, in a second
        // place.
        //
        // The id is re-checked rather than trusted. Cut-then-paste sweeps the
        // emptied group away, and a paste on another sheet cannot resolve an id
        // from this one; in both cases the members still have to arrive inside
        // SOME group, so one is built from the clipboard's own name and colour.
        //
        // Phase 63: per OBJECT, so a clipboard holding two different groups
        // pastes two — and a clipboard holding two appearances of ONE group
        // pastes two more appearances of it rather than one bigger one.
        const bool reused = object.sourceGroupId >= 0
                              && active->findGroup (object.sourceGroupId) != nullptr;

        const int newGroupId = reused ? object.sourceGroupId
                                      : addGroupCopy (*active, object, delta);

        if (newGroupId < 0)
            continue;

        //-- Phase 58: the paste is a NEW APPEARANCE, not more members -------
        // Phase 57 lent the members to the group itself, which made one bracket
        // swallow both stretches: dragging the copy dragged the original, and
        // the only way to separate them was to disband the group. The paste
        // places another appearance of the same definition — same name, same
        // trim, same mount, same curves — with members of its own.
        const int occurrence = active->nextGroupOccurrence (newGroupId);

        std::vector<int> objectNotes;
        std::vector<int> objectClips;

        insertGroupMembers (*active, newGroupId, occurrence, delta,
                            object.notes, object.clips, objectNotes, objectClips);

        if (objectNotes.empty() && objectClips.empty())
        {
            // Only a group this paste CREATED may be thrown away — the reused
            // one was here before the paste and is not ours to delete.
            if (! reused)
                active->removeGroup (newGroupId);   // nothing landed; no bracket

            continue;
        }

        newNotes.insert (newNotes.end(), objectNotes.begin(), objectNotes.end());
        newClips.insert (newClips.end(), objectClips.begin(), objectClips.end());
    }

    //-- The loose leftovers of a mixed selection ----------------------------
    // Same delta as the objects: a selection of "two groups and a stray note"
    // has to land in the same relationship it was copied in, or the stray note
    // arrives somewhere it never stood.
    pasteLooseNotesInto (*active, delta, newNotes);
    pasteLooseSamplesInto (*active, delta, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;

    ensureTimelineExtendsTo (anchor + juce::jmax (minimumNoteLength(), sourceEnd - sourceFirst) + 4.0);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已粘贴 ") + getGroupClipboardName()
                   + membersDescription ((int) newNotes.size(), (int) newClips.size()));

    repaint();
    return true;
}


bool PianoRollComponent::duplicateGroupToNextBar (int layerIndex, int groupId, int occurrence)
{
    // The selection this command writes is the ACTIVE layer's, so a group on
    // another sheet has to bring that sheet forward first — otherwise the copy
    // lands on one layer and the highlight on another.
    if (stack.getActiveLayerIndex() != layerIndex)
        stack.setActiveLayerIndex (layerIndex);

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return false;

    // Read once: adding the copy reallocates the group list, so `group` must
    // not be dereferenced after that (P53's dangling-pointer lesson, one
    // container over).
    const juce::String groupName = group->getName();

    std::vector<NoteEvent>    notes;
    std::vector<SampleEvent>  clips;
    double first = std::numeric_limits<double>::max();
    double last  = 0.0;

    // Phase 58: one appearance, unless the caller does not know which (the
    // older "copy the whole group" call, and the self-test, pass -1).
    const auto belong = [groupId, occurrence] (int memberGroup, int memberOccurrence)
    {
        return memberGroup == groupId && (occurrence < 0 || memberOccurrence == occurrence);
    };

    for (const auto& note : layer->getNotes())
        if (belong (note.groupId, note.groupOccurrence))
        {
            notes.push_back (note);
            first = juce::jmin (first, note.startBeat);
            last  = juce::jmax (last, note.endBeat());
        }

    for (const auto& clip : layer->getSamples())
        if (belong (clip.groupId, clip.groupOccurrence))
        {
            clips.push_back (clip);
            first = juce::jmin (first, clip.startBeat);
            last  = juce::jmax (last, clip.endBeat());
        }

    if (notes.empty() && clips.empty())
        return false;

    if (first == std::numeric_limits<double>::max())
        first = 0.0;

    //-- Where the copy goes ------------------------------------------------
    // "复制到后一小节" is the next BAR LINE, not "start + 4 beats" — the two are
    // the same only for a group that starts on a bar line, and a copy that
    // lands on the source is not a copy. If the group is longer than the gap,
    // the next line after its own end is used instead, so the two never
    // overlap.
    double target = nextBarLineAfter (first, gridDivision);

    if (target < last - 1.0e-9)
        target = barLineAtOrAfter (last);

    const double delta = target - first;

    // Phase 58: another APPEARANCE of the same group, not a copy of it. The
    // definition stays one row in the group list; what multiplies is the thing
    // on the timeline, and each one moves on its own. That also means there is
    // no second NoteGroup to build — the name, trim, mount and curves are
    // already there, and the new members simply join them.
    const int copyId        = groupId;
    const int newOccurrence = layer->nextGroupOccurrence (groupId);

    std::vector<int> newNotes;
    std::vector<int> newClips;

    insertGroupMembers (*layer, copyId, newOccurrence, delta, notes, clips, newNotes, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;       // nothing landed; the group is not ours to delete

    ensureTimelineExtendsTo (last + delta + beatsPerBar);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已复制 ") + groupName + membersDescription ((int) newNotes.size(), (int) newClips.size())
                   + utf8 (" 到第 ") + juce::String ((int) std::floor (target / beatsPerBar) + 1)
                   + utf8 (" 小节"));

    repaint();
    return true;
}


bool PianoRollComponent::placeGroupAppearance (int layerIndex, int groupId, double atBeat)
{
    // The selection and the notifications this ends with are the ACTIVE layer's,
    // and the row may have come from another sheet — so bring it forward first,
    // for the same reason duplicateGroupToNextBar does.
    if (stack.getActiveLayerIndex() != layerIndex)
        stack.setActiveLayerIndex (layerIndex);

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return false;

    // Read before anything grows a vector: adding members can reallocate the
    // group list, so `group` must not be dereferenced after this point.
    const juce::String groupName = group->getName();

    //-- Template: the group's FIRST appearance -----------------------------
    // The row in the list is "this group", not "wherever it was last put".
    // First appearance keeps the answer stable: drag twice from the same row
    // and you get the same shape twice, instead of a copy of a copy.
    const auto occurrences = layer->getGroupOccurrences (groupId);

    if (occurrences.empty())
        return false;

    const int source = occurrences.front();

    std::vector<NoteEvent>   notes;
    std::vector<SampleEvent> clips;
    double first = std::numeric_limits<double>::max();
    double last  = 0.0;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId && note.groupOccurrence == source)
        {
            notes.push_back (note);
            first = juce::jmin (first, note.startBeat);
            last  = juce::jmax (last, note.endBeat());
        }

    for (const auto& clip : layer->getSamples())
        if (clip.groupId == groupId && clip.groupOccurrence == source)
        {
            clips.push_back (clip);
            first = juce::jmin (first, clip.startBeat);
            last  = juce::jmax (last, clip.endBeat());
        }

    if (notes.empty() && clips.empty())
        return false;       // nothing to place; do not invent an empty body

    if (first == std::numeric_limits<double>::max())
        first = 0.0;

    // The pointer's beat is where the FIRST member goes, not where the group
    // object starts in some abstract sense: the members are what you can see.
    const double target = juce::jmax (0.0, atBeat);
    const double delta  = target - first;

    // Phase 58: another appearance of the SAME definition. Same name, colour,
    // mount, trims and curves — the group list gains no row, and this copy
    // moves on its own because it is a different (groupId, occurrence) pair.
    const int newOccurrence = layer->nextGroupOccurrence (groupId);

    std::vector<int> newNotes;
    std::vector<int> newClips;

    insertGroupMembers (*layer, groupId, newOccurrence, delta, notes, clips, newNotes, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;

    ensureTimelineExtendsTo (last + delta + beatsPerBar);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (groupName + utf8 (" 又放了一处：")
                   + membersDescription ((int) newNotes.size(), (int) newClips.size()));

    repaint();
    return true;
}


std::vector<int> PianoRollComponent::duplicateSelectedSamples (double offsetBeats)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return {};

    std::vector<SampleEvent> sources;
    double longest = 0.0;

    for (const auto& clip : active->getSamples())
        if (isSampleSelected (clip.id))
        {
            sources.push_back (clip);
            longest = juce::jmax (longest, clip.lengthBeats);
        }

    if (sources.empty())
        return {};

    // "Copy to the next bar" means "clear of the source", so the default offset
    // is the length of the longest copied block, not a fixed beat count: two
    // five-bar blocks copied by one bar would overlap themselves.
    const double offset = offsetBeats >= 0.0 ? offsetBeats : juce::jmax (gridDivision, longest);

    std::vector<int> newIds;

    for (auto clip : sources)
    {
        clip.startBeat += offset;
        clip.id = 0;                 // addSample() assigns the next id

        // A copy is a new object, not a second membership: inheriting the group
        // would make the group's bracket swallow the copy and drag it later on,
        // which is not what "duplicate" promised.
        clip.groupId = -1;

        const int newId = active->addSample (std::move (clip));

        if (newId > 0)
        {
            newIds.push_back (newId);

            if (const auto* created = active->findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }

    if (newIds.empty())
        return {};

    setSampleSelection (newIds);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已复制 ") + juce::String ((int) newIds.size()) + utf8 (" 个采样"));
    return newIds;
}


bool PianoRollComponent::copySamplesToClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return false;

    sampleClip.clear();
    sampleClipFirstBeat = std::numeric_limits<double>::max();
    double endBeat = 0.0;

    for (const auto& clip : active->getSamples())
    {
        if (! isSampleSelected (clip.id))
            continue;

        sampleClip.push_back (clip);
        sampleClipFirstBeat = juce::jmin (sampleClipFirstBeat, clip.startBeat);
        endBeat             = juce::jmax (endBeat, clip.endBeat());
    }

    if (sampleClip.empty())
    {
        sampleClipFirstBeat = 0.0;
        return false;
    }

    sampleClipLengthBeat = juce::jmax (gridDivision, endBeat - sampleClipFirstBeat);

    // Phase 37: a fresh copy starts a fresh streak. Keeping the old next-beat
    // would send the first paste of a new clipboard to the tail of the previous
    // one instead of to the playhead.
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    flashStatus (utf8 ("已复制 ") + juce::String ((int) sampleClip.size()) + utf8 (" 个采样"));
    return true;
}


bool PianoRollComponent::cutSamplesToClipboard()
{
    if (! copySamplesToClipboard())
        return false;

    return deleteSelectedSamples();
}


bool PianoRollComponent::pasteSamplesFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleClip.empty())
        return false;

    //-- Where this paste lands (Phase 37) ------------------------------------
    // Three rules, in this order.
    //
    //  1. A streak of Ctrl+V steps forward. The first paste goes to the
    //     playhead (or the clipboard's own start); every repeat starts where
    //     the previous one ended. With one fixed anchor the second Ctrl+V
    //     lands exactly on the first, which is why "paste three times" used to
    //     leave one block on the timeline and look like the earlier pastes had
    //     been dragged away. Moving the playhead ends the streak.
    //
    //  2. With snap on, the LANDING BEAT is a grid line. Snapping the anchor
    //     alone used to be enough only while the step-forward came before the
    //     snap; applying it after handed the next paste a beat that is off the
    //     grid whenever the clipboard length is not whole grid steps. So the
    //     step is taken in grid units, and the advance recorded for the next
    //     paste is rounded UP to a grid line as well.
    //
    //  3. A paste that would land on its own source moves past it. Rounding
    //     can eat that move (grid 0.25, clip 0.375 → 0.25 lands inside the
    //     source), so this step rounds up.
    auto snapUp = [this] (double beat)
    {
        return std::ceil (beat / gridDivision - 1.0e-9) * gridDivision;
    };

    const bool continuing = samplePasteNextBeat >= 0.0
                             && std::abs (playheadBeat - samplePastePlayheadSeen) < 1.0e-9;

    double anchor = continuing ? samplePasteNextBeat
                              : (playheadBeat >= 0.0 ? playheadBeat : sampleClipFirstBeat);

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - sampleClipFirstBeat) < 1.0e-9)
    {
        anchor += sampleClipLengthBeat;

        if (snapEnabled)
            anchor = snapUp (anchor);
    }

    const double delta = anchor - sampleClipFirstBeat;

    // A pasted block does NOT inherit the group it was cut from. Membership
    // means "these move together", and a copy that lands four bars later is not
    // part of the shape the original group describes. One implementation with
    // the group paste's leftovers, so the two cannot disagree.
    std::vector<int> newIds;
    pasteLooseSamplesInto (*active, delta, newIds);

    if (newIds.empty())
        return false;

    setSampleSelection (newIds);
    stack.notifyNotesChanged();

    // The next paste in this streak starts where this one stopped — computed
    // from the beat actually written, so a clipboard that spans gaps between
    // its blocks steps by its real width rather than by a block count.
    samplePastePlayheadSeen = playheadBeat;
    samplePasteNextBeat     = snapEnabled ? snapUp (anchor + sampleClipLengthBeat)
                                          : anchor + sampleClipLengthBeat;

    flashStatus (utf8 ("已粘贴 ") + juce::String ((int) newIds.size()) + utf8 (" 个采样"));
    return true;
}


//==============================================================================
//  Phase 29: the edit commands, over a possibly mixed selection
//==============================================================================
void PianoRollComponent::copySelectionToClipboards()
{
    // The clipboards are written by ONE gesture, and the ones with nothing
    // selected are EMPTIED. That is the whole point: a paste must not resurrect
    // the clips from a copy three actions ago just because this time only notes
    // happened to be selected.
    //
    // Phase 63: one pass fills all three — objects, loose notes, loose blocks.
    // Running copySamplesToClipboard() afterwards (as this used to) handed the
    // block clipboard a list that still contained the blocks the object
    // clipboard had already claimed: two descriptions of one copy, and a paste
    // that wrote both.
    if (! selection.empty() || ! sampleSelection.empty())
    {
        copyNotesToClipboard();
        return;
    }

    noteClip.clear();
    noteClipFirstBeat = 0.0;
    noteClipEndBeat   = 0.0;

    sampleClip.clear();
    sampleClipFirstBeat     = 0.0;
    sampleClipLengthBeat    = 1.0;
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    groupClips.clear();
}


bool PianoRollComponent::pasteEditClipboard()
{
    //-- Phase 56: a copied object pastes as ONE transaction ----------------
    // Not "the notes, then the blocks": two passes mean two anchors and two
    // halves that do not know about each other — the notes landed in a new
    // group and the blocks landed beside it, ungrouped. A group is one thing,
    // so the paste is one transaction.
    //
    // Phase 63: "a group" became "every object on the clipboard", pasted by the
    // same code with one shared anchor. That is what makes "copy two groups,
    // paste" put back two groups instead of one group holding both.
    if (! groupClips.empty())
        return pasteGroupFromClipboard();

    const bool haveNotes = canPasteNotes();
    const bool haveClips = canPasteSamples();

    if (! haveNotes && ! haveClips)
        return false;

    if (! haveNotes)
        return pasteSamplesFromClipboard();

    if (! haveClips)
        return pasteNotesFromClipboard();

    // Each paste leaves exactly what it inserted selected, so the mixed result
    // is those two answers put back together. Doing it this way keeps a single
    // implementation of "where does a note land" and "where does a block land"
    // — the two anchors are allowed to differ, and they do.
    selection.clear();
    pasteNotesFromClipboard();
    const auto pastedNotes = selection;

    pasteSamplesFromClipboard();
    const auto pastedClips = sampleSelection;

    if (pastedNotes.empty() || pastedClips.empty())
        return true;                    // one kind landed; the other is empty

    setMixedSelection (pastedNotes, pastedClips);
    return true;
}


void PianoRollComponent::duplicateEditSelection()
{
    // Capture the clips BEFORE the note pass: selecting the note copies lets go
    // of the clips (see setSelection), and "one gesture, one copy" has to
    // survive that.
    const auto clipIds = sampleSelection;
    const auto newNotes = duplicateSelectedNotes (1.0);

    if (clipIds.empty())
        return;

    sampleSelection = clipIds;                    // put them back for the clip pass
    const auto newClips = duplicateSelectedSamples();

    if (! newClips.empty() && ! newNotes.empty())
        setMixedSelection (newNotes, newClips);
}
} // namespace atomic

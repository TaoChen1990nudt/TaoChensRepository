#include "LayerModel.h"

namespace atomic
{

//==============================================================================
namespace
{
    /** Layer ids are never reused, even across a project load: automation tracks
        bind to them, and a recycled id would silently reattach a curve to a
        different sheet. restoreLayers() raises the counter past whatever the
        file used, so a post-load "add layer" cannot collide either. */
    int& layerIdCounter()
    {
        static int counter = 0;
        return counter;
    }

    /** Phase 16: group ids are unique across the WHOLE session, not per layer.

        An automation track addresses its owner by (kind, id) alone, so a
        per-layer counter would let layer 1's "group 1" and layer 2's "group 1"
        both answer to the same track — and the window would edit whichever it
        happened to find first. Global ids cost one shared counter and remove
        that class of bug entirely. */
    int& groupIdCounter()
    {
        static int counter = 0;
        return counter;
    }
}

Layer::Layer (juce::String layerName, juce::Colour layerColour, int initialOutputSlotId)
    : name (std::move (layerName)),
      colour (layerColour),
      outputSlotId (initialOutputSlotId)
{
    id = ++layerIdCounter();
}

//==============================================================================
NoteGroup::NoteGroup (int groupId, juce::String groupName, juce::Colour groupColour)
    : id (groupId), name (std::move (groupName)), colour (groupColour)
{
}

//==============================================================================
int Layer::addGroup (juce::String groupName, juce::Colour groupColour)
{
    const int newId = ++groupIdCounter();
    groups.emplace_back (newId, std::move (groupName), groupColour);
    return newId;
}

NoteGroup* Layer::findGroup (int groupId) noexcept
{
    if (groupId < 0)
        return nullptr;

    for (auto& g : groups)
        if (g.getId() == groupId)
            return &g;

    return nullptr;
}

const NoteGroup* Layer::findGroup (int groupId) const noexcept
{
    return const_cast<Layer*> (this)->findGroup (groupId);
}

void Layer::removeGroup (int groupId)
{
    if (findGroup (groupId) == nullptr)
        return;

    // Detach first: membership lives on the notes, so dissolving a group is
    // exactly "clear the field on everything that pointed here". Nothing else
    // has to be pruned, and no note can be left in a group that no longer
    // exists.
    for (auto& n : notes)
        if (n.groupId == groupId)
        {
            n.groupId         = -1;
            n.groupOccurrence = 0;
        }

    // Phase 27: clips are members too, and they are detached the same way. A
    // clip left pointing at a dissolved group would be a member of nothing that
    // still draws inside somebody's bracket.
    for (auto& s : samples)
        if (s.groupId == groupId)
        {
            s.groupId         = -1;
            s.groupOccurrence = 0;
        }

    groups.erase (std::remove_if (groups.begin(), groups.end(),
                                  [groupId] (const NoteGroup& g) { return g.getId() == groupId; }),
                  groups.end());
}

void Layer::assignNotesToGroup (const std::vector<int>& noteIds, int groupId, int occurrence)
{
    // A group id that does not exist is the same as "no group": better to
    // detach than to leave notes pointing at a ghost.
    const int target = findGroup (groupId) != nullptr ? groupId : -1;
    const int which  = target >= 0 ? juce::jmax (0, occurrence) : 0;

    for (int noteId : noteIds)
        if (auto* note = findNote (noteId))
        {
            note->groupId         = target;
            note->groupOccurrence = which;
        }
}

int Layer::getGroupIdOfNote (int noteId) const noexcept
{
    for (const auto& n : notes)
        if (n.id == noteId)
            return n.groupId;

    return -1;
}

int Layer::getGroupOccurrenceOfNote (int noteId) const noexcept
{
    for (const auto& n : notes)
        if (n.id == noteId)
            return n.groupId >= 0 ? n.groupOccurrence : 0;

    return 0;
}

//==============================================================================
std::vector<int> Layer::getGroupOccurrences (int groupId) const
{
    std::vector<int> occurrences;

    if (groupId < 0)
        return occurrences;

    const auto add = [&occurrences] (int occurrence)
    {
        if (std::find (occurrences.begin(), occurrences.end(), occurrence) == occurrences.end())
            occurrences.push_back (occurrence);
    };

    for (const auto& n : notes)
        if (n.groupId == groupId)
            add (n.groupOccurrence);

    for (const auto& s : samples)
        if (s.groupId == groupId)
            add (s.groupOccurrence);

    std::sort (occurrences.begin(), occurrences.end());
    return occurrences;
}

int Layer::nextGroupOccurrence (int groupId) const
{
    const auto occurrences = getGroupOccurrences (groupId);

    return occurrences.empty() ? 0 : occurrences.back() + 1;
}

std::vector<int> Layer::getOccurrenceNoteIds (int groupId, int occurrence) const
{
    std::vector<int> ids;

    if (groupId < 0)
        return ids;

    for (const auto& n : notes)
        if (n.groupId == groupId && n.groupOccurrence == occurrence)
            ids.push_back (n.id);

    return ids;
}

int Layer::countNotesInOccurrence (int groupId, int occurrence) const noexcept
{
    if (groupId < 0)
        return 0;

    int count = 0;

    for (const auto& n : notes)
        if (n.groupId == groupId && n.groupOccurrence == occurrence)
            ++count;

    return count;
}

//==============================================================================
void Layer::assignSamplesToGroup (const std::vector<int>& sampleIds, int groupId, int occurrence)
{
    const int target = findGroup (groupId) != nullptr ? groupId : -1;
    const int which  = target >= 0 ? juce::jmax (0, occurrence) : 0;

    for (int sampleId : sampleIds)
        if (auto* sample = findSample (sampleId))
        {
            sample->groupId         = target;
            sample->groupOccurrence = which;
        }
}

int Layer::getGroupIdOfSample (int sampleId) const noexcept
{
    if (const auto* sample = findSample (sampleId))
        return sample->groupId;

    return -1;
}

int Layer::getGroupOccurrenceOfSample (int sampleId) const noexcept
{
    if (const auto* sample = findSample (sampleId))
        return sample->groupId >= 0 ? sample->groupOccurrence : 0;

    return 0;
}

std::vector<int> Layer::getOccurrenceSampleIds (int groupId, int occurrence) const
{
    std::vector<int> ids;

    if (groupId < 0)
        return ids;

    for (const auto& s : samples)
        if (s.groupId == groupId && s.groupOccurrence == occurrence)
            ids.push_back (s.id);

    return ids;
}

int Layer::countSamplesInOccurrence (int groupId, int occurrence) const noexcept
{
    if (groupId < 0)
        return 0;

    int count = 0;

    for (const auto& s : samples)
        if (s.groupId == groupId && s.groupOccurrence == occurrence)
            ++count;

    return count;
}

int Layer::countSamplesInGroup (int groupId) const noexcept
{
    if (groupId < 0)
        return 0;

    int count = 0;

    for (const auto& s : samples)
        if (s.groupId == groupId)
            ++count;

    return count;
}

std::vector<int> Layer::getGroupSampleIds (int groupId) const
{
    std::vector<int> ids;

    if (groupId < 0)
        return ids;

    for (const auto& s : samples)
        if (s.groupId == groupId)
            ids.push_back (s.id);

    return ids;
}

//==============================================================================
//  Phase 62: one shape, several places
//==============================================================================
bool NoteGroup::hasOccurrenceStart (int occurrence) const
{
    return occurrenceStarts.find (occurrence) != occurrenceStarts.end();
}

double NoteGroup::getOccurrenceStart (int occurrence) const
{
    const auto found = occurrenceStarts.find (occurrence);

    return found != occurrenceStarts.end() ? found->second : 0.0;
}

void NoteGroup::setOccurrenceStart (int occurrence, double beat)
{
    occurrenceStarts[occurrence] = beat;
}

void NoteGroup::forgetOccurrenceStart (int occurrence)
{
    occurrenceStarts.erase (occurrence);
}

double Layer::occurrenceStart (int groupId, int occurrence)
{
    auto* group = findGroup (groupId);

    if (group == nullptr)
        return 0.0;

    if (group->hasOccurrenceStart (occurrence))
        return group->getOccurrenceStart (occurrence);

    // Materialise from the members: this is the ONE moment a group built before
    // Phase 62 (or a project written before it) has its position inferred. From
    // here on the number is stored, so deleting the member that happens to be
    // first cannot move the appearance.
    double first = std::numeric_limits<double>::max();

    for (const auto& n : notes)
        if (n.groupId == groupId && n.groupOccurrence == occurrence)
            first = juce::jmin (first, n.startBeat);

    for (const auto& s : samples)
        if (s.groupId == groupId && s.groupOccurrence == occurrence)
            first = juce::jmin (first, s.startBeat);

    if (first == std::numeric_limits<double>::max())
        return 0.0;         // nothing to anchor; store nothing, invent nothing

    group->setOccurrenceStart (occurrence, first);
    return first;
}

void Layer::setOccurrenceStart (int groupId, int occurrence, double beat)
{
    if (auto* group = findGroup (groupId))
        group->setOccurrenceStart (occurrence, beat);
}

void Layer::pruneOccurrenceStarts (int groupId)
{
    for (auto& group : groups)
    {
        if (groupId >= 0 && group.getId() != groupId)
            continue;

        const auto live = getGroupOccurrences (group.getId());

        std::vector<int> stale;

        for (const auto& entry : group.getOccurrenceStarts())
            if (std::find (live.begin(), live.end(), entry.first) == live.end())
                stale.push_back (entry.first);

        for (int occurrence : stale)
            group.forgetOccurrenceStart (occurrence);
    }
}

void Layer::materialiseOccurrenceStarts()
{
    // Iterate by INDEX: occurrenceStart() writes into the group, and the list
    // of appearances has to be read before that happens.
    for (size_t i = 0; i < groups.size(); ++i)
    {
        const int groupId     = groups[i].getId();
        const auto occurrences = getGroupOccurrences (groupId);

        for (int occurrence : occurrences)
            occurrenceStart (groupId, occurrence);      // stores it the first time
    }
}

namespace
{
    /** Everything about a note that a sibling appearance has to copy. Position
        is included because the caller passes the already-anchored start. */
    bool sameNoteShape (const NoteEvent& a, const NoteEvent& b)
    {
        return a.pitch == b.pitch
                && std::abs (a.startBeat - b.startBeat) < 1.0e-9
                && std::abs (a.lengthBeats - b.lengthBeats) < 1.0e-9
                && std::abs (a.velocity - b.velocity) < 1.0e-6f
                && a.modTarget == b.modTarget
                && a.modPluginParam == b.modPluginParam
                && a.outputSlotOverride == b.outputSlotOverride
                && a.envelope.isEnabled() == b.envelope.isEnabled()
                && a.envelope.hasSameShapeAs (b.envelope);
    }

    bool sameClipShape (const SampleEvent& a, const SampleEvent& b)
    {
        return a.name == b.name
                && a.path == b.path
                && std::abs (a.startBeat - b.startBeat) < 1.0e-9
                && std::abs (a.lengthBeats - b.lengthBeats) < 1.0e-9
                && a.pitch == b.pitch
                && a.laneSpan == b.laneSpan
                && std::abs (a.gainDb - b.gainDb) < 1.0e-6f
                && a.outputSlotId == b.outputSlotId
                && a.reversed == b.reversed
                && a.stretchMode == b.stretchMode
                && a.pitchRangeSemitones == b.pitchRangeSemitones
                && std::abs (a.sourceOffsetSeconds - b.sourceOffsetSeconds) < 1.0e-9
                && a.automationTarget == b.automationTarget
                && a.audio == b.audio
                && a.volumeCurve.hasSameShapeAs (b.volumeCurve)
                && a.panCurve.hasSameShapeAs (b.panCurve);
    }

    /** One order for every appearance of a group: start, then pitch, then id.

        Two appearances of one group hold the same members, so the same
        comparator puts the twin at the same ORDINAL in each — and ordinal is
        all the pairing needs. Identical duplicates are interchangeable, so a
        tie never has to be broken meaningfully. */
    bool noteShapeLess (const NoteEvent& a, const NoteEvent& b)
    {
        if (std::abs (a.startBeat - b.startBeat) > 1.0e-9)
            return a.startBeat < b.startBeat;

        if (a.pitch != b.pitch)
            return a.pitch < b.pitch;

        return a.id < b.id;
    }

    bool clipShapeLess (const SampleEvent& a, const SampleEvent& b)
    {
        if (std::abs (a.startBeat - b.startBeat) > 1.0e-9)
            return a.startBeat < b.startBeat;

        // Phase 72: pitch is SHAPE, not identity — and this comparator is what
        // PAIRS a member with its twin, so it must not read a field the shape
        // edit itself can change. Dragging a block's bottom edge moves its
        // pitch; sorting on pitch then re-ordered the members, and the ordinal
        // pairing (which is all the pairing has) handed each twin the other's
        // name, audio and curves. Start + name identifies a member; identical
        // duplicates are interchangeable, so the id tie-break never has to be
        // meaningful.
        if (a.name != b.name)
            return a.name < b.name;

        return a.id < b.id;
    }
}

bool Layer::syncGroupShape (int groupId, int sourceOccurrence)
{
    if (findGroup (groupId) == nullptr)
        return false;

    const auto occurrences = getGroupOccurrences (groupId);

    if (occurrences.size() < 2)
        return false;               // one place is nothing to keep in step with

    const double sourceStart = occurrenceStart (groupId, sourceOccurrence);

    //-- The template: the source appearance, measured from its own origin ----
    // Copies, not pointers: adding members below reallocates the vectors these
    // would be pointing into.
    std::vector<NoteEvent>   shapeNotes;
    std::vector<SampleEvent> shapeClips;

    for (const auto& n : notes)
        if (n.groupId == groupId && n.groupOccurrence == sourceOccurrence)
            shapeNotes.push_back (n);

    for (const auto& s : samples)
        if (s.groupId == groupId && s.groupOccurrence == sourceOccurrence)
            shapeClips.push_back (s);

    if (shapeNotes.empty() && shapeClips.empty())
        return false;               // a wiped appearance is not a template

    // One order for both sides: start, then pitch, then id. Two appearances of
    // one group hold the same members, so the same comparator puts the twin at
    // the same ordinal in each — which is all the pairing needs.
    const auto noteLess = [] (const NoteEvent& a, const NoteEvent& b) { return noteShapeLess (a, b); };
    const auto clipLess = [] (const SampleEvent& a, const SampleEvent& b) { return clipShapeLess (a, b); };

    std::sort (shapeNotes.begin(), shapeNotes.end(), noteLess);
    std::sort (shapeClips.begin(), shapeClips.end(), clipLess);

    std::vector<double> noteRel;
    std::vector<double> clipRel;

    for (const auto& n : shapeNotes)
        noteRel.push_back (n.startBeat - sourceStart);

    for (const auto& s : shapeClips)
        clipRel.push_back (s.startBeat - sourceStart);

    bool changed = false;

    for (int occurrence : occurrences)
    {
        if (occurrence == sourceOccurrence)
            continue;

        // The sibling's OWN origin. This is the whole of "位置独立": the shape
        // arrives, the place stays.
        const double anchor = occurrenceStart (groupId, occurrence);

        auto existingNotes = getOccurrenceNoteIds (groupId, occurrence);
        auto existingClips = getOccurrenceSampleIds (groupId, occurrence);

        std::sort (existingNotes.begin(), existingNotes.end(),
                   [this, &noteLess] (int a, int b)
                   {
                       const auto* na = findNote (a);
                       const auto* nb = findNote (b);

                       return (na != nullptr && nb != nullptr) ? noteLess (*na, *nb) : a < b;
                   });

        std::sort (existingClips.begin(), existingClips.end(),
                   [this, &clipLess] (int a, int b)
                   {
                       const auto* sa = findSample (a);
                       const auto* sb = findSample (b);

                       return (sa != nullptr && sb != nullptr) ? clipLess (*sa, *sb) : a < b;
                   });

        //-- Notes -------------------------------------------------------------
        const int noteCount = juce::jmax ((int) shapeNotes.size(), (int) existingNotes.size());

        for (int i = 0; i < noteCount; ++i)
        {
            const bool fromTemplate = i < (int) shapeNotes.size();
            const bool fromSibling  = i < (int) existingNotes.size();

            if (! fromTemplate)
            {
                removeNote (existingNotes[(size_t) i]);
                changed = true;
                continue;
            }

            NoteEvent fresh = shapeNotes[(size_t) i];
            fresh.startBeat       = anchor + noteRel[(size_t) i];
            fresh.groupId         = groupId;
            fresh.groupOccurrence = occurrence;

            if (fromSibling)
            {
                auto* target = findNote (existingNotes[(size_t) i]);

                if (target == nullptr)
                    continue;

                fresh.id = target->id;

                if (! sameNoteShape (*target, fresh))
                {
                    *target = fresh;
                    changed = true;
                }
            }
            else
            {
                fresh.id = 0;

                if (addNote (fresh) > 0)
                    changed = true;
            }
        }

        //-- Blocks ------------------------------------------------------------
        const int clipCount = juce::jmax ((int) shapeClips.size(), (int) existingClips.size());

        for (int i = 0; i < clipCount; ++i)
        {
            const bool fromTemplate = i < (int) shapeClips.size();
            const bool fromSibling  = i < (int) existingClips.size();

            if (! fromTemplate)
            {
                removeSample (existingClips[(size_t) i]);
                changed = true;
                continue;
            }

            SampleEvent fresh = shapeClips[(size_t) i];
            fresh.startBeat       = anchor + clipRel[(size_t) i];
            fresh.groupId         = groupId;
            fresh.groupOccurrence = occurrence;

            if (fromSibling)
            {
                auto* target = findSample (existingClips[(size_t) i]);

                if (target == nullptr)
                    continue;

                const auto audio = fresh.audio;     // the buffer is shared, not owned
                fresh.id = target->id;

                if (! sameClipShape (*target, fresh))
                {
                    *target      = fresh;
                    target->audio = audio;
                    changed      = true;
                }
            }
            else
            {
                fresh.id = 0;

                if (addSample (fresh) > 0)
                    changed = true;
            }
        }
    }

    if (changed)
        pruneOccurrenceStarts (groupId);

    return changed;
}

void Layer::collectTwinMembers (const std::vector<int>& noteIds,
                                const std::vector<int>& clipIds,
                                std::vector<int>& twinNotes,
                                std::vector<int>& twinClips) const
{
    twinNotes.clear();
    twinClips.clear();

    // Group -> the ordinal each appearance stores its members at. Built once
    // per group, because a multi-select of forty notes asks forty times.
    struct Layout
    {
        std::vector<int> occurrences;
        std::vector<std::vector<int>> notes;    // per occurrence, in member order
        std::vector<std::vector<int>> clips;
    };

    std::map<int, Layout> layouts;

    const auto buildLayout = [this] (int groupId)
    {
        Layout layout;
        layout.occurrences = getGroupOccurrences (groupId);

        for (int occurrence : layout.occurrences)
        {
            auto noteList = getOccurrenceNoteIds (groupId, occurrence);
            std::sort (noteList.begin(), noteList.end(), [this] (int a, int b)
            {
                const auto* na = findNote (a);
                const auto* nb = findNote (b);

                return (na != nullptr && nb != nullptr) ? noteShapeLess (*na, *nb) : a < b;
            });
            layout.notes.push_back (std::move (noteList));

            auto clipList = getOccurrenceSampleIds (groupId, occurrence);
            std::sort (clipList.begin(), clipList.end(), [this] (int a, int b)
            {
                const auto* sa = findSample (a);
                const auto* sb = findSample (b);

                return (sa != nullptr && sb != nullptr) ? clipShapeLess (*sa, *sb) : a < b;
            });
            layout.clips.push_back (std::move (clipList));
        }

        return layout;
    };

    const auto twinsOf = [&] (int groupId, int occurrence, int ordinal, bool isNote)
    {
        auto found = layouts.find (groupId);

        if (found == layouts.end())
            found = layouts.emplace (groupId, buildLayout (groupId)).first;

        const auto& layout = found->second;

        if (layout.occurrences.size() < 2)
            return;

        const auto source = std::find (layout.occurrences.begin(), layout.occurrences.end(), occurrence);

        if (source == layout.occurrences.end())
            return;

        const auto sourceIndex = (size_t) std::distance (layout.occurrences.begin(), source);

        for (size_t i = 0; i < layout.occurrences.size(); ++i)
        {
            if (i == sourceIndex)
                continue;

            const auto& list = isNote ? layout.notes[i] : layout.clips[i];

            if ((size_t) ordinal >= list.size())
                continue;       // that appearance is already short; nothing to pair with

            (isNote ? twinNotes : twinClips).push_back (list[(size_t) ordinal]);
        }
    };

    for (int noteId : noteIds)
    {
        const auto* note = findNote (noteId);

        if (note == nullptr || note->groupId < 0)
            continue;

        const int groupId    = note->groupId;
        const int occurrence = note->groupOccurrence;

        auto found = layouts.find (groupId);

        if (found == layouts.end())
            found = layouts.emplace (groupId, buildLayout (groupId)).first;

        const auto& layout = found->second;
        const auto source  = std::find (layout.occurrences.begin(), layout.occurrences.end(), occurrence);

        if (source == layout.occurrences.end())
            continue;

        const auto& list = layout.notes[(size_t) std::distance (layout.occurrences.begin(), source)];
        const auto  it   = std::find (list.begin(), list.end(), noteId);

        if (it == list.end())
            continue;

        twinsOf (groupId, occurrence, (int) std::distance (list.begin(), it), true);
    }

    for (int clipId : clipIds)
    {
        const auto* clip = findSample (clipId);

        if (clip == nullptr || clip->groupId < 0)
            continue;

        const int groupId    = clip->groupId;
        const int occurrence = clip->groupOccurrence;

        auto found = layouts.find (groupId);

        if (found == layouts.end())
            found = layouts.emplace (groupId, buildLayout (groupId)).first;

        const auto& layout = found->second;
        const auto source  = std::find (layout.occurrences.begin(), layout.occurrences.end(), occurrence);

        if (source == layout.occurrences.end())
            continue;

        const auto& list = layout.clips[(size_t) std::distance (layout.occurrences.begin(), source)];
        const auto  it   = std::find (list.begin(), list.end(), clipId);

        if (it == list.end())
            continue;

        twinsOf (groupId, occurrence, (int) std::distance (list.begin(), it), false);
    }
}

std::vector<int> Layer::getGroupNoteIds (int groupId) const
{
    std::vector<int> ids;

    if (groupId < 0)
        return ids;

    for (const auto& n : notes)
        if (n.groupId == groupId)
            ids.push_back (n.id);

    return ids;
}

int Layer::countNotesInGroup (int groupId) const noexcept
{
    if (groupId < 0)
        return 0;

    int count = 0;

    for (const auto& n : notes)
        if (n.groupId == groupId)
            ++count;

    return count;
}

std::vector<int> Layer::getAllNoteIds() const
{
    std::vector<int> ids;
    ids.reserve (notes.size());

    for (const auto& n : notes)
        ids.push_back (n.id);

    return ids;
}

void Layer::restoreGroups (std::vector<NoteGroup> restoredGroups)
{
    groups = std::move (restoredGroups);

    // Same rule as layer ids: never hand out an id the file already used, or a
    // new group would inherit an automation track that belongs to the old one.
    for (const auto& g : groups)
        groupIdCounter() = juce::jmax (groupIdCounter(), g.getId());

    // A note that names a group which is not in the file is repaired to "no
    // group" rather than left dangling — the roll would otherwise draw a
    // label for nothing and route by a group nobody can see. Phase 58: the
    // occurrence goes with it, so a repaired note cannot claim appearance 3 of
    // a group that has none.
    for (auto& n : notes)
        if (n.groupId >= 0 && findGroup (n.groupId) == nullptr)
        {
            n.groupId         = -1;
            n.groupOccurrence = 0;
        }

    for (auto& s : samples)
        if (s.groupId >= 0 && findGroup (s.groupId) == nullptr)
        {
            s.groupId         = -1;
            s.groupOccurrence = 0;
        }
}

NoteEvent* Layer::findNote (int noteId) noexcept
{
    for (auto& n : notes)
        if (n.id == noteId)
            return &n;

    return nullptr;
}
const NoteEvent* Layer::findNote (int noteId) const noexcept
{
    for (const auto& n : notes)
        if (n.id == noteId)
            return &n;

    return nullptr;
}


int Layer::addNote (NoteEvent noteToAdd)
{
    noteToAdd.id = nextNoteId++;
    noteToAdd.startBeat = juce::jmax (0.0, noteToAdd.startBeat);
    noteToAdd.lengthBeats = juce::jmax (0.0, noteToAdd.lengthBeats);
    notes.push_back (noteToAdd);
    return noteToAdd.id;
}

void Layer::removeNote (int noteId)
{
    notes.erase (std::remove_if (notes.begin(), notes.end(),
                                 [noteId] (const NoteEvent& n) { return n.id == noteId; }),
                 notes.end());
}

void Layer::restoreNotes (std::vector<NoteEvent> restoredNotes)
{
    int highestId = 0;

    for (auto& n : restoredNotes)
    {
        n.pitch       = juce::jlimit (0, 127, n.pitch);
        n.startBeat   = juce::jmax (0.0, n.startBeat);
        n.lengthBeats = juce::jmax (0.0, n.lengthBeats);
        n.velocity    = juce::jlimit (0.0f, 1.0f, n.velocity);
        highestId     = juce::jmax (highestId, n.id);
    }

    notes = std::move (restoredNotes);
    nextNoteId = highestId + 1;
}

double Layer::getEndBeat() const noexcept
{
    double end = 0.0;

    for (const auto& n : notes)
        end = juce::jmax (end, n.endBeat());

    // Phase 17: a clip is content too. Leaving it out would let the transport
    // loop before the last sample finished, and the timeline would not grow
    // when a long clip is dragged past the end.
    for (const auto& s : samples)
        end = juce::jmax (end, s.endBeat());

    return end;
}

//==============================================================================
//  Phase 17: audio clips
//==============================================================================
SampleEvent* Layer::findSample (int sampleId) noexcept
{
    for (auto& s : samples)
        if (s.id == sampleId)
            return &s;

    return nullptr;
}

const SampleEvent* Layer::findSample (int sampleId) const noexcept
{
    return const_cast<Layer*> (this)->findSample (sampleId);
}

int Layer::addSample (SampleEvent sampleToAdd)
{
    sampleToAdd.id          = nextSampleId++;
    sampleToAdd.startBeat   = juce::jmax (0.0, sampleToAdd.startBeat);

    // Phase 70: no floor here beyond "not negative". The model used to clamp to
    // 0.25 beats (a 1/16 note), which since Phase 69 silently stretched every
    // short clip to 16× its length on the way through addSample — and this is
    // the entry point for pasting, cutting and the group-sync top-up, so a
    // 1/64 clip could come back as its own 1/16 sibling. How short a clip may
    // be drawn or dragged is an editing rule (minimumNoteLength()); the model
    // stores what it is told, exactly as restoreNotes does.
    sampleToAdd.lengthBeats = juce::jmax (0.0, sampleToAdd.lengthBeats);
    sampleToAdd.laneSpan    = juce::jlimit (1, SampleEvent::numPitchLanes, sampleToAdd.laneSpan);
    sampleToAdd.pitch       = juce::jlimit (0, 127 - sampleToAdd.laneSpan + 1, sampleToAdd.pitch);
    samples.push_back (std::move (sampleToAdd));
    return samples.back().id;
}

void Layer::removeSample (int sampleId)
{
    samples.erase (std::remove_if (samples.begin(), samples.end(),
                                   [sampleId] (const SampleEvent& s) { return s.id == sampleId; }),
                   samples.end());
}

void Layer::restoreSamples (std::vector<SampleEvent> restoredSamples)
{
    int highestId = 0;

    for (auto& s : restoredSamples)
    {
        s.startBeat   = juce::jmax (0.0, s.startBeat);
        s.lengthBeats = juce::jmax (0.0, s.lengthBeats);   // Phase 70: was 0.25
        s.laneSpan    = juce::jlimit (1, SampleEvent::numPitchLanes, s.laneSpan);
        s.pitch       = juce::jlimit (0, 127 - s.laneSpan + 1, s.pitch);
        s.gainDb      = juce::jlimit (-48.0f, 12.0f, s.gainDb);
        highestId     = juce::jmax (highestId, s.id);
    }

    samples    = std::move (restoredSamples);
    nextSampleId = highestId + 1;
}

//==============================================================================
int LayerStack::getNumLayers() const noexcept
{
    return (int) layers.size();
}

Layer* LayerStack::getLayer (int index) noexcept
{
    if (juce::isPositiveAndBelow (index, (int) layers.size()))
        return layers[(size_t) index].get();

    return nullptr;
}

const Layer* LayerStack::getLayer (int index) const noexcept
{
    if (juce::isPositiveAndBelow (index, (int) layers.size()))
        return layers[(size_t) index].get();

    return nullptr;
}

Layer* LayerStack::getActiveLayer() noexcept
{
    return getLayer (activeIndex);
}

const Layer* LayerStack::getActiveLayer() const noexcept
{
    return getLayer (activeIndex);
}

Layer* LayerStack::findLayerById (int layerId) noexcept
{
    if (layerId <= 0)
        return nullptr;

    for (auto& layer : layers)
        if (layer->getId() == layerId)
            return layer.get();

    return nullptr;
}

int LayerStack::indexOfLayerId (int layerId) const noexcept
{
    for (int i = 0; i < (int) layers.size(); ++i)
        if (layers[(size_t) i]->getId() == layerId)
            return i;

    return -1;
}

void LayerStack::setActiveLayerIndex (int newIndex)
{
    if (! juce::isPositiveAndBelow (newIndex, (int) layers.size()))
        return;

    if (newIndex == activeIndex)
        return;

    activeIndex = newIndex;
    notifyLayersChanged();
}

int LayerStack::addLayer (juce::String name, juce::Colour colour)
{
    // outputSlotId stays 0 ("unassigned") until the host assigns a real slot;
    // routing::layerSlotId() then falls back to the rack's first synth.
    layers.push_back (std::make_unique<Layer> (name, colour, 0));
    activeIndex = (int) layers.size() - 1;
    notifyLayersChanged();
    return activeIndex;
}

void LayerStack::removeLayer (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) layers.size()))
        return;

    // A stack with zero layers has no meaning for the roll; keep at least one.
    if (layers.size() <= 1)
        return;

    layers.erase (layers.begin() + index);

    // Phase 70: removing a layer ABOVE the active one slides the whole stack up,
    // so the active index has to move with it — clamping alone left the user
    // editing a different sheet than the one that was selected. Removing the
    // active layer itself keeps the index, which is the layer that took its
    // place (or the new last one, if it was at the end).
    if (index < activeIndex)
        --activeIndex;

    activeIndex = juce::jlimit (0, (int) layers.size() - 1, activeIndex);
    notifyLayersChanged();
}

void LayerStack::restoreLayers (std::vector<std::unique_ptr<Layer>> restoredLayers,
                               int activeLayerIndex)
{
    layers = std::move (restoredLayers);
    activeIndex = juce::jlimit (0, juce::jmax (0, (int) layers.size() - 1), activeLayerIndex);

    // Never hand out an id that the file already used.
    for (auto& layer : layers)
        layerIdCounter() = juce::jmax (layerIdCounter(), layer->getId());

    notifyLayersChanged();
}

void LayerStack::setLayerVisible (int index, bool shouldBeVisible)
{
    if (auto* layer = getLayer (index))
    {
        if (layer->isVisible() == shouldBeVisible)
            return;

        layer->setVisible (shouldBeVisible);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerOutputSlot (int index, int slotId)
{
    auto* layer = getLayer (index);

    if (layer == nullptr || layer->getOutputSlotId() == slotId)
        return;

    layer->setOutputSlotId (slotId);
    notifyLayersChanged();
}

void LayerStack::notifyNotesChanged()
{
    listeners.call ([] (Listener& l) { l.notesChanged(); });
}

//==============================================================================
void LayerStack::setLayerLevelDb (int index, float db)
{
    if (auto* layer = getLayer (index))
    {
        if (juce::approximatelyEqual (layer->getLevelDb(), juce::jlimit (-48.0f, 6.0f, db)))
            return;

        layer->setLevelDb (db);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerPan (int index, float pan)
{
    if (auto* layer = getLayer (index))
    {
        const float clamped = juce::jlimit (-1.0f, 1.0f, pan);

        if (juce::approximatelyEqual (layer->getPan(), clamped))
            return;

        layer->setPan (clamped);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerMuted (int index, bool shouldBeMuted)
{
    if (auto* layer = getLayer (index))
    {
        if (layer->isMuted() == shouldBeMuted)
            return;

        layer->setMuted (shouldBeMuted);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerSoloed (int index, bool shouldBeSoloed)
{
    if (auto* layer = getLayer (index))
    {
        if (layer->isSoloed() == shouldBeSoloed)
            return;

        layer->setSoloed (shouldBeSoloed);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerAutomationTarget (int index, LayerAutomationTarget target)
{
    if (auto* layer = getLayer (index))
    {
        if (layer->getAutomationTarget() == target)
            return;

        layer->setAutomationTarget (target);
        notifyLayersChanged();
    }
}

void LayerStack::setLayerAutomationExpanded (int index, bool expanded)
{
    if (auto* layer = getLayer (index))
    {
        if (layer->isAutomationExpanded() == expanded)
            return;

        layer->setAutomationExpanded (expanded);
        notifyLayersChanged();
    }
}

void LayerStack::touchLayers()
{
    notifyLayersChanged();
}

void LayerStack::notifyLayersChanged()
{
    listeners.call ([] (Listener& l) { l.layersChanged(); });
}

} // namespace atomic

#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Model/Quantize.h"
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
//  Selection
//==============================================================================
bool PianoRollComponent::isSelected (int noteId) const
{
    return std::find (selection.begin(), selection.end(), noteId) != selection.end();
}


void PianoRollComponent::setSelection (std::vector<int> newSelection)
{
    selection = std::move (newSelection);

    // Phase 28: one selection at a time. See setSampleSelection for why.
    if (! selection.empty())
        sampleSelection.clear();

    notifySelectionChanged();
    repaint();
}


void PianoRollComponent::setSampleSelection (std::vector<int> ids)
{
    // Drop ids that no longer exist: a delete, an undo, or a layer switch can
    // all leave the list pointing at nothing, and a selection of ghosts makes
    // Delete look broken.
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        ids.clear();
    else
        ids.erase (std::remove_if (ids.begin(), ids.end(),
                                   [layer] (int id) { return layer->findSample (id) == nullptr; }),
                   ids.end());

    sampleSelection = std::move (ids);

    // A block and a note cannot both be "the selection": Delete, Ctrl+C and the
    // next drag would each have to guess. Clicking a block therefore lets go of
    // the notes, and clicking a note lets go of the blocks.
    if (! sampleSelection.empty() && ! selection.empty())
        selection.clear();

    // Phase 32: always tell the host, not only when notes were dropped. The
    // rack lights the sampler a selected block is mounted on, and that answer
    // changes on every click that lands on a block.
    notifySelectionChanged();
    repaint();
}


void PianoRollComponent::setMixedSelection (std::vector<int> noteIds, std::vector<int> sampleIds)
{
    auto* layer = stack.getActiveLayer();

    // Same hygiene as setSampleSelection: an id that no longer resolves makes
    // Delete look broken, so it is dropped at the door.
    if (layer == nullptr)
    {
        noteIds.clear();
        sampleIds.clear();
    }
    else
    {
        sampleIds.erase (std::remove_if (sampleIds.begin(), sampleIds.end(),
                                         [layer] (int id) { return layer->findSample (id) == nullptr; }),
                         sampleIds.end());
        noteIds.erase (std::remove_if (noteIds.begin(), noteIds.end(),
                                       [layer] (int id) { return layer->findNote (id) == nullptr; }),
                       noteIds.end());
    }

    selection       = std::move (noteIds);
    sampleSelection = std::move (sampleIds);

    notifySelectionChanged();
    repaint();
}


bool PianoRollComponent::isSampleSelected (int sampleId) const
{
    return std::find (sampleSelection.begin(), sampleSelection.end(), sampleId)
             != sampleSelection.end();
}


void PianoRollComponent::toggleSampleSelection (int sampleId)
{
    if (const auto found = std::find (sampleSelection.begin(), sampleSelection.end(), sampleId);
        found != sampleSelection.end())
    {
        sampleSelection.erase (found);
    }
    else
    {
        sampleSelection.push_back (sampleId);
    }

    if (! sampleSelection.empty() && ! selection.empty())
    {
        selection.clear();
        notifySelectionChanged();
    }

    repaint();
}


void PianoRollComponent::notifySelectionChanged()
{
    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}


int PianoRollComponent::getSelectedRoutingSlotId() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return -1;

    int slotId = -1;
    bool first = true;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr)
            continue;

        const auto resolution = routing::resolve (*layer, *note, rack);

        if (! resolution.isValid())
            return -1;

        if (first)
        {
            slotId = resolution.slotId;
            first = false;
        }
        else if (slotId != resolution.slotId)
        {
            return -1;      // the selection is split: nothing single to highlight
        }
    }

    return slotId;
}


bool PianoRollComponent::selectionUsesOverride() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    for (int id : selection)
        if (const auto* note = layer->findNote (id))
            if (note->outputSlotOverride >= 0)
                return true;

    return false;
}


void PianoRollComponent::routeSelectionTo (int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return;

    // Phase 27: see mountSelectionTo — a sampler cannot play a note, so it is
    // not a destination for one.
    if (slotId >= 0)
        if (const auto* slot = rack.findSlot (slotId); slot != nullptr && slot->isSampler())
            return;

    for (int id : selection)
        if (auto* note = layer->findNote (id))
            note->outputSlotOverride = slotId < 0 ? -1 : slotId;

    stack.notifyNotesChanged();
    notifySelectionChanged();
    repaint();
}


bool PianoRollComponent::mountSelectionTo (int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return false;

    // Phase 27: clicking a sampler card while notes are selected used to mean
    // "route these into that slot", which is now a silent mute — a sampler holds
    // a file and has no oscillator. Refusing outright is better than doing it
    // and leaving the user to work out why the notes stopped.
    if (slotId >= 0)
        if (const auto* slot = rack.findSlot (slotId); slot != nullptr && slot->isSampler())
            return false;

    const int groupId = getSelectedGroupId();

    // Phase 24: a group action only when the WHOLE group is selected.
    //
    // Phase 23 mounted the group as soon as the selection happened to lie
    // inside one, which made "this note should use the other synth" impossible:
    // clicking a single note and then a synth card re-mounted all twelve. The
    // note-level override is the tier that means "just this one", and it can
    // only reach the router if we do not overwrite the group instead.
    if (groupId < 0 || ! isWholeGroupSelected (groupId))
    {
        // A partial (or loose) selection: the per-note override is the honest
        // answer — one exception should stay one exception.
        routeSelectionTo (slotId);
        return true;
    }

    // A group. Mount the group itself, then clear the members' individual
    // overrides: an override outranks the group in routing::resolve(), so
    // leaving one behind would make the group's chip claim a synth the note is
    // not actually playing on.
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* target = stack.getLayer (i);

        if (target == nullptr)
            continue;

        if (auto* group = target->findGroup (groupId))
        {
            group->setOutputSlotId (slotId < 0 ? -1 : slotId);

            for (const auto& note : target->getNotes())
                if (note.groupId == groupId)
                    if (auto* mutableNote = target->findNote (note.id))
                        mutableNote->outputSlotOverride = -1;

            stack.notifyNotesChanged();
            notifySelectionChanged();
            repaint();
            return true;
        }
    }

    return false;
}


//==============================================================================
//  Phase 24: which hosted parameter a curve can drive
//==============================================================================
int PianoRollComponent::resolvedSlotForNote (const NoteEvent& note) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    return routing::resolve (*layer, note, rack).slotId;
}


int PianoRollComponent::resolvedSlotForGroup (int layerIndex, int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return -1;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return -1;

    // Two tiers, exactly like routing::resolve() one level down: the group's
    // own mount when it has a live one, otherwise the layer default.
    if (! group->followsLayerOutput() && rack.findSlot (group->getOutputSlotId()) != nullptr)
        return group->getOutputSlotId();

    return routing::layerSlotId (*layer, rack);
}


std::vector<PluginParamInfo> PianoRollComponent::pluginParamsForSlot (int slotId) const
{
    if (listPluginParams == nullptr || slotId < 0)
        return {};

    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr || ! slot->isPlugin())
        return {};      // an internal synth has three targets, and they are built in

    return listPluginParams (slotId);
}


juce::String PianoRollComponent::pluginParamName (int slotId, int paramIndex) const
{
    if (paramIndex < 0)
        return {};

    for (const auto& info : pluginParamsForSlot (slotId))
        if (info.index == paramIndex)
            return info.name;

    return {};
}


juce::String PianoRollComponent::noteTargetLabel (const NoteEvent& note) const
{
    // Phase 82: 空 is a destination too. Naming it matters twice over — an
    // empty cell beside a caret reads as a rendering bug, and a cell that said
    // "音量" while the curve drove nothing would be a lie about the only thing
    // this cell exists to say.
    if (note.curveTarget() < 0)
        return utf8 ("空");

    if (! note.drivesPluginParam())
        return modTargetLabel (note.modTarget());

    const auto name = pluginParamName (resolvedSlotForNote (note), note.modPluginParam());

    // The plugin is gone, or its parameter list changed under the project. The
    // curve is still there and still says what it says; it just cannot be
    // named. Saying so beats an empty cell.
    return name.isNotEmpty() ? name : utf8 ("音源参数");
}


juce::String PianoRollComponent::groupTargetLabel (const NoteGroup& group) const
{
    const int target = group.curveTarget();      // Phase 82: read, never cached

    if (target < 0)
        return utf8 ("空");

    if (target != (int) GroupAutomationTarget::plugin)
        return target == (int) GroupAutomationTarget::pan ? utf8 ("组声相") : utf8 ("组音量");

    const auto name = pluginParamName (resolvedSlotForGroup (expandedGroupLayerIndex, group.getId()),
                                       group.getPluginParam());

    return name.isNotEmpty() ? name : utf8 ("音源参数");
}


bool PianoRollComponent::addPluginParamSubmenu (juce::PopupMenu& menu, int slotId,
                                                int currentIndex, int firstItemId)
{
    const auto params = pluginParamsForSlot (slotId);

    if (params.empty())
        return false;

    juce::PopupMenu sub;

    // A hosted VST3 can expose two thousand parameters (JUCE's host-side
    // wrapper lists a block of them for MIDI-style control on top of whatever
    // the vendor wrote). A menu that long is unusable and takes a visible
    // moment to lay out, so the list is capped: the first 64 are the ones a
    // vendor parameter list starts with, and the rest stay reachable through
    // the plugin's own editor window.
    const int listed = juce::jmin ((int) params.size(), kMaxListedPluginParams);

    for (int i = 0; i < listed; ++i)
        sub.addItem (firstItemId + i, params[(size_t) i].name,
                     true, params[(size_t) i].index == currentIndex);

    if ((int) params.size() > listed)
        sub.addItem (-1, utf8 ("另有 ") + juce::String ((int) params.size() - listed)
                            + utf8 (" 个参数 · 用音源自己的窗口"), false, false);

    menu.addSubMenu (utf8 ("音源参数") + "  ·  " + rack.getSlotName (slotId), sub, true);
    return true;
}


void PianoRollComponent::toggleSelection (int noteId)
{    if (isSelected (noteId))
        selection.erase (std::remove (selection.begin(), selection.end(), noteId), selection.end());
    else
        selection.push_back (noteId);

    // Phase 28: touching a note lets go of the clips — see setSampleSelection.
    sampleSelection.clear();

    notifySelectionChanged();
    repaint();
}


//==============================================================================
//  Phase 16: note groups
//==============================================================================
int PianoRollComponent::groupSelection()
{
    auto* layer = stack.getActiveLayer();

    // Phase 35: a group is "the members", not "the notes". A selection holding
    // only sample blocks used to do nothing here — G pressed and no group
    // appeared, because the button was asking a question about notes.
    if (layer == nullptr || (selection.empty() && sampleSelection.empty()))
        return -1;

    // The name/colour cycle follows the number of groups that already exist, so
    // a session reads A, B, C… even after a few have been dissolved.
    const int ordinal = (int) layer->getGroups().size();
    const int groupId = layer->addGroup (groupPalette::defaultName (ordinal),
                                          groupPalette::nextGroupColour (ordinal));

    layer->assignNotesToGroup (selection, groupId);

    // Both kinds, one id. The second line is the whole of "采样建组": membership
    // already lived on the clip (Phase 27 saved it, Phase 27's menu set it), the
    // only thing missing was a way to say it without finding an existing group.
    layer->assignSamplesToGroup (sampleSelection, groupId);

    // Phase 62: pin where this appearance sits NOW, while "the earliest member"
    // still means what it says. Deriving it later would let the first deleted
    // note take the whole appearance with it.
    layer->occurrenceStart (groupId, 0);

    // Re-grouping may have emptied the group these notes came from. Prune it
    // here as well as on delete: an empty group is a row in the automation
    // window that nobody can find in the roll.
    pruneEmptyGroups();

    // One notification for the whole gesture: the engine republishes once, the
    // undo history takes one step, and no view sees a half-built group.
    stack.notifyNotesChanged();
    repaint();
    return groupId;
}


int PianoRollComponent::getSelectedGroupId() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    // Phase 35: samples are members too, so a selection of blocks can be "the
    // selection's group" just as well as a selection of notes. A MIXED selection
    // has to answer with the group both halves belong to; if one half is loose
    // the honest answer is -1, because "group this" would then mean two things.
    int groupId = -1;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr)
            continue;

        if (note->groupId < 0)
            return -1;

        if (groupId < 0)
            groupId = note->groupId;
        else if (groupId != note->groupId)
            return -1;
    }

    for (int id : sampleSelection)
    {
        const int clipGroup = layer->getGroupIdOfSample (id);

        if (clipGroup < 0)
            return -1;

        if (groupId < 0)
            groupId = clipGroup;
        else if (groupId != clipGroup)
            return -1;
    }

    return groupId;
}


int PianoRollComponent::getSelectedGroupOccurrence() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    // Phase 58: the selection also answers "WHICH copy", because copying a
    // group has to copy the copy the user selected — not all four of them. A
    // selection that straddles two appearances is not one object, so it answers
    // -1 and the copy falls back to the loose-notes rule.
    int occurrence = -1;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr || note->groupId < 0)
            return -1;

        if (occurrence < 0)
            occurrence = note->groupOccurrence;
        else if (occurrence != note->groupOccurrence)
            return -1;
    }

    for (int id : sampleSelection)
    {
        const auto* clip = layer->findSample (id);

        if (clip == nullptr || clip->groupId < 0)
            return -1;

        if (occurrence < 0)
            occurrence = clip->groupOccurrence;
        else if (occurrence != clip->groupOccurrence)
            return -1;
    }

    return occurrence;
}


bool PianoRollComponent::isWholeGroupSelected (int groupId, bool includeSamples,
                                               int occurrence) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0)
        return false;

    // Every member has to be in the selection, and nothing else may be — a
    // selection that also holds a loose note is two actions, not one.
    //
    // Phase 58: "every member" means every member of ONE appearance when
    // `occurrence` is given. Selecting the chorus copy alone is a whole object,
    // even though the group it belongs to has three more copies elsewhere.
    const int notes   = occurrence < 0 ? layer->countNotesInGroup (groupId)
                                       : layer->countNotesInOccurrence (groupId, occurrence);
    const int clips   = ! includeSamples ? 0
                        : (occurrence < 0 ? layer->countSamplesInGroup (groupId)
                                          : layer->countSamplesInOccurrence (groupId, occurrence));
    const int members = notes + clips;
    const int chosen  = (int) selection.size() + (includeSamples ? (int) sampleSelection.size() : 0);

    if (selection.empty() && (! includeSamples || sampleSelection.empty()))
        return false;

    if (members <= 0 || chosen != members)
        return false;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr || note->groupId != groupId)
            return false;

        if (occurrence >= 0 && note->groupOccurrence != occurrence)
            return false;
    }

    if (includeSamples)
        for (int id : sampleSelection)
        {
            const auto* clip = layer->findSample (id);

            if (clip == nullptr || clip->groupId != groupId)
                return false;

            if (occurrence >= 0 && clip->groupOccurrence != occurrence)
                return false;
        }

    return true;
}


std::vector<std::pair<int, int>> PianoRollComponent::getSelectedGroups() const
{
    std::vector<std::pair<int, int>> groups;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return groups;

    // The selection only ever holds ids of the ACTIVE layer (switching sheets
    // clears it), so this is one layer's worth of groups, not the session's.
    for (const auto& group : layer->getGroups())
    {
        for (int occurrence : layer->getGroupOccurrences (group.getId()))
        {
            // Blocks count: the user selects a group by clicking its label or by
            // dragging it, and a group of samples has no notes to select.
            //
            // Phase 63: per appearance. Asking `isWholeGroupSelected` here made
            // "select two groups" light up NEITHER row in the group panel — the
            // same whole-selection-vs-one-appearance confusion as P63-A.
            if (isWholeOccurrenceSelected (group.getId(), occurrence))
            {
                groups.push_back ({ stack.getActiveLayerIndex(), group.getId() });
                break;
            }
        }
    }

    return groups;
}


void PianoRollComponent::selectGroup (int groupId, int occurrence)
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        if (layer->findGroup (groupId) == nullptr)
            continue;

        // A group belongs to exactly one layer, and only the active layer is
        // clickable, so selecting it also means bringing that sheet forward.
        if (i != stack.getActiveLayerIndex())
            stack.setActiveLayerIndex (i);

        // Phase 35: BOTH kinds, in one write.
        //
        // "Select the whole group" has to mean the whole group. Setting only the
        // notes left the group's blocks dark while the bracket said they were
        // in it, and — worse — the next Delete then removed half of what the
        // highlight appeared to cover. setMixedSelection exists for exactly this
        // (Phase 29 built it for mixed copy), and it is the only way to write
        // both lists without one clearing the other.
        // Phase 58: with an occurrence, only that copy; without one, the whole
        // group (every copy) — which is what a caller holding just an id means.
        if (occurrence < 0)
            setMixedSelection (layer->getGroupNoteIds (groupId),
                              layer->getGroupSampleIds (groupId));
        else
            setMixedSelection (layer->getOccurrenceNoteIds (groupId, occurrence),
                              layer->getOccurrenceSampleIds (groupId, occurrence));

        return;
    }
}


void PianoRollComponent::addOccurrenceToSelection (int groupId, int occurrence)
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0)
        return;

    auto notes = selection;
    auto clips = sampleSelection;

    // Appended, not merged by group: the existing selection keeps its order (it
    // is what the panels list) and only gains what it did not already hold.
    for (int id : layer->getOccurrenceNoteIds (groupId, occurrence))
        if (std::find (notes.begin(), notes.end(), id) == notes.end())
            notes.push_back (id);

    for (int id : layer->getOccurrenceSampleIds (groupId, occurrence))
        if (std::find (clips.begin(), clips.end(), id) == clips.end())
            clips.push_back (id);

    setMixedSelection (std::move (notes), std::move (clips));
}


void PianoRollComponent::expandToWholeOccurrences (std::vector<int>& noteIds,
                                                   std::vector<int>& sampleIds) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    // Collected from a SNAPSHOT of the hits, then widened: reading the two
    // vectors while appending to them would walk the members just added.
    std::vector<std::pair<int, int>> occurrences;

    const auto collect = [&] (int groupId, int occurrence)
    {
        // An OPEN group is a container, not an object: inside it the member is
        // exactly what the user aimed at, and widening would take the gesture
        // back off them.
        if (! isGroupLocked (groupId))
            return;

        const auto entry = std::make_pair (groupId, occurrence);

        if (std::find (occurrences.begin(), occurrences.end(), entry) == occurrences.end())
            occurrences.push_back (entry);
    };

    for (int id : noteIds)
        if (const auto* n = layer->findNote (id))
            collect (n->groupId, n->groupOccurrence);

    for (int id : sampleIds)
        if (const auto* c = layer->findSample (id))
            collect (c->groupId, c->groupOccurrence);

    for (const auto& entry : occurrences)
    {
        for (int id : layer->getOccurrenceNoteIds (entry.first, entry.second))
            if (std::find (noteIds.begin(), noteIds.end(), id) == noteIds.end())
                noteIds.push_back (id);

        for (int id : layer->getOccurrenceSampleIds (entry.first, entry.second))
            if (std::find (sampleIds.begin(), sampleIds.end(), id) == sampleIds.end())
                sampleIds.push_back (id);
    }
}


bool PianoRollComponent::ungroupSelection()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || (selection.empty() && sampleSelection.empty()))
        return false;

    bool changed = false;

    // Phase 62: taking a member out of the shape takes its twin out too, or the
    // next edit would read the difference as "this member was deleted" and
    // remove it from the other appearance behind the user's back.
    std::vector<int> twinNotes, twinClips;
    collectTwinsForEdit (twinNotes, twinClips);

    const auto detach = [layer, &changed] (int id, bool isNote)
    {
        if (isNote)
        {
            if (auto* note = layer->findNote (id); note != nullptr && note->groupId >= 0)
            {
                note->groupId         = -1;
                note->groupOccurrence = 0;      // Phase 58: leave no stale appearance
                changed = true;
            }
        }
        else if (auto* clip = layer->findSample (id); clip != nullptr && clip->groupId >= 0)
        {
            clip->groupId         = -1;
            clip->groupOccurrence = 0;
            changed = true;
        }
    };

    for (int id : selection)
        detach (id, true);

    for (int id : twinNotes)
        detach (id, true);

    // Phase 35: Shift+G on a mixed selection has to take the blocks out too, or
    // the group would still exist — held open by members the user just asked to
    // leave, and still counted in its bracket.
    for (int id : sampleSelection)
        detach (id, false);

    for (int id : twinClips)
        detach (id, false);

    if (changed)
    {
        pruneEmptyGroups();
        stack.notifyNotesChanged();
        repaint();
    }

    return changed;
}


bool PianoRollComponent::disbandGroup (int layerIndex, int groupId, int occurrence)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || layer->findGroup (groupId) == nullptr)
        return false;

    //-- Phase 58: one copy, or the whole definition? ------------------------
    // With several appearances on the sheet, "解散编组" on the chorus copy must
    // not take the verse copy's group away with it — the user pointed at one
    // object. So a single occurrence is DETACHED, and the definition is only
    // dropped when this was the last thing holding it (or when no occurrence
    // was named at all, which is the old whole-group verb).
    if (occurrence >= 0 && layer->getGroupOccurrences (groupId).size() > 1)
    {
        const auto noteIds = layer->getOccurrenceNoteIds (groupId, occurrence);
        const auto clipIds = layer->getOccurrenceSampleIds (groupId, occurrence);

        layer->assignNotesToGroup (noteIds, -1);
        layer->assignSamplesToGroup (clipIds, -1);

        stack.notifyNotesChanged();
        repaint();
        return true;
    }

    // Tell the host FIRST: it owns the automation window and has to drop the
    // group's track before the id stops resolving. Doing it afterwards leaves a
    // row pointing at nothing for one repaint.
    if (onGroupRemoved != nullptr)
        onGroupRemoved (groupId);

    layer->removeGroup (groupId);

    // Notes the group used to hold are still selected; that is deliberate —
    // dissolving a group should not also lose your place.
    stack.notifyNotesChanged();
    repaint();
    return true;
}


bool PianoRollComponent::setGroupOutputForSelection (int slotId)
{
    const int groupId = getSelectedGroupId();

    if (groupId < 0)
        return false;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        if (auto* group = layer->findGroup (groupId))
        {
            group->setOutputSlotId (slotId < 0 ? -1 : slotId);
            stack.notifyNotesChanged();
            repaint();
            return true;
        }
    }

    return false;
}


int PianoRollComponent::getGroupLabelAt (juce::Point<float> position, int* occurrence) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& group : layer->getGroups())
        {
            // Phase 58: every appearance has its own pill, and the one the
            // pointer is over is the one the click is about.
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                // Phase 21: the pill now ends in a handle, and that handle is
                // its own hit target. Without this exclusion clicking it would
                // select the group instead of opening the curve — which is
                // exactly the bug the note's tail button already taught us to
                // avoid.
                if (getGroupAutomationHandleBounds (i, group.getId(), which).contains (position))
                    continue;

                if (getGroupLabelBounds (i, group.getId(), which).contains (position))
                {
                    if (occurrence != nullptr)
                        *occurrence = which;

                    return group.getId();
                }
            }
        }
    }

    return -1;
}


int PianoRollComponent::getGroupFrameAt (juce::Point<float> position, int* occurrence) const
{
    // Only the active layer: a ghost group is not editable, and a click on one
    // that picked the group up would drag notes the user cannot even select.
    const int  activeIndex = stack.getActiveLayerIndex();
    const auto* layer      = stack.getLayer (activeIndex);

    if (layer == nullptr)
        return -1;

    for (const auto& group : layer->getGroups())
    {
        // Phase 58: one box per appearance. The pointer is inside at most one of
        // them, so "which group" is no longer a good enough answer — the caller
        // also needs "which copy of it".
        for (int which : layer->getGroupOccurrences (group.getId()))
        {
            // Phase 57: the same box the frame is drawn with (groupFrameOf), so
            // the pointer agrees with the line — the old 3px expansion would
            // have the group grabbed from outside its own border.
            const auto content = groupFrameOf (activeIndex, group.getId(), which);

            if (content.isEmpty())
                continue;

            if (content.contains (position))
            {
                if (occurrence != nullptr)
                    *occurrence = which;

                return group.getId();
            }
        }
    }

    return -1;
}


bool PianoRollComponent::isGroupFocused (int layerIndex, int groupId, int occurrence) const
{
    if (focusSlotId < 0)
        return false;

    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId
             && (occurrence < 0 || note.groupOccurrence == occurrence)
             && routing::resolve (*layer, note, rack).slotId == focusSlotId)
            return true;

    return false;
}


juce::Rectangle<float> PianoRollComponent::getGroupContentBounds (const Layer& layer,
                                                                 const NoteGroup& group,
                                                                 int occurrence) const
{
    const auto area = getGridArea();
    juce::Rectangle<float> unionBounds;

    // Phase 58: -1 means "the whole group" (every appearance at once), which is
    // what a caller that only knows the group id wants; anything else picks ONE
    // appearance out, and those are the rectangles the user sees.
    const auto belongs = [&group, occurrence] (int groupId, int memberOccurrence)
    {
        return groupId == group.getId() && (occurrence < 0 || memberOccurrence == occurrence);
    };

    for (const auto& note : layer.getNotes())
    {
        if (! belongs (note.groupId, note.groupOccurrence))
            continue;

        const auto bounds = getNoteBounds (note);

        if (! bounds.intersects (area))
            continue;

        unionBounds = unionBounds.isEmpty() ? bounds : unionBounds.getUnion (bounds);
    }

    // Phase 27: clips are members too, so the bracket has to reach them.
    // Otherwise "group the clip with these notes" would draw a frame around
    // the notes with the clip sticking out of it, and dragging that frame would
    // leave the clip behind.
    for (const auto& sample : layer.getSamples())
    {
        if (! belongs (sample.groupId, sample.groupOccurrence))
            continue;

        const auto bounds = getSampleScreenBounds (layerIndexOf (&layer), sample.id);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        unionBounds = unionBounds.isEmpty() ? bounds : unionBounds.getUnion (bounds);
    }

    return unionBounds;
}


int PianoRollComponent::layerIndexOf (const Layer* layer) const
{
    if (layer == nullptr)
        return -1;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (stack.getLayer (i) == layer)
            return i;

    return -1;
}


juce::Rectangle<float> PianoRollComponent::getGroupLabelBounds (int layerIndex, int groupId,
                                                              int occurrence) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return {};

    // Phase 58: a pill per appearance. Two copies of "副歌" on the sheet are two
    // objects with two grab areas, not one label floating over both.
    const auto content = getGroupContentBounds (*layer, *group, occurrence);

    if (content.isEmpty())
        return {};

    const auto area = getGridArea();
    const float height = ui::pf (14);

    const juce::String text = groupLabelText (*layer, *group);

    juce::Font font (juce::FontOptions (atomic::ui::sp (9.5f)));

    // Measured with GlyphArrangement rather than Font::getStringWidthFloat —
    // that one is deprecated in JUCE 8 (it cannot account for the fallback that
    // draws CJK) and this project builds warning-free.
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);

    // Phase 66: the pill may not be wider than the group it names. A group
    // 0.2 beats long is about 8 px at the default zoom, while the minimum pill
    // is 34 px — the name tag then hangs out over whatever sits to the right,
    // and which group it belongs to stops being readable. Clamped to the
    // group's own width the tag is exactly as wide as the bracket under it;
    // `drawText` with useEllipses = false trims the text for us.
    const float width = juce::jmin (juce::jlimit (ui::pf (34), ui::pf (168),
                                                  glyphs.getBoundingBox (0, -1, true).getWidth()
                                                    + ui::pf (16) + ui::pf (13)),
                                    content.getWidth());

    // Above the topmost member, and inside the grid when the group sits against
    // the very top row — a label half off the canvas would be unclickable.
    float labelY = content.getY() - height - ui::pf (2);

    if (labelY < area.getY())
        labelY = juce::jmin (area.getBottom() - height, content.getY() + ui::pf (2));

    float labelX = content.getX();

    if (labelX + width > area.getRight())
        labelX = juce::jmax (area.getX(), area.getRight() - width);

    return { labelX, labelY, width, height };
}


juce::Rectangle<float> PianoRollComponent::getGroupAutomationHandleBounds (int layerIndex,
                                                                          int groupId,
                                                                          int occurrence) const
{
    const auto pill = getGroupLabelBounds (layerIndex, groupId, occurrence);

    if (pill.isEmpty())
        return {};

    // The right end of the pill, inset so the pill's rounded corner still
    // reads: a handle flush with the edge looks like a clipped button.
    const float w = ui::pf (12);

    // Phase 66: a pill narrower than the group now can be narrower than this
    // handle too — placing it anyway puts a 12 px button outside the tag it
    // belongs to. Below that width there is no handle: the tag is a name, and
    // a group that narrow is not a place where two controls fit anyway.
    if (pill.getWidth() < w + ui::pf (6))
    {
        return {};
    }
    return { pill.getRight() - w - ui::pf (1.5f), pill.getY() + ui::pf (1.5f),
             w, juce::jmax (ui::pf (8), pill.getHeight() - ui::pf (3)) };
}


int PianoRollComponent::getFocusedNoteCount() const
{
    if (focusSlotId < 0)
        return 0;

    int count = 0;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
            if (routing::resolve (*layer, note, rack).slotId == focusSlotId)
                ++count;
    }

    return count;
}


void PianoRollComponent::setFocusSlotId (int slotId)
{
    if (focusSlotId == slotId)
        return;

    focusSlotId = slotId;
    repaint();
}


bool PianoRollComponent::dismissSpotlight()
{
    if (focusSlotId < 0)
        return false;

    // The host owns the rack's own highlight indicator, so it gets told first;
    // without a host (or if it declines) the view still has to recover, because
    // a mode the user cannot leave is a bug no matter who else is asleep.
    if (onClearSpotlight != nullptr)
        onClearSpotlight();

    setFocusSlotId (-1);
    return true;
}


int PianoRollComponent::layerIndexOfGroup (int groupId) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i);
            layer != nullptr && layer->findGroup (groupId) != nullptr)
            return i;

    return -1;
}


void PianoRollComponent::pruneEmptyGroups()
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr || layer->getGroups().empty())
            continue;

        // Collected first: removeGroup() rewrites the vector this loop reads.
        std::vector<int> empty;

        for (const auto& group : layer->getGroups())
            if (layer->countNotesInGroup (group.getId()) == 0
                 && layer->countSamplesInGroup (group.getId()) == 0)   // Phase 27: clips count
                empty.push_back (group.getId());

        for (int groupId : empty)
        {
            if (onGroupRemoved != nullptr)
                onGroupRemoved (groupId);

            layer->removeGroup (groupId);
        }

        // Phase 62: an appearance that just lost its last member must not leave
        // its origin behind — the next appearance to take that number would
        // inherit a position that was never its own.
        layer->pruneOccurrenceStarts();
    }
}


void PianoRollComponent::fillGroupMenu (juce::PopupMenu& menu, int layerIndex, int groupId,
                                        int occurrence) const
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return;

    // Phase 58: the counts belong to the copy under the pointer. With one
    // appearance they are the group's counts, exactly as before; with several,
    // "12 音符" for a 3-note copy would be a lie about what the verbs below do.
    const bool perCopy = occurrence >= 0;
    const int  members = perCopy ? layer->countNotesInOccurrence (groupId, occurrence)
                                 : layer->countNotesInGroup (groupId);
    const int  clips   = perCopy ? layer->countSamplesInOccurrence (groupId, occurrence)
                                 : layer->countSamplesInGroup (groupId);   // Phase 27

    const int copies = (int) layer->getGroupOccurrences (groupId).size();

    menu.addSectionHeader (group->getName() + utf8 (" · ")
                             + juce::String (members) + utf8 (" 音符")
                             + (clips > 0 ? utf8 (" · ") + juce::String (clips) + utf8 (" 采样")
                                          : juce::String())
                             + (copies > 1 && perCopy
                                  ? utf8 (" · 第 ") + juce::String (occurrence + 1) + utf8 (" 处")
                                  : juce::String()));
    menu.addItem (1, utf8 ("选中整组"));
    // Phase 56: the group as one object, moved one bar on. Not "复制" + "粘贴":
    // that spends the clipboard and lands wherever the playhead is, and the
    // point of this item is that the answer is already known.
    menu.addItem (8, utf8 ("复制到后一小节"));
    menu.addItem (2, utf8 ("编组挂载音源…"));
    // Phase 20: two parameters, and the tick means "this curve already has a
    // row in the automation window", so the menu doubles as the index.
    // Phase 42: one verb, one entry — the two parameters are the submenu.
    {
        juce::PopupMenu autoMenu;
        autoMenu.addItem (3, utf8 ("组音量"), true, group->getLevelAutomation().isEnabled());
        autoMenu.addItem (4, utf8 ("组声相"), true, group->getPanAutomation().isEnabled());
        menu.addSubMenu (utf8 ("创建自动化"), autoMenu);
    }
    // Phase 35: the tick is the state, so the menu is also the readout — the
    // same convention the two automation rows above already use.
    menu.addItem (6, utf8 ("静音（音符与采样一起）"), true, group->isMuted());
    // Phase 50: freeze the group into one block. The render keeps the group's
    // own curves, its mount and its trim, because the host publishes the same
    // schedule the sound card would hear — this is not a second mixer.
    menu.addItem (7, utf8 ("渲染为采样…"));
    menu.addSeparator();
    menu.addItem (5, utf8 ("解散编组（音符保留）"));
}


void PianoRollComponent::showGroupMenu (int layerIndex, int groupId, int occurrence,
                                        juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    fillGroupMenu (menu, layerIndex, groupId, occurrence);

    // Nothing to show means the group went away between the click and here.
    if (menu.getNumItems() == 0)
        return;

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, layerIndex, groupId, occurrence, screenPos] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            switch (result)
                            {
                                case 1: safe->selectGroup (groupId, occurrence); break;

                                case 8:
                                    safe->duplicateGroupToNextBar (layerIndex, groupId, occurrence);
                                    break;

                                case 2:
                                    if (safe->onGroupMountRequested != nullptr)
                                        safe->onGroupMountRequested (layerIndex, groupId, screenPos);
                                    break;

                                case 3:
                                case 4:
                                    if (safe->onGroupAutomationRequested != nullptr)
                                        safe->onGroupAutomationRequested (layerIndex, groupId,
                                                                          result == 4 ? 1 : 0);
                                    break;

                                case 5:
                                    safe->disbandGroup (layerIndex, groupId, occurrence);
                                    break;

                                case 6:
                                    // Re-resolved: the menu can outlive the group
                                    // it was opened on (undo, another window).
                                    if (auto* owner = safe->stack.getLayer (layerIndex))
                                        if (auto* live = owner->findGroup (groupId))
                                        {
                                            live->setMuted (! live->isMuted());
                                            safe->stack.notifyNotesChanged();
                                            safe->repaint();
                                        }
                                    break;

                                case 7:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex      = layerIndex;
                                        request.groupId         = groupId;
                                        request.groupOccurrence = occurrence;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                default: break;
                            }
                        });
}


//==============================================================================
//  Phase 90: quantise
//==============================================================================
double PianoRollComponent::quantizeGridAt (int index, double barBeats) noexcept
{
    switch (index)
    {
        case 0:  return 1.0 / 8.0 * 0.5;    // 1/32 note: an eighth of a beat
        case 1:  return gridDivision;       // 1/16 note: a quarter of a beat
        case 2:  return 0.5;                // 1/8 note
        case 3:  return 1.0;                // 1/4 note = one beat
        case 4:  return barBeats;           // a bar, whatever the meter says
        default: return 0.0;
    }
}


juce::String PianoRollComponent::quantizeGridName (int index)
{
    switch (index)
    {
        case 0:  return utf8 ("1/32 音符");
        case 1:  return utf8 ("1/16 音符");
        case 2:  return utf8 ("1/8 音符");
        case 3:  return utf8 ("1/4 音符");
        case 4:  return utf8 ("1 小节");
        default: return {};
    }
}


double PianoRollComponent::quantizeStrengthAt (int index) noexcept
{
    switch (index)
    {
        case 0:  return 1.0;
        case 1:  return 0.75;
        case 2:  return 0.5;
        case 3:  return 0.25;
        default: return 1.0;
    }
}


void PianoRollComponent::setQuantizeGrid (double gridBeats) noexcept
{
    if (gridBeats > 0.0)
        quantizeGridBeats = gridBeats;
}


void PianoRollComponent::setQuantizeStrength (double amount) noexcept
{
    quantizeStrength = juce::jlimit (0.0, 1.0, amount);
}


int PianoRollComponent::quantizeSelection (double gridBeats, double strength)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || ! (gridBeats > 0.0))
        return 0;

    const double amount = juce::jlimit (0.0, 1.0, strength);

    // "0%" is "leave it exactly where it is". Letting it through would rewrite
    // every start with itself, which is a move of zero that still costs an undo
    // step and a repaint — the two things the early return below is about.
    if (amount <= 0.0)
        return 0;

    int moved = 0;
    double reach = 0.0;

    // The comparison is against a billionth of a beat, not against zero.
    // `round (x / g) * g` is NOT bit-exact even for a start already on the grid
    // (0.3 / 0.1 is 2.9999999999999996), so comparing exactly would report a
    // move on every press and turn "quantise twice" into two undo steps that
    // change nothing anybody can hear.
    const auto land = [&] (double& startBeat, double lengthBeats)
    {
        const double target = juce::jmax (0.0, quantize::apply (startBeat, gridBeats, amount));

        if (std::abs (target - startBeat) < 1.0e-9)
            return;

        startBeat = target;
        reach = juce::jmax (reach, target + lengthBeats);
        ++moved;
    };

    for (int id : selection)
        if (auto* note = layer->findNote (id))
            land (note->startBeat, note->lengthBeats);

    for (int id : sampleSelection)
        if (auto* clip = layer->findSample (id))
            land (clip->startBeat, clip->lengthBeats);

    if (moved == 0)
        return 0;

    // Same rule as a drag that runs off the end: the timeline grows, nothing is
    // dragged back to fit. Quantise is a nudge of at most half a grid step, so
    // this only ever fires on a note already sitting against the end.
    if (reach > timelineBeats)
        ensureTimelineExtendsTo (reach + gridBeats);

    //-- Re-anchor, then propagate the shape ----------------------------------
    // Two steps, and the first one is not optional. `syncGroupShape` places a
    // sibling at `its own origin + the source's relative shape`, and the origin
    // is a STORED number (P62) that a drag keeps in step through
    // applyOccurrenceOriginsForMove. Quantise does not drag: it moves each
    // member by its own error, so the origin no longer names the earliest
    // member — and a stale origin shifts every copy by exactly the source's
    // first error while the shape looks right. Measured: a copy at 8.00/8.57
    // became 7.89/8.64 instead of 8.00/8.75 before this loop existed.
    //
    // "The origin is the earliest member" is not a new rule: it is exactly what
    // occurrenceStart() materialises when nothing is stored, and what every
    // reader of getOccurrenceStarts() already assumes.
    const auto touched = groupOccurrencesOf (selection, sampleSelection);

    for (const auto& entry : touched)
    {
        double origin = std::numeric_limits<double>::max();

        for (const auto& n : layer->getNotes())
            if (n.groupId == entry.first && n.groupOccurrence == entry.second)
                origin = juce::jmin (origin, n.startBeat);

        for (const auto& clip : layer->getSamples())
            if (clip.groupId == entry.first && clip.groupOccurrence == entry.second)
                origin = juce::jmin (origin, clip.startBeat);

        if (origin < std::numeric_limits<double>::max())
            layer->setOccurrenceStart (entry.first, entry.second, origin);
    }

    // Unconditionally, and this is the one place this differs from a drag: see
    // the declaration. A start IS the group's shape, and quantise changes each
    // start by its own error, so the appearance that was touched is a new shape
    // for every copy of it.
    syncGroupShapesAfterEdit (touched);

    stack.notifyNotesChanged();
    repaint();

    return moved;
}


void PianoRollComponent::addQuantizeItems (juce::PopupMenu& menu, bool hasSelection) const
{
    juce::PopupMenu sub;

    for (int i = 0; i < numQuantizeGrids; ++i)
        sub.addItem (quantizeGridBase + i, quantizeGridName (i), hasSelection,
                     std::abs (quantizeGridAt (i, (double) beatsPerBar) - quantizeGridBeats) < 1.0e-9);

    sub.addSeparator();

    // Strength lives one level in, because it is a modifier of the verb above it
    // rather than a sixth grid: twelve flat entries would bury "1/16", which is
    // the one people press.
    juce::PopupMenu amounts;

    for (int i = 0; i < numQuantizeStrengths; ++i)
    {
        const double amount = quantizeStrengthAt (i);

        amounts.addItem (quantizeStrengthBase + i,
                         juce::String ((int) std::lround (amount * 100.0)) + "%",
                         hasSelection, std::abs (amount - quantizeStrength) < 1.0e-9);
    }

    sub.addSubMenu (utf8 ("强度"), amounts, hasSelection);

    menu.addSubMenu (utf8 ("量化"), sub, hasSelection);
}


void PianoRollComponent::addRenderScopeItems (juce::PopupMenu& menu, bool hasSelection)
{
    menu.addItem (7, utf8 ("渲染选中内容为采样…"), hasSelection);
    menu.addItem (8, utf8 ("渲染整个图层为采样…"), true);
}


void PianoRollComponent::showCanvasMenu (juce::Point<int> screenPos)
{
    const int count    = (int) selection.size();
    const int clips    = (int) sampleSelection.size();   // Phase 35
    const int groupId  = getSelectedGroupId();
    const int ownerLayer = groupId >= 0 ? layerIndexOfGroup (groupId) : -1;

    juce::PopupMenu menu;
    menu.addSectionHeader (count > 0 || clips > 0
                             ? juce::String (count) + utf8 (" 个音符")
                                 + (clips > 0 ? utf8 (" · ") + juce::String (clips) + utf8 (" 个采样")
                                              : juce::String())
                             : juce::String (utf8 ("钢琴卷帘")));

    menu.addItem (1, utf8 ("编组选中内容"), count > 0 || clips > 0);
    menu.addItem (2, utf8 ("选中整组"), groupId >= 0);
    menu.addItem (3, utf8 ("取消编组"), groupId >= 0);
    menu.addSeparator();
    menu.addItem (4, utf8 ("编组挂载音源…"), groupId >= 0);
    {
        // Phase 42: the same verb / parameters split as everywhere else.
        juce::PopupMenu autoMenu;
        autoMenu.addItem (5, utf8 ("组音量"), groupId >= 0);
        autoMenu.addItem (6, utf8 ("组声相"), groupId >= 0);
        menu.addSubMenu (utf8 ("创建自动化"), autoMenu, groupId >= 0);
    }

    //-- Phase 90: quantise --------------------------------------------------
    // Above the bounce entries, below the group ones: it edits what is already
    // selected, and everything that CREATES something (a group, a render) stays
    // below it.
    menu.addSeparator();
    addQuantizeItems (menu, count > 0 || clips > 0);

    //-- Phase 50: bounce to a sample block ----------------------------------
    // The selection and the whole layer are here; a group has its own menu
    // entry (right-clicking its pill), which is where "this group" is obvious.
    // Built by a named function so the self-test can read the menu instead of
    // trusting a screenshot of a popup (P43's rule).
    menu.addSeparator();
    addRenderScopeItems (menu, count > 0 || clips > 0);

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, groupId, ownerLayer, screenPos] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            switch (result)
                            {
                                case 1: safe->groupSelection(); break;
                                case 2: safe->selectGroup (groupId); break;
                                case 3: safe->ungroupSelection(); break;

                                case 4:
                                    if (safe->onGroupMountRequested != nullptr)
                                        safe->onGroupMountRequested (ownerLayer, groupId, screenPos);
                                    break;

                                case 5:
                                case 6:
                                    if (safe->onGroupAutomationRequested != nullptr)
                                        safe->onGroupAutomationRequested (ownerLayer, groupId,
                                                                          result == 6 ? 1 : 0);
                                    break;

                                // Phase 50: the id list is COPIED here. The
                                // render runs on its own thread, and the
                                // selection is free to change while it does.
                                case 7:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex = safe->stack.getActiveLayerIndex();
                                        request.noteIds    = safe->selection;
                                        request.sampleIds  = safe->sampleSelection;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                case 8:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex = safe->stack.getActiveLayerIndex();
                                        request.wholeLayer = true;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                default:
                                    if (result >= quantizeGridBase
                                          && result < quantizeGridBase + numQuantizeGrids)
                                    {
                                        const double grid = quantizeGridAt (result - quantizeGridBase,
                                                                            (double) safe->getBeatsPerBar());

                                        const int moved = safe->quantizeSelection (grid,
                                                                                   safe->getQuantizeStrength());

                                        // Ticked even when nothing moved: the
                                        // user made a choice, and the next
                                        // press should show what it was.
                                        safe->setQuantizeGrid (grid);

                                        if (moved > 0)
                                            safe->flashStatus (utf8 ("已量化 ") + juce::String (moved)
                                                                 + utf8 (" 个 · ")
                                                                 + quantizeGridName (result - quantizeGridBase)
                                                                 + " · "
                                                                 + juce::String ((int) std::lround (safe->getQuantizeStrength() * 100.0))
                                                                 + "%");
                                        else
                                            safe->flashStatus (utf8 ("已经在网格上，没有东西动"));
                                    }
                                    else if (result >= quantizeStrengthBase
                                               && result < quantizeStrengthBase + numQuantizeStrengths)
                                    {
                                        safe->setQuantizeStrength (quantizeStrengthAt (result - quantizeStrengthBase));
                                    }
                                    break;
                            }
                        });
}
} // namespace atomic

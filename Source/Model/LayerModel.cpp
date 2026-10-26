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
            n.groupId = -1;

    // Phase 27: clips are members too, and they are detached the same way. A
    // clip left pointing at a dissolved group would be a member of nothing that
    // still draws inside somebody's bracket.
    for (auto& s : samples)
        if (s.groupId == groupId)
            s.groupId = -1;

    groups.erase (std::remove_if (groups.begin(), groups.end(),
                                  [groupId] (const NoteGroup& g) { return g.getId() == groupId; }),
                  groups.end());
}

void Layer::assignNotesToGroup (const std::vector<int>& noteIds, int groupId)
{
    // A group id that does not exist is the same as "no group": better to
    // detach than to leave notes pointing at a ghost.
    const int target = findGroup (groupId) != nullptr ? groupId : -1;

    for (int noteId : noteIds)
        if (auto* note = findNote (noteId))
            note->groupId = target;
}

int Layer::getGroupIdOfNote (int noteId) const noexcept
{
    for (const auto& n : notes)
        if (n.id == noteId)
            return n.groupId;

    return -1;
}

//==============================================================================
void Layer::assignSamplesToGroup (const std::vector<int>& sampleIds, int groupId)
{
    const int target = findGroup (groupId) != nullptr ? groupId : -1;

    for (int sampleId : sampleIds)
        if (auto* sample = findSample (sampleId))
            sample->groupId = target;
}

int Layer::getGroupIdOfSample (int sampleId) const noexcept
{
    if (const auto* sample = findSample (sampleId))
        return sample->groupId;

    return -1;
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
    // label for nothing and route by a group nobody can see.
    for (auto& n : notes)
        if (n.groupId >= 0 && findGroup (n.groupId) == nullptr)
            n.groupId = -1;
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
    sampleToAdd.lengthBeats = juce::jmax (0.25, sampleToAdd.lengthBeats);
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
        s.lengthBeats = juce::jmax (0.25, s.lengthBeats);
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

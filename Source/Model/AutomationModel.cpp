#include "AutomationModel.h"

namespace atomic
{

//==============================================================================
int AutomationModel::addTrack (AutomationOwner owner, int ownerId, int target)
{
    if (const int existing = findTrackId (owner, ownerId, target); existing > 0)
    {
        selectTrack (existing);
        return existing;
    }

    // The master bus is the one owner whose id is legitimately 0: there is
    // exactly one of it, so it needs no identity.
    if ((int) tracks.size() >= maxTracks
         || (owner != AutomationOwner::master && ownerId <= 0))
        return -1;

    AutomationTrack track;
    track.id      = nextTrackId++;
    track.owner   = owner;
    track.ownerId = ownerId;
    track.target  = target;

    tracks.push_back (track);
    selectedId = track.id;

    notify();
    return track.id;
}

void AutomationModel::removeTrack (int trackId)
{
    const int index = indexOfTrack (trackId);

    if (index < 0)
        return;

    tracks.erase (tracks.begin() + index);

    if (selectedId == trackId)
        selectedId = tracks.empty() ? -1
                                    : tracks[(size_t) juce::jmin (index, (int) tracks.size() - 1)].id;

    notify();
}

bool AutomationModel::moveTrack (int trackId, int delta)
{
    const int index = indexOfTrack (trackId);

    if (index < 0 || delta == 0)
        return false;

    const int target = juce::jlimit (0, (int) tracks.size() - 1, index + delta);

    if (target == index)
        return false;

    auto moved = tracks[(size_t) index];
    tracks.erase (tracks.begin() + index);
    tracks.insert (tracks.begin() + target, moved);

    notify();
    return true;
}

bool AutomationModel::contains (AutomationOwner owner, int ownerId, int target) const
{
    return findTrackId (owner, ownerId, target) > 0;
}

int AutomationModel::findTrackId (AutomationOwner owner, int ownerId, int target) const
{
    for (const auto& track : tracks)
        if (track.owner == owner && track.ownerId == ownerId && track.target == target)
            return track.id;

    return -1;
}

void AutomationModel::removeTracksForOwner (AutomationOwner owner, int ownerId)
{
    const int before = (int) tracks.size();

    tracks.erase (std::remove_if (tracks.begin(), tracks.end(),
                                  [owner, ownerId] (const AutomationTrack& t)
                                  {
                                      return t.owner == owner && t.ownerId == ownerId;
                                  }),
                  tracks.end());

    if ((int) tracks.size() == before)
        return;

    if (findTrack (selectedId) == nullptr)
        selectedId = tracks.empty() ? -1 : tracks.front().id;

    notify();
}

void AutomationModel::selectTrack (int trackId)
{
    if (selectedId == trackId)
        return;

    if (trackId > 0 && findTrack (trackId) == nullptr)
        return;

    selectedId = trackId;
    notify();
}

int AutomationModel::getSelectedTrackId() const
{
    return selectedId;
}

const AutomationTrack* AutomationModel::findTrack (int trackId) const
{
    for (const auto& track : tracks)
        if (track.id == trackId)
            return &track;

    return nullptr;
}

const AutomationTrack* AutomationModel::getSelected() const
{
    return findTrack (selectedId);
}

int AutomationModel::indexOfTrack (int trackId) const
{
    for (int i = 0; i < (int) tracks.size(); ++i)
        if (tracks[(size_t) i].id == trackId)
            return i;

    return -1;
}

void AutomationModel::restoreTracks (std::vector<AutomationTrack> restoredTracks)
{
    tracks.clear();
    nextTrackId = 1;
    selectedId = -1;

    for (auto track : restoredTracks)
    {
        if (track.ownerId <= 0 || (int) tracks.size() >= maxTracks)
            continue;

        if (findTrackId (track.owner, track.ownerId, track.target) > 0)
            continue;                      // same curve twice: drop the duplicate

        // Ids from the file are kept when usable, so a later save round-trips.
        if (track.id <= 0 || findTrack (track.id) != nullptr)
            track.id = nextTrackId;

        nextTrackId = juce::jmax (nextTrackId, track.id + 1);
        tracks.push_back (track);
    }

    if (! tracks.empty())
        selectedId = tracks.front().id;

    notify();
}

void AutomationModel::clear()
{
    if (tracks.empty() && selectedId < 0)
        return;

    tracks.clear();
    selectedId = -1;
    notify();
}

void AutomationModel::notify()
{
    listeners.call ([] (Listener& l) { l.automationTracksChanged(); });
}

} // namespace atomic

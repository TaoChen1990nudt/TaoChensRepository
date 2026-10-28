#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "AutomationLane.h"

namespace atomic
{

//==============================================================================
/** What an automation track is attached to. */
enum class AutomationOwner
{
    layer,
    slot,
    /** Phase 16: a note group. `ownerId` is NoteGroup::getId(), which is only
        unique INSIDE its layer — the window resolves it by scanning the stack,
        so a group curve always belongs to exactly one layer's group. */
    group,
    /** Phase 18: the master bus. There is exactly one, so `ownerId` is always 0
        — the field stays because every track carries one and a special case in
        the track list would be worse than a constant. */
    master,
    /** Phase 80: the project's tempo. Also one-of-a-kind, also `ownerId` 0.

        A separate owner rather than a fourth lane on `master`, because tempo is
        not a signal path: "reset the mixer" must not erase a tempo map, and a
        row labelled "主输出 · 速度" would be a row that lies about what it
        drives. `target` is always 0 — there is one tempo curve, not a list. */
    tempo
};

//==============================================================================
/** One row in the automation window.

    This is a REFERENCE, not a copy of the curve. The curve itself stays inside
    the Layer or SynthSlot it drives, so the audio engine can read it without
    knowing this list exists, and deleting a layer takes its curves with it.

    `ownerId` is a stable id (Layer::getId / SynthSlot::id), never an index:
    tracks outlive index changes, and an index would silently reattach a curve
    to a different sheet after a layer is deleted.
*/
struct AutomationTrack
{
    int             id      = 0;
    AutomationOwner owner   = AutomationOwner::layer;
    int             ownerId = 0;
    int             target  = 0;   // LayerAutomationTarget or SlotAutomationTarget

    bool operator== (const AutomationTrack& other) const noexcept
    {
        return id == other.id;
    }
};

//==============================================================================
/**
    The ordered list of automation tracks shown in the automation window.

    Deliberately dumb: it owns order, identity and selection, and nothing else.
    The curve data, the fader it multiplies, and the audio that obeys it all
    live elsewhere. Keeping this as a separate list is what allows two tracks
    for the same layer (volume and pan) to be reordered or deleted
    independently without touching the layer.
*/
class AutomationModel
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** A track was added, removed, reordered or re-selected. */
        virtual void automationTracksChanged() {}
    };

    static constexpr int maxTracks = 32;

    AutomationModel() = default;

    /** Adds a track for (owner, ownerId, target).

        Returns the existing track's id when one is already there — asking for
        the same curve twice must select it, not create a duplicate row that
        edits the same data behind the user's back. Returns -1 when the list is
        full. */
    int  addTrack (AutomationOwner owner, int ownerId, int target);

    void removeTrack (int trackId);

    /** Moves a track by `delta` positions, clamped. Returns true if it moved. */
    bool moveTrack (int trackId, int delta);

    bool contains (AutomationOwner owner, int ownerId, int target) const;
    int  findTrackId (AutomationOwner owner, int ownerId, int target) const;

    /** Removes every track that points at a layer / synth that no longer
        exists. Called after a deletion so the window cannot show a row for a
        curve nobody owns. */
    void removeTracksForOwner (AutomationOwner owner, int ownerId);

    void selectTrack (int trackId);
    int  getSelectedTrackId() const;

    const AutomationTrack* findTrack (int trackId) const;
    const AutomationTrack* getSelected() const;

    const std::vector<AutomationTrack>& getTracks() const noexcept { return tracks; }
    int getNumTracks() const noexcept { return (int) tracks.size(); }

    int indexOfTrack (int trackId) const;

    /** Replaces the whole list (project load). Ids are preserved; duplicates of
        the same (owner, ownerId, target) are dropped, order is kept. */
    void restoreTracks (std::vector<AutomationTrack> restoredTracks);

    void clear();

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void notify();

private:
    std::vector<AutomationTrack> tracks;
    juce::ListenerList<Listener> listeners;
    int nextTrackId = 1;
    int selectedId = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationModel)
};

} // namespace atomic

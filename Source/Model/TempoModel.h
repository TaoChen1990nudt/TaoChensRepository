#pragma once

#include <juce_core/juce_core.h>

#include "AutomationLane.h"
#include "TempoMap.h"

namespace atomic
{

//==============================================================================
/**
    The project's tempo automation: one curve, and nothing else.

    Deliberately NOT a field on MixerModel, even though the master bus is the
    other one-of-a-kind owner with lanes: tempo is not a signal path. Resetting
    the mixer must not erase a tempo map, and the BPM field — not a channel strip
    — is where the user thinks about it. A separate object is also what keeps the
    rule from Phase 12 intact: the model stores the curve, the track list stores
    a reference, the engine reads a snapshot.

    **It does not own the manual BPM.** That number belongs to the transport
    (`AudioEngine::bpm`) and the BPM field, and there must be exactly one of it —
    same reasoning as "a name is one field, not a copy". This object answers a
    different question: "when the automation is on, what does it say?"
*/
class TempoModel
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** The curve, its enabled flag, or its points changed. */
        virtual void tempoChanged() {}
    };

    TempoModel();

    AutomationLane&       getAutomation() noexcept       { return lane; }
    const AutomationLane& getAutomation() const noexcept { return lane; }

    /** Shorthand for `getAutomation().isEnabled()` — the switch every other lane
        also keeps separate from its data, so turning automation off restores the
        BPM field without destroying the curve. */
    bool isAutomated() const noexcept { return lane.isEnabled(); }
    void setAutomated (bool shouldAutomate);

    /** The map the transport should follow. `manualBpm` is the BPM field's
        value, i.e. what the tempo is when the automation is off. */
    TempoMap buildMap (double manualBpm, double lengthBeats) const;

    /** A flat line at the project default (120 BPM), automation off.

        Used as the constructed state and when a project that has no tempo lane
        is loaded, so "no automation" and "automation at one speed" are not two
        different things the loader has to distinguish. */
    void resetToDefault (double lengthBeats);

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void notify();

private:
    AutomationLane lane;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoModel)
};

} // namespace atomic

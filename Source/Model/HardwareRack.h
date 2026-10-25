#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <vector>

#include "AutomationLane.h"
#include "Gain.h"

namespace atomic
{

//==============================================================================
/** Oscillator shapes available to a rack slot. Deliberately crude: this phase
    is about routing, not about synthesis quality. */
enum class Waveform
{
    sine,
    saw,
    square,
    triangle
};

juce::String waveformName (Waveform);
/** sine -> saw -> square -> triangle -> sine */
Waveform nextWaveform (Waveform);

//==============================================================================
/**
    One synth in the rack.

    `id` is stable and NEVER reused. Layers and notes address a synth by id, not
    by index, because the rack is a list the user can add to and delete from —
    an index would silently repoint every route the moment something above it
    is removed. That single decision is what keeps "Layer 2 -> Synth B" meaning
    the same thing after Synth A is deleted.
*/
struct SynthSlot
{
    /** Mirrors MixerModel::numChannels. Kept here so the rack does not have to
        include the mixer just to clamp a number. */
    static constexpr int maxMixerChannels = 8;

    int          id       = 0;
    juce::String name;
    juce::Colour colour   { 0xff7c96f5 };
    Waveform     waveform = Waveform::saw;
    float        levelDb  = -6.0f;
    bool         muted    = false;

    //-- Phase 11: which mixer channel this synth feeds ----------------------
    /** -1 means "not sent to the console": the synth goes straight to the
        master strip. That is the default, because a channel nobody asked for
        would put eight extra gain stages between the instrument and the
        speakers on a project that has not been mixed yet.

        Layers deliberately do NOT have one of these. A sheet of notes picks an
        instrument; the instrument is what a console channel receives. */
    int mixerChannel = -1;

    bool sendsToMixer() const noexcept
    {
        return juce::isPositiveAndBelow (mixerChannel, maxMixerChannels);
    }

    //-- Phase 5: the synth's own output stage -------------------------------
    float pan    = 0.0f;
    bool  soloed = false;

    //-- Phase 9: a hosted VST3 instrument -----------------------------------
    /** 0 means "use the built-in oscillator". Anything else is a PluginHost
        instance id, and the waveform is then irrelevant: the plugin makes the
        sound and this slot only carries its fader, pan and automation.

        The id, not a pointer. A project can name a plugin that is not installed
        any more; that has to resolve to silence, not to a dangling pointer. */
    int    pluginId   = 0;
    juce::String pluginName;
    std::shared_ptr<const juce::PluginDescription> plugin;

    bool isPlugin() const noexcept { return pluginId > 0; }

    /** Macro automation for this synth. Both parameters keep their own lane, so
        a filter sweep and a level ride can be drawn and heard at the same time. */
    AutomationLane& automationFor (SlotAutomationTarget t) noexcept
    {
        return automationLanes[(size_t) t];
    }

    const AutomationLane& automationFor (SlotAutomationTarget t) const noexcept
    {
        return automationLanes[(size_t) t];
    }

    /** The lane the rack panel is pointed at. */
    AutomationLane& getAutomation() noexcept
    {
        return automationFor (automationTarget);
    }

    std::array<AutomationLane, 2> automationLanes;
    SlotAutomationTarget automationTarget = SlotAutomationTarget::filter;
    bool                 automationExpanded = false;

    /** Phase 17: the whole card shrinks to a one-line title bar. View state on
        the model for the same reason as above: the rack's row geometry is a
        function of the model, so a flag kept in the panel would have to be
        mirrored back into every hit test. */
    bool collapsed = false;

    float getGainLinear() const noexcept
    {
        return gain::fromDb (levelDb);
    }
};

//==============================================================================
/** The right-hand rack. Owns the synth slots; knows nothing about layers. */
class RackModel
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;

        /** Slot added / removed / renamed / re-tuned / muted. */
        virtual void rackChanged() {}
    };

    /** Starts with Synth A and Synth B, so routing is meaningful immediately. */
    RackModel();

    /** A brand new session's rack: ONE built-in synth, id 1, no plugin, no
        mixer send. Static so the session builder does not have to reach for the
        ordinal-name helper. */
    static SynthSlot makeDefaultSlot();

    int getNumSlots() const noexcept { return (int) slots.size(); }

    SynthSlot*       getSlot (int index) noexcept;
    const SynthSlot* getSlot (int index) const noexcept;

    int getSlotIdAt (int index) const;

    SynthSlot*       findSlot (int slotId) noexcept;
    const SynthSlot* findSlot (int slotId) const noexcept;
    int              indexOfSlot (int slotId) const;

    /** First slot's id, or -1 for an empty rack. Final fallback for a layer
        whose configured output no longer exists. */
    int getFirstSlotId() const;

    /** Adds a synth, auto-named (Synth C, D, ...). Returns its id. */
    int  addSynth();

    /** Adds a rack slot driven by a hosted VST3. `instanceId` must already be
        loaded in PluginHost; 0 adds the slot anyway, silent, so a project that
        lost its plugin still shows what used to be there. Returns the slot id. */
    int  addPluginSynth (const juce::PluginDescription& description, int instanceId);
    void removeSlot (int slotId);

    //-- Phase 11: replace the instrument inside a slot ----------------------
    /** Swaps a slot's instrument WITHOUT changing its id.

        Why not delete + add: every layer route, note override and automation
        track addresses the slot by id. A "replace" that minted a new id would
        silently drop all of that on the floor — the exact failure mode the id
        scheme exists to prevent. So the slot keeps its identity, its fader, its
        pan and its curves; only what makes the sound changes.

        Returns the plugin instance id that was unloaded (0 if there was none),
        so the caller can hand it to the host's teardown path. */
    int replaceWithNative (int slotId, Waveform waveform);
    int replaceWithPlugin (int slotId, const juce::PluginDescription& description, int instanceId);

    /** Phase 11: which console channel this synth feeds, or -1 for "straight
        to master". Anything out of range is stored as -1. */
    void setMixerChannel (int slotId, int channel);

    /** Replaces the whole rack in one step (project load), keeping the ids the
        file was written with so every layer route and note override still
        means the same synth afterwards. */
    void restoreSlots (std::vector<SynthSlot> restoredSlots);

    void setMuted (int slotId, bool shouldBeMuted);
    void setLevelDb (int slotId, float newLevelDb);
    void setWaveform (int slotId, Waveform newWaveform);

    //-- Phase 5 mixture controls ---------------------------------------------
    void setPan (int slotId, float pan);
    void setSoloed (int slotId, bool shouldBeSoloed);
    void setAutomationTarget (int slotId, SlotAutomationTarget target);
    void setAutomationExpanded (int slotId, bool expanded);
    void setCollapsed (int slotId, bool shouldCollapse);
    void touch();

    /** Name for a slot id; "—" when the id is unknown. Used by UI readouts. */
    juce::String getSlotName (int slotId) const;
    juce::Colour getSlotColour (int slotId) const;

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    void notify();

    std::vector<SynthSlot>        slots;
    juce::ListenerList<Listener>  listeners;
    int nextSlotId = 1;
    int nextOrdinal = 0;      // A, B, C... never decremented, so names never repeat

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackModel)
};

} // namespace atomic

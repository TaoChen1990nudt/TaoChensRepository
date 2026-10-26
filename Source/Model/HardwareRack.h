#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <vector>

#include "AutomationLane.h"
#include "Gain.h"
#include "Media/SampleLibrary.h"

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
        include the mixer just to clamp a number — ProjectIO static_asserts the
        two agree, so this cannot quietly fall behind the console again (it did:
        Phase 25 raised the console to 127 strips and this stayed at 8, which
        silently refused every send above channel 8). */
    static constexpr int maxMixerChannels = 127;

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

    /** Phase 38: what the card, the menus and every "→ Synth A" readout CALL
        this synth.

        A hosted plugin's name IS its identity: "Serum" tells you what will
        happen when you play a note on it, and "Synth A" does not. Leaving the
        card on the old slot name is why replacing a built-in synth with a VST3
        looked like the click had done nothing — the pill changed from SAW to
        "VST3 · 插件" and nothing else did.

        A built-in synth keeps its slot name on purpose: the pill already says
        SAW, and "SAW" as a card title is not a name. A sampler keeps whatever
        it was called, because its file name is on the pill.

        Derived, not written: replacing back to an oscillator must give the old
        name back, and a stored `name` cannot do that once it has been
        overwritten. */
    juce::String getDisplayName() const
    {
        if (isPlugin() && pluginName.isNotEmpty())
            return pluginName;

        return name;
    }

    //-- Phase 27: a slot that holds a sample instead of an instrument --------
    /** A sampler slot is a CONTAINER, not a player.

        Dropping a file into the roll grows one of these, holding exactly that
        one clip. The point is the indirection: a clip in the roll names a
        sampler slot to play, so re-pointing the clip at another sampler changes
        what it sounds like without touching its position, length or gain — the
        same way mounting a note on another synth changes its timbre.

        It deliberately has no oscillator. A slot whose job is "hold this file"
        must not also make a saw wave when a note happens to be routed to it,
        because then "which slot am I on" would silently become a change of
        instrument as well as a change of sample. */
    bool sampler = false;
    std::shared_ptr<const SampleBuffer> sampleBuffer;
    juce::String samplePath;             // absolute, so a reload can find it

    bool isSampler() const noexcept { return sampler; }

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

    const AutomationLane& getAutomation() const noexcept
    {
        return automationFor (automationTarget);
    }

    /** Phase 42: which parameter of the hosted instrument `plugin` drives.

        -1 means "no parameter chosen", and that is the default: an index only
        means something relative to one plugin's list, so a slot that has never
        had one picked must not claim parameter 0 of whatever gets mounted next.
        Same field, same meaning as NoteGroup::getPluginParam(). */
    int  getPluginParam() const noexcept  { return pluginParam; }
    void setPluginParam (int index) noexcept { pluginParam = index; }
    bool drivesPluginParam() const noexcept  { return pluginParam >= 0; }

    AutomationLane&       getPluginAutomation() noexcept
    {
        return automationFor (SlotAutomationTarget::plugin);
    }

    const AutomationLane& getPluginAutomation() const noexcept
    {
        return automationFor (SlotAutomationTarget::plugin);
    }

    std::array<AutomationLane, numSlotAutomationTargets> automationLanes;
    SlotAutomationTarget automationTarget = SlotAutomationTarget::filter;
    bool                 automationExpanded = false;
    int                  pluginParam = -1;

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

    //-- Phase 27: sampler slots ---------------------------------------------
    /** Grows a slot that holds one decoded sample, named after the file, and
        returns its id. Reuses an existing slot that already holds the SAME file
        so dragging one file twice does not litter the rack with twins — the
        second drop points at the sampler the first one created. */
    int  addSampler (std::shared_ptr<const SampleBuffer> buffer,
                     const juce::String& path,
                     const juce::String& displayName);

    /** Every sampler slot id, in rack order. What the clip's "mount" menu lists. */
    std::vector<int> getSamplerSlotIds() const;

    /** First slot that can actually PLAY A NOTE, skipping samplers (which have
        no oscillator) and empty plugin slots. The fallback for a layer whose
        configured output is gone: answering "the sampler I happen to have first"
        would mute every note on that sheet for no visible reason. */
    int getFirstInstrumentSlotId() const;

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

    /** Phase 42: which parameter of the hosted instrument the slot's `plugin`
        lane writes. Notifies, because the published schedule carries the index
        and a curve pointing at the wrong one would be audible immediately. */
    void setPluginParam (int slotId, int paramIndex);

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

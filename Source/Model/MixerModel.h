#pragma once

#include <juce_core/juce_core.h>

#include <array>

#include "Audio/Effects.h"
#include "AutomationLane.h"

namespace atomic
{

//==============================================================================
/**
    The mixer's own state: eight insert chains plus the master strip.

    Deliberately does NOT duplicate the channel faders. A channel's volume, pan,
    mute and solo are the LAYER's, and the LayerPanel already owns them — two
    edit paths into one value is fine (that is what a mixer is), two copies of
    one value is not. So this model stores exactly what the layers do not have:
    the eight insert slots of each channel, and the master strip.

    Channel i is layer i. Layers are a stack, not a reorderable list, so that
    mapping is stable for the lifetime of a session. Removing a layer shifts the
    ones below it up one channel, and their inserts stay with the channel
    NUMBER, exactly like a hardware console.
*/
class MixerModel
{
public:
    static constexpr int numChannels   = 8;
    static constexpr int numInserts    = fx::ChainState::maxSlots;
    static constexpr int masterChannel = numChannels;     // index 8

    /** Phase 18: the console's strip count is a session value, not a constant.
        The engine still owns eight buses, so that is the ceiling; a project can
        grow or shrink inside 1..8. Channel NUMBERS stay stable when the count
        changes — shrinking hides the top strips, it never renumbers a strip
        that is still in use, because a renumbered strip would silently move
        every synth routed to it. */
    static constexpr int minChannels = 1;
    static constexpr int maxChannels = numChannels;

    int  getChannelCount() const noexcept { return channelCount; }
    bool addChannel();                    // false at the ceiling
    bool removeChannel();                 // false at the floor
    void setChannelCount (int count);     // clamped, notifies

    /** Phase 18: master volume / pan automation. Two lanes, always present, the
        same way a layer always has its two — enabling is a separate flag, so
        switching the curve off restores the fader. */
    static constexpr int numMasterParameters = 2;    // 0 = level, 1 = pan

    AutomationLane&       automationFor (int target) noexcept;
    const AutomationLane& automationFor (int target) const noexcept;

    class Listener
    {
    public:
        virtual ~Listener() = default;

        virtual void mixerChanged() {}
    };

    MixerModel() = default;

    //-- Inserts --------------------------------------------------------------
    /** 0..7 = channels, 8 = master. Out of range returns the master's chain so
        a stale index can never write into another channel's memory. */
    std::array<fx::Settings, numInserts>&       getInserts (int channel) noexcept;
    const std::array<fx::Settings, numInserts>& getInserts (int channel) const noexcept;

    const fx::Settings& getInsert (int channel, int slot) const noexcept;

    void setInsertType (int channel, int slot, fx::Type type);

    /** Turns a slot into a hosted VST3 insert. `instanceId` comes from
        PluginHost; 0 means "remember the plugin but it failed to load", which
        keeps the rack honest instead of clearing the user's setup. */
    void setInsertPlugin (int channel, int slot, const juce::PluginDescription& description,
                          int instanceId);
    void setInsertParam (int channel, int slot, float p1, float p2);
    void setInsertEnabled (int channel, int slot, bool shouldBeEnabled);
    void clearInsert (int channel, int slot);
    void clearChannelInserts (int channel);

    //-- Master strip ---------------------------------------------------------
    float getMasterLevelDb() const noexcept { return masterLevelDb; }
    void  setMasterLevelDb (float db) noexcept
    {
        masterLevelDb = juce::jlimit (-48.0f, 6.0f, db);
    }

    float getMasterPan() const noexcept { return masterPan; }
    void  setMasterPan (float p) noexcept { masterPan = juce::jlimit (-1.0f, 1.0f, p); }

    bool isMasterMuted() const noexcept { return masterMuted; }
    void setMasterMuted (bool m) noexcept { masterMuted = m; }

    float getMasterGainLinear() const noexcept
    {
        return masterMuted ? 0.0f
                           : (masterLevelDb <= -48.0f ? 0.0f
                                                      : std::pow (10.0f, masterLevelDb / 20.0f));
    }

    /** Clears every insert and returns the master to unity (project load). */
    void resetToDefault();

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void notify();

private:
    std::array<std::array<fx::Settings, numInserts>, numChannels + 1> inserts;
    std::array<AutomationLane, numMasterParameters> masterAutomation;
    int   channelCount  = numChannels;
    float masterLevelDb = 0.0f;
    float masterPan     = 0.0f;
    bool  masterMuted   = false;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerModel)
};

} // namespace atomic

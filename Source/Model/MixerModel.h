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

    Phase 23 adds the one thing a console has that neither a layer nor a synth
    owns: which strip feeds which. It lives here for the same reason the insert
    chains do — a channel is a bus, and a bus knows its own destination.
*/
class MixerModel
{
public:
    static constexpr int numChannels   = 127;    // capacity, and the master sentinel
    static constexpr int numInserts    = fx::ChainState::maxSlots;
    static constexpr int masterChannel = numChannels;     // index 127

    /** A fresh console. Phase 25 raised the CEILING to 127 strips, but a session
        that opens with 127 empty channels is not a session, it is a wall: the
        default count stays what the app has always opened with. */
    static constexpr int defaultChannels = 8;

    /** Phase 18: the console's strip count is a session value, not a constant.
        Phase 25: a project can grow or shrink inside 1..127.

        Phase 26 changes HOW a strip leaves. "－" and "删除通道条" now remove the
        strip the user pointed at, not just the top one, so the row closes up and
        the channels above it shift down by one. That is a renumber, and a
        renumber is only safe when everything that points at a strip is moved
        with it — `removeChannelAt` does exactly that, and MixerPanel re-points
        the synths in the same breath. Doing one without the other is the bug
        this comment exists to prevent.

        `minChannels` stays 1 because it is the CLAMP for loading files (a v5
        project can legitimately hold three strips). The console the user may
        DELETE down to is `deletionFloor`, which is the shipped layout: eight
        strips is the baseline, and removing into it is not editing, it is
        dismantling the mixer. */
    static constexpr int minChannels = 1;
    static constexpr int maxChannels = numChannels;

    /** Phase 26: the floor the delete controls stop at (= 8, the default). */
    static constexpr int deletionFloor = defaultChannels;

    int  getChannelCount() const noexcept { return channelCount; }
    bool addChannel();                    // false at the ceiling
    bool removeChannel();                 // false at `deletionFloor`

    /** Phase 26: removes `channel` itself, closing the gap. Retunes every strip
        that pointed at it (`out` → master) and shifts the ones above it down, so
        "channel 4" always means the fourth strip on screen. Returns false for a
        strip that does not exist or when the console is at the floor. */
    bool removeChannelAt (int channel);

    /** Phase 26: the delete controls are live exactly when this is true. */
    bool canRemoveChannel() const noexcept { return channelCount > deletionFloor; }

    void setChannelCount (int count);     // clamped, notifies

    //-- Phase 23: strip-to-strip routing -------------------------------------
    /** Which strip a channel's post-insert signal feeds: another channel, or
        `masterChannel` (= "主输出"). Master itself has no destination here — it
        is the end of the chain and always feeds the audio device.

        The default is master for every channel, so a project that never touches
        this control renders exactly like it did in Phase 22. */
    int getChannelOutput (int channel) const noexcept;

    /** Re-points a channel's output. Refuses (returns false, changes nothing)
        when the destination is not a live channel, is the channel itself, or
        would close a loop — a loop has no render order, so it is a state this
        model must make unreachable rather than resolve at render time. */
    bool setChannelOutput (int channel, int destination);

    /** True when routing `channel` to `destination` would close a loop. The
        strip's menu greys those entries out instead of letting a click fail. */
    bool wouldLoop (int channel, int destination) const noexcept;

    //-- Phase 25: mono monitoring ---------------------------------------------
    /** Sums the strip's stereo bus to mono before it leaves for the next
        destination. A monitoring control, not a mix control: it is applied
        AFTER the inserts and before the sum, so the effect chain still sees the
        stereo signal it was set up for. */
    bool isChannelMono (int channel) const noexcept;

    /** Out-of-range channels are ignored — the strip for a channel that does
        not exist has nothing to monitor. */
    void setChannelMono (int channel, bool shouldBeMono);

    /** Phase 26: the master strip listens too. It is the same control with the
        same meaning — a listen path, not a mix decision — but it belongs to the
        master bus, which has no channel number, so it needs its own flag rather
        than an entry in `channelMono`. */
    bool isMasterMono() const noexcept { return masterMono; }
    void setMasterMono (bool shouldBeMono);

    /** Phase 18: master volume / pan automation. Two lanes, always present, the
        same way a layer always has its two — enabling is a separate flag, so
        switching the curve off restores the fader.

        Phase 43: a third lane for mute. Not a second volume lane — it is read as
        a gate at 0.5, because a mute that fades is a mute that is sometimes on. */
    static constexpr int numMasterParameters = 3;    // 0 = level, 1 = pan, 2 = mute

    AutomationLane&       automationFor (int target) noexcept;
    const AutomationLane& automationFor (int target) const noexcept;

    class Listener
    {
    public:
        virtual ~Listener() = default;

        virtual void mixerChanged() {}
    };

    MixerModel();

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

    /** Phase 43: moves an insert to another slot, sliding the ones in between.

        The chain is an ORDER, not an eight-item set — a filter before a drive
        and a drive before a filter are two different sounds — so a rack you
        cannot reorder is a rack that only plays one of them. `to` is where the
        insert ends up, and everything between shifts by one: this is a MOVE, not
        a swap, because "put this one first" must not also move the first one to
        where the dragged one came from.

        Returns false (and does nothing) for out-of-range indices, so a drag that
        wanders off the rack cannot corrupt the chain. */
    bool moveInsert (int channel, int from, int to);

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
    /** Drops any route that points at a strip which no longer exists (or at
        the strip itself). Shrinking the console must never leave a dangling
        destination behind: the renderer would then sum into a bus nobody reads
        and the channel would simply go silent. */
    void sanitiseOutputs();

    std::array<std::array<fx::Settings, numInserts>, numChannels + 1> inserts;
    std::array<AutomationLane, numMasterParameters> masterAutomation;

    /** Phase 23: destination strip per channel. `masterChannel` = "主输出". */
    std::array<int, numChannels> channelOutputs {};

    /** Phase 25: mono monitoring per strip. */
    std::array<bool, numChannels> channelMono {};
    int   channelCount  = defaultChannels;
    float masterLevelDb = 0.0f;
    float masterPan     = 0.0f;
    bool  masterMuted   = false;
    bool  masterMono    = false;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerModel)
};

} // namespace atomic

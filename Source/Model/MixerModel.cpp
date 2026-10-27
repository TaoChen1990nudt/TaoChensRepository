#include "MixerModel.h"

#include <cmath>

namespace atomic
{

//==============================================================================
namespace
{
    int clampChannel (int channel) noexcept
    {
        return juce::jlimit (0, MixerModel::masterChannel, channel);
    }

    int clampSlot (int slot) noexcept
    {
        return juce::jlimit (0, MixerModel::numInserts - 1, slot);
    }
}

//==============================================================================
MixerModel::MixerModel()
{
    // Phase 23: every strip starts plugged into the master. A fresh project
    // therefore renders exactly like it did before this routing existed.
    channelOutputs.fill (masterChannel);
}

//==============================================================================
int MixerModel::getChannelOutput (int channel) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, numChannels))
        return masterChannel;

    const int destination = channelOutputs[(size_t) channel];

    // Read-side safety net: a destination that is no longer a live strip reads
    // as the master. The write path already refuses to create one, but a shrunk
    // console or a hand-edited file must not send a channel into a dead bus.
    if (destination != masterChannel && ! juce::isPositiveAndBelow (destination, channelCount))
        return masterChannel;

    return destination;
}

bool MixerModel::wouldLoop (int channel, int destination) const noexcept
{
    if (channel == destination)
        return true;

    int cursor = destination;

    // Walk the destination's own chain. Eight strips means eight steps is
    // already more than any acyclic chain can be; the counter is there so a
    // chain that is somehow already circular terminates instead of hanging.
    for (int guard = 0; guard <= numChannels; ++guard)
    {
        if (cursor == channel)
            return true;

        if (cursor == masterChannel || ! juce::isPositiveAndBelow (cursor, numChannels))
            return false;

        cursor = getChannelOutput (cursor);
    }

    return true;
}

bool MixerModel::setChannelOutput (int channel, int destination)
{
    if (! juce::isPositiveAndBelow (channel, channelCount))
        return false;

    if (destination != masterChannel && ! juce::isPositiveAndBelow (destination, channelCount))
        return false;

    if (wouldLoop (channel, destination))
        return false;

    if (getChannelOutput (channel) == destination)
        return true;

    channelOutputs[(size_t) channel] = destination;
    notify();
    return true;
}

//==============================================================================
bool MixerModel::isChannelMono (int channel) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, numChannels))
        return false;

    return channelMono[(size_t) channel];
}

void MixerModel::setChannelMono (int channel, bool shouldBeMono)
{
    if (! juce::isPositiveAndBelow (channel, channelCount))
        return;

    if (channelMono[(size_t) channel] == shouldBeMono)
        return;

    channelMono[(size_t) channel] = shouldBeMono;
    notify();
}

void MixerModel::setMasterMono (bool shouldBeMono)
{
    if (masterMono == shouldBeMono)
        return;

    masterMono = shouldBeMono;
    notify();
}

void MixerModel::sanitiseOutputs()
{
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const int destination = channelOutputs[(size_t) channel];

        if (destination == masterChannel)
            continue;

        if (destination == channel || ! juce::isPositiveAndBelow (destination, channelCount))
            channelOutputs[(size_t) channel] = masterChannel;
    }
}

//==============================================================================
std::array<fx::Settings, MixerModel::numInserts>& MixerModel::getInserts (int channel) noexcept
{
    return inserts[(size_t) clampChannel (channel)];
}

const std::array<fx::Settings, MixerModel::numInserts>& MixerModel::getInserts (int channel) const noexcept
{
    return inserts[(size_t) clampChannel (channel)];
}

const fx::Settings& MixerModel::getInsert (int channel, int slot) const noexcept
{
    return inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];
}

void MixerModel::setInsertType (int channel, int slot, fx::Type type)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    if (insert.type == type)
        return;

    insert.type = type;
    insert.enabled = (type != fx::Type::none);

    // Phase 47: every knob starts at its type's default, not at whatever the
    // PREVIOUS effect left there — reusing a slot for an EQ must not inherit
    // the delay's feedback value.
    for (int i = 0; i < fx::maxParams; ++i)
        insert.params[(size_t) i] = fx::defaultParam (type, i);

    notify();
}

void MixerModel::setInsertParam (int channel, int slot, float p1, float p2)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    insert.params[(size_t) 0] = juce::jlimit (0.0f, 1.0f, p1);
    insert.params[(size_t) 1] = juce::jlimit (0.0f, 1.0f, p2);
    notify();
}

void MixerModel::setInsertParam (int channel, int slot, int index, float value)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    // Clamped against the TYPE's count: an EQ has nine knobs, a drive one —
    // a stray index 4 on a drive must not write past the knobs that exist.
    if (index < 0 || index >= fx::numParams (insert.type))
        return;

    insert.params[(size_t) index] = juce::jlimit (0.0f, 1.0f, value);
    notify();
}

void MixerModel::setInsertEnabled (int channel, int slot, bool shouldBeEnabled)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    if (insert.enabled == shouldBeEnabled)
        return;

    insert.enabled = shouldBeEnabled;
    notify();
}

void MixerModel::setInsertPlugin (int channel, int slot, const juce::PluginDescription& description,
                                  int instanceId)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    insert.type       = fx::Type::vst;
    insert.enabled    = true;
    insert.pluginId   = instanceId;
    insert.pluginName = description.name;
    insert.plugin     = std::make_shared<const juce::PluginDescription> (description);
    notify();
}

bool MixerModel::moveInsert (int channel, int from, int to)
{
    if (! juce::isPositiveAndBelow (channel, numChannels + 1)
        || ! juce::isPositiveAndBelow (from, numInserts)
        || ! juce::isPositiveAndBelow (to, numInserts)
        || from == to)
        return false;

    auto& chain = inserts[(size_t) clampChannel (channel)];

    // Rotate, not swap. Everything between `from` and `to` shifts by one, so
    // "drag this to the top" leaves the rest of the chain in the order it was
    // already in. A swap would drag the destination's effect back to where the
    // dragged one came from — a different edit than the one the user made, and
    // one that is impossible to undo by dragging back.
    const auto moved = chain[(size_t) from];

    if (from < to)
        for (int i = from; i < to; ++i)
            chain[(size_t) i] = chain[(size_t) i + 1];
    else
        for (int i = from; i > to; --i)
            chain[(size_t) i] = chain[(size_t) i - 1];

    chain[(size_t) to] = moved;
    notify();
    return true;
}

void MixerModel::clearInsert (int channel, int slot)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    if (insert.type == fx::Type::none)
        return;

    insert = fx::Settings {};
    notify();
}

void MixerModel::clearChannelInserts (int channel)
{
    auto& chain = inserts[(size_t) clampChannel (channel)];

    for (auto& insert : chain)
        insert = fx::Settings {};

    notify();
}

//==============================================================================
bool MixerModel::addChannel()
{
    if (channelCount >= maxChannels)
        return false;

    ++channelCount;
    notify();
    return true;
}

bool MixerModel::removeChannel()
{
    if (! canRemoveChannel())
        return false;

    --channelCount;
    sanitiseOutputs();
    notify();
    return true;
}

bool MixerModel::removeChannelAt (int channel)
{
    if (! canRemoveChannel() || ! juce::isPositiveAndBelow (channel, channelCount))
        return false;

    const int last = channelCount - 1;

    // 1. The strip's own state travels with the strip. `inserts`, `channelMono`
    //    AND `channelOutputs` are all addressed by position, so all three have to
    //    close the gap — a route is as much "this strip's property" as its
    //    insert chain is. (First version shifted the first two and left the
    //    routes in place: it decremented their VALUES but never moved them to
    //    the index of the strip that now owns them, so channel 6 lost its
    //    destination and channel 7 kept one it no longer had.)
    for (int i = channel; i < last; ++i)
    {
        inserts[(size_t) i]        = inserts[(size_t) i + 1];
        channelMono[(size_t) i]    = channelMono[(size_t) i + 1];
        channelOutputs[(size_t) i] = channelOutputs[(size_t) i + 1];
    }

    inserts[(size_t) last]        = {};
    channelMono[(size_t) last]    = false;
    channelOutputs[(size_t) last] = masterChannel;

    // 2. Routes, now that every strip sits at its final index: a destination
    //    above the hole is the same physical strip one number lower, and a
    //    destination AT the hole has nowhere to go, so it falls back to the
    //    master — the same answer the read path gives for a dead strip.
    for (int src = 0; src < last; ++src)
    {
        int& out = channelOutputs[(size_t) src];

        if (out == channel)
            out = masterChannel;
        else if (out != masterChannel && out > channel)
            --out;

        if (out == src)
            out = masterChannel;
    }

    --channelCount;

    sanitiseOutputs();
    notify();
    return true;
}

void MixerModel::setChannelCount (int count)
{
    const int clamped = juce::jlimit (minChannels, maxChannels, count);

    if (clamped == channelCount)
        return;

    channelCount = clamped;
    sanitiseOutputs();
    notify();
}

AutomationLane& MixerModel::automationFor (int target) noexcept
{
    return masterAutomation[(size_t) juce::jlimit (0, numMasterParameters - 1, target)];
}

const AutomationLane& MixerModel::automationFor (int target) const noexcept
{
    return masterAutomation[(size_t) juce::jlimit (0, numMasterParameters - 1, target)];
}

void MixerModel::resetToDefault()
{
    for (auto& chain : inserts)
        for (auto& insert : chain)
            insert = fx::Settings {};

    masterLevelDb = 0.0f;
    masterPan     = 0.0f;
    masterMuted   = false;
    masterMono    = false;
    channelCount  = defaultChannels;

    // Phase 23: "reset" also puts every strip back on the master. A factory
    // console has no strip feeding another strip.
    channelOutputs.fill (masterChannel);

    // Phase 25: and no strip is monitoring in mono.
    channelMono.fill (false);

    for (auto& lane : masterAutomation)
        lane.setEnabled (false);

    notify();
}

void MixerModel::notify()
{
    listeners.call ([] (Listener& l) { l.mixerChanged(); });
}

} // namespace atomic

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

    // Phase 100: and no strip has anything patched into its detector. The fill
    // is not decoration: the default for an `int` array is ZERO, and a zero here
    // means "every compressor on this console listens to strip 1" — a factory
    // session that ducks everything to whatever channel 0 happens to play.
    channelKeys.fill (noKeySource);
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

bool MixerModel::reaches (int from, int to) const noexcept
{
    if (from == to)
        return true;

    // Breadth-first over the console graph. `visited` is the bound: a chain
    // that is already circular (a hand-edited file) terminates instead of
    // hanging the menu that is asking the question.
    std::array<bool, numChannels> visited {};

    if (! juce::isPositiveAndBelow (from, numChannels))
        return false;

    std::array<int, numChannels> queue {};
    int head = 0;
    int tail = 0;

    queue[(size_t) tail++] = from;
    visited[(size_t) from]    = true;

    while (head < tail)
    {
        const int channel = queue[(size_t) head++];

        if (channel == to)
            return true;

        // Both kinds of edge, and both of them are "signal leaves this strip":
        // the output is the one that goes on to the master, the sends are the
        // copies that do not.
        const auto visit = [&] (int next)
        {
            if (next == to)
                return true;

            if (juce::isPositiveAndBelow (next, numChannels) && ! visited[(size_t) next])
            {
                visited[(size_t) next] = true;
                queue[(size_t) tail++] = next;
            }

            return false;
        };

        if (visit (channelOutputs[(size_t) channel]))
            return true;

        for (const auto& send : channelSends[(size_t) channel])
            if (send.isLive() && visit (send.destination))
                return true;

        // Phase 100: the key is an edge too, but it is declared on the OTHER
        // end — a strip does not know who listens to it. So the donor's
        // outgoing edges include "every strip whose detector points at me",
        // which is a scan rather than a field. A cycle can hide here (0 feeds 1
        // and 1 is 0's key), which is exactly what a chain-walking cursor used
        // to miss.
        for (int listener = 0; listener < numChannels; ++listener)
            if (channelKeys[(size_t) listener] == channel && visit (listener))
                return true;
    }

    return false;
}

bool MixerModel::wouldLoop (int channel, int destination) const noexcept
{
    if (channel == destination)
        return true;

    if (destination == masterChannel || ! juce::isPositiveAndBelow (destination, numChannels))
        return false;

    // Phase 99: the console is a graph now — a strip can reach another one by
    // an output AND by a send, so asking "does the destination already lead
    // back here" is a search, not a walk down one chain.
    return reaches (destination, channel);
}

void MixerModel::restoreRouting (const std::array<int, numChannels>& outputs)
{
    // Start from a clean console. Every strip to the master is not just the
    // default — it is the only baseline that makes the file's routes mean the
    // same thing they meant when they were saved.
    channelOutputs.fill (masterChannel);

    for (int channel = 0; channel < numChannels; ++channel)
    {
        const int destination = outputs[(size_t) channel];

        if (destination == masterChannel)
            continue;

        // A destination that is not a live strip, or one that would close a
        // loop, is bad data. The strip falls back to the master — the same
        // degradation getChannelOutput() applies on the read side, decided
        // once here instead of per block at render time.
        if (! juce::isPositiveAndBelow (destination, channelCount) || wouldLoop (channel, destination))
            continue;

        channelOutputs[(size_t) channel] = destination;
    }

    notify();
}

//==============================================================================
const MixerModel::Send& MixerModel::getSend (int channel, int index) const noexcept
{
    // A dead index reads as a dead SEND, not as one pointing at the master:
    // `getInserts` hands out the master's chain because writing into it is
    // harmless, but a send that quietly fed a bus would be making sound on
    // behalf of a strip that no longer exists.
    static const Send idle;

    if (! juce::isPositiveAndBelow (channel, numChannels) || ! juce::isPositiveAndBelow (index, numSends))
        return idle;

    const auto& send = channelSends[(size_t) channel][(size_t) index];

    // Read-side safety net, the same one getChannelOutput applies: a strip that
    // shrank away from under a saved send reads as unassigned.
    if (send.destination != noSendDestination
          && ! juce::isPositiveAndBelow (send.destination, channelCount))
        return idle;

    return send;
}

bool MixerModel::setSendDestination (int channel, int index, int destination)
{
    if (! juce::isPositiveAndBelow (channel, channelCount) || ! juce::isPositiveAndBelow (index, numSends))
        return false;

    auto& send = channelSends[(size_t) channel][(size_t) index];

    if (destination == noSendDestination)
    {
        if (send.destination == noSendDestination && ! send.enabled)
            return true;

        send.destination = noSendDestination;
        send.enabled     = false;
        notify();
        return true;
    }

    if (! juce::isPositiveAndBelow (destination, channelCount) || destination == channel)
        return false;

    // Judge against the graph as it WOULD be: this send is being re-pointed, so
    // the edge it has now must not count as the way back to this strip. Clearing
    // it first and putting it back on refusal is the only way to ask the
    // question about the new edge alone.
    const int previous = send.destination;
    send.destination   = noSendDestination;

    const bool loops = reaches (destination, channel);

    if (loops)
    {
        send.destination = previous;
        return false;
    }

    if (previous == destination)
        return true;

    send.destination = destination;

    // A send that has been given a destination but was never switched on stays
    // off: "where would this go" and "is this on" are two decisions, and the
    // second one is the user's.
    notify();
    return true;
}

void MixerModel::setSendLevelDb (int channel, int index, float db)
{
    if (! juce::isPositiveAndBelow (channel, numChannels) || ! juce::isPositiveAndBelow (index, numSends))
        return;

    auto& send = channelSends[(size_t) channel][(size_t) index];
    const float clamped = juce::jlimit (-48.0f, 6.0f, db);

    if (juce::approximatelyEqual (send.levelDb, clamped))
        return;

    send.levelDb = clamped;
    notify();
}

void MixerModel::setSendPreInsert (int channel, int index, bool shouldBePreInsert)
{
    if (! juce::isPositiveAndBelow (channel, numChannels) || ! juce::isPositiveAndBelow (index, numSends))
        return;

    auto& send = channelSends[(size_t) channel][(size_t) index];

    if (send.preInsert == shouldBePreInsert)
        return;

    send.preInsert = shouldBePreInsert;
    notify();
}

bool MixerModel::setSendEnabled (int channel, int index, bool shouldBeEnabled)
{
    if (! juce::isPositiveAndBelow (channel, channelCount) || ! juce::isPositiveAndBelow (index, numSends))
        return false;

    auto& send = channelSends[(size_t) channel][(size_t) index];

    // Refused, not silently ignored: the panel greys the switch on a send with
    // nowhere to go, and this is the same rule, stated where it cannot be
    // skipped.
    if (shouldBeEnabled && send.destination == noSendDestination)
        return false;

    if (send.enabled == shouldBeEnabled)
        return true;

    send.enabled = shouldBeEnabled;
    notify();
    return true;
}

bool MixerModel::isSendTarget (int channel) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, numChannels))
        return false;

    for (int src = 0; src < channelCount; ++src)
        for (int index = 0; index < numSends; ++index)
            if (getSend (src, index).isLive() && getSend (src, index).destination == channel)
                return true;

    return false;
}

//==============================================================================
int MixerModel::getSidechainSource (int channel) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, numChannels))
        return noKeySource;

    const int source = channelKeys[(size_t) channel];

    // Read-side safety net, the same one `getSend` applies: a donor that is no
    // longer a live strip reads as "nothing patched in", because a compressor
    // listening to a bus nobody writes is a compressor that never ducks again.
    if (source != noKeySource
          && (! juce::isPositiveAndBelow (source, channelCount) || source == channel))
        return noKeySource;

    return source;
}

bool MixerModel::setSidechainSource (int channel, int source)
{
    if (! juce::isPositiveAndBelow (channel, channelCount))
        return false;

    if (source == noKeySource)
    {
        if (channelKeys[(size_t) channel] == noKeySource)
            return true;

        channelKeys[(size_t) channel] = noKeySource;
        notify();
        return true;
    }

    // No self-key: a strip whose detector listens to the signal it is about to
    // compress is just a compressor, which is what noKeySource already says.
    //
    // No master either, and for a different reason: the master is DOWNSTREAM of
    // every strip, so its finished signal does not exist yet when a strip's
    // inserts run. Refusing it here is cheaper than a cycle check that would
    // have to special-case it anyway.
    if (! juce::isPositiveAndBelow (source, channelCount) || source == channel)
        return false;

    // Judge the NEW edge alone: the key this strip has now must not count as
    // the way back, or re-pointing a key inside a legal graph would be refused.
    const int previous = channelKeys[(size_t) channel];
    channelKeys[(size_t) channel] = noKeySource;

    const bool loops = reaches (channel, source);   // listener -> donor is a cycle

    channelKeys[(size_t) channel] = previous;

    if (loops)
        return false;

    if (previous == source)
        return true;

    channelKeys[(size_t) channel] = source;
    notify();
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

void MixerModel::sanitiseRouting()
{
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const int destination = channelOutputs[(size_t) channel];

        if (destination != masterChannel
              && (destination == channel || ! juce::isPositiveAndBelow (destination, channelCount)))
            channelOutputs[(size_t) channel] = masterChannel;

        // Phase 99: a send that points at a strip which no longer exists is
        // switched OFF rather than re-pointed. There is no strip it obviously
        // should have meant, and picking one would be this model inventing an
        // intention the user never had.
        for (auto& send : channelSends[(size_t) channel])
        {
            if (send.destination == noSendDestination)
                continue;

            if (send.destination == channel
                  || ! juce::isPositiveAndBelow (send.destination, channelCount))
            {
                send.destination = noSendDestination;
                send.enabled     = false;
            }
        }

        // Phase 100: a key pointing at a strip that no longer exists falls back
        // to the strip's own signal, which is the default and needs no notice.
        // Clearing (rather than re-pointing) is the same call the sends make: no
        // donor is obviously the one that was meant.
        const int keySource = channelKeys[(size_t) channel];

        if (keySource != noKeySource
              && (keySource == channel || ! juce::isPositiveAndBelow (keySource, channelCount)))
            channelKeys[(size_t) channel] = noKeySource;
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
    sanitiseRouting();
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
        channelSends[(size_t) i]   = channelSends[(size_t) i + 1];   // Phase 99
        channelKeys[(size_t) i]    = channelKeys[(size_t) i + 1];     // Phase 100
    }

    inserts[(size_t) last]        = {};
    channelMono[(size_t) last]    = false;
    channelOutputs[(size_t) last] = masterChannel;
    channelSends[(size_t) last]   = {};
    channelKeys[(size_t) last]    = noKeySource;

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

        // Phase 99: a send needs BOTH halves of the same renumber. A send whose
        // destination was the removed strip loses its destination (and goes
        // off); one pointing past the hole follows the strip it meant.
        for (auto& send : channelSends[(size_t) src])
        {
            if (send.destination == noSendDestination)
                continue;

            if (send.destination == channel)
            {
                send.destination = noSendDestination;
                send.enabled     = false;
            }
            else if (send.destination > channel)
            {
                --send.destination;
            }
        }

        // Phase 100: same two halves for a key — the donor follows the strip it
        // meant, and a key whose donor WAS the removed strip falls back to the
        // strip's own signal instead of pointing at whoever moved into the hole.
        int& key = channelKeys[(size_t) src];

        if (key == channel)
            key = noKeySource;
        else if (key > channel)
            --key;
    }

    --channelCount;

    sanitiseRouting();
    notify();
    return true;
}

void MixerModel::setChannelCount (int count)
{
    const int clamped = juce::jlimit (minChannels, maxChannels, count);

    if (clamped == channelCount)
        return;

    channelCount = clamped;
    sanitiseRouting();
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

    // Phase 99: and no sends. Not "sends off with their destinations kept" —
    // a factory console has not been patched yet, so the destinations go too.
    // (Loading a project uses restoreRouting + the file's sends; this function
    // is the "new empty console" path.)
    for (auto& strip : channelSends)
        for (auto& send : strip)
            send = Send {};

    // Phase 25: and no strip is monitoring in mono.
    channelMono.fill (false);

    // Phase 100: and nothing patched into any detector. `noKeySource`, not
    // zero — zero is strip 1, and a factory compressor listens to itself.
    channelKeys.fill (noKeySource);

    for (auto& lane : masterAutomation)
        lane.setEnabled (false);

    notify();
}

void MixerModel::notify()
{
    listeners.call ([] (Listener& l) { l.mixerChanged(); });
}

} // namespace atomic

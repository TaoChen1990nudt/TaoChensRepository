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
    notify();
}

void MixerModel::setInsertParam (int channel, int slot, float p1, float p2)
{
    auto& insert = inserts[(size_t) clampChannel (channel)][(size_t) clampSlot (slot)];

    insert.p1 = juce::jlimit (0.0f, 1.0f, p1);
    insert.p2 = juce::jlimit (0.0f, 1.0f, p2);
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
    if (channelCount <= minChannels)
        return false;

    --channelCount;
    notify();
    return true;
}

void MixerModel::setChannelCount (int count)
{
    const int clamped = juce::jlimit (minChannels, maxChannels, count);

    if (clamped == channelCount)
        return;

    channelCount = clamped;
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
    channelCount  = numChannels;

    for (auto& lane : masterAutomation)
        lane.setEnabled (false);

    notify();
}

void MixerModel::notify()
{
    listeners.call ([] (Listener& l) { l.mixerChanged(); });
}

} // namespace atomic

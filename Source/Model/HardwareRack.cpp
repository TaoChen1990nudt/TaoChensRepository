#include "HardwareRack.h"

#include "Util/Text.h"

namespace atomic
{

namespace
{
    const juce::Colour kSlotColours[] { juce::Colour (0xff7c96f5),   // indigo
                                        juce::Colour (0xffe879a6),   // rose
                                        juce::Colour (0xff5ecf9a),   // green
                                        juce::Colour (0xffe0b13c),   // amber
                                        juce::Colour (0xff9b8cf0) }; // lilac

    constexpr int numSlotColours = (int) (sizeof (kSlotColours) / sizeof (juce::Colour));

    juce::String ordinalName (int ordinal)
    {
        if (ordinal < 26)
            return "Synth " + juce::String::charToString ((juce::juce_wchar) ('A' + ordinal));

        return "Synth " + juce::String (ordinal + 1);
    }
}

//==============================================================================
juce::String waveformName (Waveform waveform)
{
    switch (waveform)
    {
        case Waveform::sine:     return "SINE";
        case Waveform::saw:      return "SAW";
        case Waveform::square:   return "SQR";
        case Waveform::triangle: return "TRI";
    }

    return "SAW";
}

Waveform nextWaveform (Waveform waveform)
{
    switch (waveform)
    {
        case Waveform::sine:     return Waveform::saw;
        case Waveform::saw:      return Waveform::square;
        case Waveform::square:   return Waveform::triangle;
        case Waveform::triangle: return Waveform::sine;
    }

    return Waveform::saw;
}

//==============================================================================
RackModel::RackModel()
{
    // The model's default rack: two synths, so a bare RackModel has something
    // to route to (the self-test builds several and expects Synth A / Synth B).
    //
    // The APP does not open with these: MainComponent::buildBlankProject()
    // installs an empty rack, which is what a new session must look like. The
    // default stays here so the demo fixture and every test keep their
    // two-synth shape without each one re-creating it.
    addSynth();   // Synth A
    addSynth();   // Synth B
}

SynthSlot RackModel::makeDefaultSlot()
{
    SynthSlot slot;
    slot.id       = 1;
    slot.name     = ordinalName (0);
    slot.colour   = kSlotColours[0];
    slot.waveform = Waveform::sine;      // a new project should not bite
    slot.levelDb  = -6.0f;
    slot.mixerChannel = -1;              // no console send until the user asks

    return slot;
}

SynthSlot* RackModel::getSlot (int index) noexcept
{
    if (juce::isPositiveAndBelow (index, (int) slots.size()))
        return &slots[(size_t) index];

    return nullptr;
}

const SynthSlot* RackModel::getSlot (int index) const noexcept
{
    if (juce::isPositiveAndBelow (index, (int) slots.size()))
        return &slots[(size_t) index];

    return nullptr;
}

int RackModel::getSlotIdAt (int index) const
{
    if (auto* slot = getSlot (index))
        return slot->id;

    return -1;
}

SynthSlot* RackModel::findSlot (int slotId) noexcept
{
    for (auto& slot : slots)
        if (slot.id == slotId)
            return &slot;

    return nullptr;
}

const SynthSlot* RackModel::findSlot (int slotId) const noexcept
{
    for (const auto& slot : slots)
        if (slot.id == slotId)
            return &slot;

    return nullptr;
}

int RackModel::indexOfSlot (int slotId) const
{
    for (int i = 0; i < (int) slots.size(); ++i)
        if (slots[(size_t) i].id == slotId)
            return i;

    return -1;
}

int RackModel::getFirstSlotId() const
{
    return slots.empty() ? -1 : slots.front().id;
}

juce::String RackModel::getSlotName (int slotId) const
{
    if (auto* slot = findSlot (slotId))
        return slot->name;

    return juce::String::fromUTF8 ("\xe2\x80\x94");   // em dash
}

juce::Colour RackModel::getSlotColour (int slotId) const
{
    if (auto* slot = findSlot (slotId))
        return slot->colour;

    return juce::Colours::grey;
}

//==============================================================================
int RackModel::addSynth()
{
    SynthSlot slot;
    slot.id       = nextSlotId++;
    slot.name     = ordinalName (nextOrdinal);
    slot.colour   = kSlotColours[nextOrdinal % numSlotColours];
    slot.waveform = (slots.size() % 2 == 0) ? Waveform::saw : Waveform::sine;

    ++nextOrdinal;

    slots.push_back (slot);
    notify();
    return slot.id;
}

int RackModel::addPluginSynth (const juce::PluginDescription& description, int instanceId)
{
    SynthSlot slot;
    slot.id         = nextSlotId++;
    slot.name       = description.name.isNotEmpty() ? description.name
                                                    : utf8 ("VST3 乐器");
    slot.colour     = kSlotColours[nextOrdinal % numSlotColours];
    slot.pluginId   = instanceId;
    slot.pluginName = slot.name;
    slot.plugin     = std::make_shared<const juce::PluginDescription> (description);

    ++nextOrdinal;

    slots.push_back (slot);
    notify();
    return slot.id;
}

void RackModel::removeSlot (int slotId)
{
    const int index = indexOfSlot (slotId);

    if (index < 0)
        return;

    slots.erase (slots.begin() + index);
    notify();
}

//==============================================================================
int RackModel::replaceWithNative (int slotId, Waveform waveform)
{
    auto* slot = findSlot (slotId);

    if (slot == nullptr)
        return 0;

    const int droppedInstance = slot->pluginId;

    slot->pluginId = 0;
    slot->pluginName.clear();
    slot->plugin.reset();
    slot->waveform = waveform;

    // Everything else — id, name, colour, level, pan, both automation lanes —
    // is deliberately left alone. That is the whole point of "replace".
    notify();
    return droppedInstance;
}

int RackModel::replaceWithPlugin (int slotId, const juce::PluginDescription& description,
                                  int instanceId)
{
    auto* slot = findSlot (slotId);

    if (slot == nullptr)
        return 0;

    const int droppedInstance = slot->pluginId;

    slot->pluginId   = instanceId;
    slot->pluginName = description.name;
    slot->plugin     = std::make_shared<const juce::PluginDescription> (description);

    notify();
    return droppedInstance;
}

void RackModel::setMixerChannel (int slotId, int channel)
{
    auto* slot = findSlot (slotId);

    if (slot == nullptr)
        return;

    const int clamped = juce::isPositiveAndBelow (channel, SynthSlot::maxMixerChannels)
                          ? channel : -1;

    if (slot->mixerChannel == clamped)
        return;

    slot->mixerChannel = clamped;
    notify();
}

void RackModel::restoreSlots (std::vector<SynthSlot> restoredSlots)
{
    slots = std::move (restoredSlots);

    // Ids are never reused: the next synth must not collide with a restored one,
    // or every route pointing at the old synth would silently move to the new.
    nextSlotId = 1;

    for (const auto& slot : slots)
        nextSlotId = juce::jmax (nextSlotId, slot.id + 1);

    nextOrdinal = (int) slots.size();
    notify();
}

void RackModel::setMuted (int slotId, bool shouldBeMuted)
{
    if (auto* slot = findSlot (slotId))
    {
        if (slot->muted == shouldBeMuted)
            return;

        slot->muted = shouldBeMuted;
        notify();
    }
}

void RackModel::setLevelDb (int slotId, float newLevelDb)
{
    if (auto* slot = findSlot (slotId))
    {
        slot->levelDb = juce::jlimit (-48.0f, 6.0f, newLevelDb);
        notify();
    }
}

void RackModel::setWaveform (int slotId, Waveform newWaveform)
{
    if (auto* slot = findSlot (slotId))
    {
        slot->waveform = newWaveform;
        notify();
    }
}

//==============================================================================
void RackModel::setPan (int slotId, float pan)
{
    if (auto* slot = findSlot (slotId))
    {
        const float clamped = juce::jlimit (-1.0f, 1.0f, pan);

        if (juce::approximatelyEqual (slot->pan, clamped))
            return;

        slot->pan = clamped;
        notify();
    }
}

void RackModel::setSoloed (int slotId, bool shouldBeSoloed)
{
    if (auto* slot = findSlot (slotId))
    {
        if (slot->soloed == shouldBeSoloed)
            return;

        slot->soloed = shouldBeSoloed;
        notify();
    }
}

void RackModel::setAutomationTarget (int slotId, SlotAutomationTarget target)
{
    if (auto* slot = findSlot (slotId))
    {
        if (slot->automationTarget == target)
            return;

        slot->automationTarget = target;
        notify();
    }
}

void RackModel::setAutomationExpanded (int slotId, bool expanded)
{
    if (auto* slot = findSlot (slotId))
    {
        if (slot->automationExpanded == expanded)
            return;

        slot->automationExpanded = expanded;
        notify();
    }
}

void RackModel::setCollapsed (int slotId, bool shouldCollapse)
{
    if (auto* slot = findSlot (slotId))
    {
        if (slot->collapsed == shouldCollapse)
            return;

        slot->collapsed = shouldCollapse;
        notify();
    }
}

void RackModel::touch()
{
    notify();
}

void RackModel::notify()
{
    listeners.call ([] (Listener& l) { l.rackChanged(); });
}

} // namespace atomic

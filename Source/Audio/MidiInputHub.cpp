#include "MidiInputHub.h"

namespace atomic
{

MidiInputHub::MidiInputHub()
{
    refreshDevices();
}

MidiInputHub::~MidiInputHub()
{
    // Ports must be stopped before this object dies: a MidiInput that is still
    // running owns a thread whose callback points at `this`.
    closeAllPorts();
}

//==============================================================================
void MidiInputHub::setSources (MidiSources nextSources)
{
    if (sources == nextSources)
        return;

    const bool wasExternal = usesExternalController();
    sources = nextSources;

    // The typing keyboard never touches a MIDI device, so it changing is not a
    // reason to close anything. Only the controller switch owns ports.
    if (! usesExternalController())
        closeAllPorts();
    else if (! wasExternal)
        // Turning the switch back on is another chance for a keyboard that
        // was absent the last time. The memory survived the time off.
        openRememberedDevices (rememberedDevices);
}

void MidiInputHub::closeAllPorts()
{
    for (auto& port : ports)
        if (port.input != nullptr)
        {
            port.input->stop();
            port.input.reset();
        }

    ports.clear();
}

//==============================================================================
void MidiInputHub::refreshDevices()
{
    devices.clear();

    std::vector<DeviceInfo> available;

    if (useInjected)
        available = injectedDevices;
    else
        for (const auto& device : juce::MidiInput::getAvailableDevices())
            available.push_back ({ device.name, device.identifier, false });

    for (const auto& device : available)
    {
        DeviceInfo info;
        info.name       = device.name;
        info.identifier = device.identifier;

        for (const auto& port : ports)
            if (port.identifier == info.identifier && port.input != nullptr)
                info.open = true;

        devices.push_back (std::move (info));
    }
}

bool MidiInputHub::setDeviceOpen (const juce::String& identifier, bool shouldOpen)
{
    // A device can only be opened while the controller switch is on. Opening a
    // port with that switch off would start a controller the user has silenced,
    // so the ask is refused rather than obeyed quietly.
    if (shouldOpen && ! usesExternalController())
        return false;

    for (auto it = ports.begin(); it != ports.end(); ++it)
    {
        if (it->identifier != identifier)
            continue;

        if (shouldOpen)
        {
            // An already-open port may have been remembered under an older
            // name; keep the intent in step with what is on screen.
            for (const auto& info : devices)
                if (info.identifier == identifier)
                    remember ({ identifier, info.name });

            return it->input != nullptr;
        }

        it->input->stop();
        it->input.reset();
        ports.erase (it);

        for (auto remembered = rememberedDevices.begin(); remembered != rememberedDevices.end(); ++remembered)
            if (remembered->identifier == identifier)
            {
                rememberedDevices.erase (remembered);
                break;
            }

        syncOpenFlags();
        return false;
    }

    if (! shouldOpen)
    {
        // An untick is the one and only way to forget a keyboard.
        for (auto remembered = rememberedDevices.begin(); remembered != rememberedDevices.end(); ++remembered)
            if (remembered->identifier == identifier)
            {
                rememberedDevices.erase (remembered);
                break;
            }

        return false;
    }

    for (const auto& info : devices)
        if (info.identifier == identifier)
            remember ({ identifier, info.name });

    OpenPort port;
    port.identifier = identifier;
    port.input = juce::MidiInput::openDevice (identifier, this);

    if (port.input == nullptr)
    {
        // The port is gone, or something else is already holding it. The
        // checkbox must show the truth, so the caller is told it failed.
        syncOpenFlags();
        return false;
    }

    port.input->start();
    ports.push_back (std::move (port));

    syncOpenFlags();
    return true;
}

void MidiInputHub::remember (const MidiDeviceBinding& binding)
{
    if (binding.identifier.isEmpty())
        return;

    for (auto& existing : rememberedDevices)
        if (existing.identifier == binding.identifier)
        {
            if (binding.name.isNotEmpty())
                existing.name = binding.name;
            return;
        }

    rememberedDevices.push_back (binding);
}

int MidiInputHub::findDeviceIndex (const std::vector<DeviceInfo>& list,
                                   const MidiDeviceBinding& binding, int skipIndex)
{
    for (int i = 0; i < (int) list.size(); ++i)
        if (i != skipIndex && list[(size_t) i].identifier == binding.identifier)
            return i;

    // Names are the fallback, not the identity: two Keystations can be plugged
    // in at once, so a name match is only used after the id has gone missing.
    if (binding.name.isNotEmpty())
        for (int i = 0; i < (int) list.size(); ++i)
            if (i != skipIndex && list[(size_t) i].name == binding.name)
                return i;

    return -1;
}

int MidiInputHub::openRememberedDevices (std::vector<MidiDeviceBinding> remembered)
{
    // Passed BY VALUE on purpose: the re-enable path calls this with
    // `rememberedDevices` itself, and the first thing this function does is
    // rebuild that member. A const reference here aliases its own clear().
    lastRestoreAttempts.clear();
    rememberedDevices.clear();
    closeAllPorts();
    refreshDevices();

    auto available = devices;
    int opened = 0;

    for (const auto& wanted : remembered)
    {
        const int index = findDeviceIndex (available, wanted);
        const auto binding = index >= 0
                               ? MidiDeviceBinding { available[(size_t) index].identifier,
                                                     available[(size_t) index].name }
                               : wanted;

        lastRestoreAttempts.push_back (binding);

        if (setDeviceOpen (binding.identifier, true))
            ++opened;

        // The port stays remembered whether or not it answered this time.
        remember (binding);

        if (index >= 0)
            available.erase (available.begin() + index);
    }

    syncOpenFlags();
    return opened;
}

void MidiInputHub::useDeviceListForTest (const std::vector<DeviceInfo>& list)
{
    injectedDevices = list;
    useInjected     = true;
    refreshDevices();
}

void MidiInputHub::syncOpenFlags()
{
    // Deliberately not refreshDevices(): this runs from inside a click handler
    // that is iterating the same rows, and re-reading the hardware list would
    // renumber them under its feet. Only the `open` flag can change here.
    for (auto& info : devices)
    {
        info.open = false;

        for (const auto& port : ports)
            if (port.identifier == info.identifier && port.input != nullptr)
                info.open = true;
    }
}

int MidiInputHub::getNumOpenDevices() const
{
    int count = 0;

    for (const auto& port : ports)
        if (port.input != nullptr)
            ++count;

    return count;
}

//==============================================================================
void MidiInputHub::handleIncomingMidiMessage (juce::MidiInput* sourcePort,
                                              const juce::MidiMessage& message)
{
    ++messageCount;

    if (message.isNoteOn())
    {
        lastType.store (1);
        lastNote.store (message.getNoteNumber());
        lastVelocity.store (juce::roundToInt (message.getFloatVelocity() * 127.0f));
    }
    else if (message.isNoteOff())
    {
        lastType.store (2);
        lastNote.store (message.getNoteNumber());
        lastVelocity.store (0);
    }
    else if (message.isController())
    {
        lastType.store (3);
        lastNote.store (message.getControllerNumber());
        lastVelocity.store (message.getControllerValue());

        // Phase 81: the monitor can only say "a knob moved". Recording needs the
        // knob and its value, and this is the only place that has both.
        if (onControl != nullptr)
            onControl (message.getControllerNumber(), message.getControllerValue());
    }
    else
    {
        lastType.store (4);
    }

    lastChannel.store (juce::jmax (1, message.getChannel()));

    if (sourcePort != nullptr)
    {
        const juce::SpinLock::ScopedTryLockType guard (nameLock);

        if (guard.isLocked())
            lastDeviceName = sourcePort->getName();
    }

    // The only line in this class that makes a sound. Everything above is the
    // monitor, and it runs even when the note goes nowhere: "the device is
    // alive" and "the synth is playing" are two different questions, and the
    // settings page has to be able to answer the first one on its own.
    if (onNote == nullptr)
        return;

    if (message.isNoteOn())
        onNote (message.getNoteNumber(), message.getFloatVelocity(), true);
    else if (message.isNoteOff())
        onNote (message.getNoteNumber(), 0.0f, false);
}

MidiInputHub::Monitor MidiInputHub::getMonitor() const
{
    Monitor monitor;

    monitor.count        = messageCount.load();
    monitor.lastType     = lastType.load();
    monitor.lastNote     = lastNote.load();
    monitor.lastVelocity = lastVelocity.load();
    monitor.lastChannel  = lastChannel.load();

    {
        const juce::SpinLock::ScopedTryLockType guard (nameLock);

        if (guard.isLocked())
            monitor.lastDevice = lastDeviceName;
    }

    return monitor;
}

} // namespace atomic

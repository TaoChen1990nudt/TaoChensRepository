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

    sources = nextSources;

    // The typing keyboard never touches a MIDI device, so it changing is not a
    // reason to close anything. Only the controller switch owns ports.
    if (! usesExternalController())
        closeAllPorts();
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

    for (const auto& device : juce::MidiInput::getAvailableDevices())
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
            return it->input != nullptr;

        it->input->stop();
        it->input.reset();
        ports.erase (it);
        syncOpenFlags();
        return false;
    }

    if (! shouldOpen)
        return false;

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

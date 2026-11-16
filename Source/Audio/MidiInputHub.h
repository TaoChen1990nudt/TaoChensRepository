#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "Util/Text.h"

namespace atomic
{

//==============================================================================
/** Phase 39/40: where played notes come from.

    Two switches, not one choice. Phase 39 made this a three-way radio —
    off / controller / typing keyboard — and that was wrong for the way people
    actually work: a controller on the desk and the computer keyboard under the
    other hand are both plugged in at the same time, and choosing between them
    means switching one off to use the other.

    "Nothing" is still sayable: it is both switches off, which is a legitimate
    state for a DAW you want to type in. But Phase 40 turns the typing keyboard
    ON out of the box — the first note after launch should not need a trip to
    the settings window.
*/
struct MidiSources
{
    bool typingKeyboard     = true;
    bool externalController = false;

    bool any() const noexcept { return typingKeyboard || externalController; }

    bool operator== (const MidiSources& other) const noexcept
    {
        return typingKeyboard     == other.typingKeyboard
            && externalController == other.externalController;
    }

    bool operator!= (const MidiSources& other) const noexcept { return ! operator== (other); }
};

/** "电脑键盘 + 外部 MIDI 控制器", "电脑键盘", "关闭". One builder, so the
    settings page and the self-test cannot disagree about what is on. */
inline juce::String midiSourcesName (const MidiSources& sources)
{
    if (sources.typingKeyboard && sources.externalController)
        return utf8 ("电脑键盘 + 外部 MIDI 控制器");

    if (sources.typingKeyboard)
        return utf8 ("电脑键盘");

    if (sources.externalController)
        return utf8 ("外部 MIDI 控制器");

    return utf8 ("关闭");
}

//==============================================================================
/** One remembered external port. `identifier` is the identity, `name` is the
    fallback when a driver change hands the same keyboard a new id. */
struct MidiDeviceBinding
{
    juce::String identifier;
    juce::String name;

    bool operator== (const MidiDeviceBinding& other) const noexcept
    {
        return identifier == other.identifier && name == other.name;
    }

    bool operator!= (const MidiDeviceBinding& other) const noexcept { return ! operator== (other); }
};

//==============================================================================
/**
    Owns the open MIDI inputs and reports what arrives.

    The whole reason this is not inside the settings window any more: a device
    can only be opened once, so the thing that plays notes and the thing that
    draws a checkbox have to be the same object. Phase 8's MIDI page opened
    devices itself, which was fine while it only drew a monitor — the moment
    those messages have to reach the synth, two owners means one of them gets
    silence.

    `onNote` is called from the MIDI thread. It must therefore only touch things
    that are safe there: the host's handler resolves the target synth from an
    atomic and pushes into the engine's lock-protected live queue. Anything that
    reads the model belongs on the message thread.
*/
class MidiInputHub final : private juce::MidiInputCallback
{
public:
    MidiInputHub();
    ~MidiInputHub() override;

    /** One switch may be flipped without touching the other. Turning the
        controller off closes every port it holds, because "off" has to mean off
        — a port that stays open and silently swallows notes is worse than one
        that is shut. Turning anything ON disturbs nothing. */
    void setSources (MidiSources nextSources);
    MidiSources getSources() const noexcept { return sources; }

    bool usesExternalController() const noexcept { return sources.externalController; }

    //-- Devices --------------------------------------------------------------
    struct DeviceInfo
    {
        juce::String name;
        juce::String identifier;
        bool         open = false;
    };

    /** Message thread. Re-reads the device list, keeping ports that are already
        open in the state they are in. */
    void refreshDevices();

    /** Message thread. Returns the state the device ended up in — opening can
        fail, and a checkbox that lies about that is useless. The *wish* is
        remembered even when the open fails: a loose cable must not make the
        app forget the keyboard forever. */
    bool setDeviceOpen (const juce::String& identifier, bool shouldOpen);

    /** Phase 121: re-asks for the remembered ports, by identifier first and
        by name when that identifier has gone away. Returns how many actually
        opened. The memory is kept for every port that failed. */
    int openRememberedDevices (std::vector<MidiDeviceBinding> remembered);

    /** The bindings to save — intent, not a census of what is open now. */
    std::vector<MidiDeviceBinding> getRememberedDevices() const { return rememberedDevices; }

    /** Which bindings the last start tried to reach. A test seam: with no
        hardware on the machine, "we went looking" is the observable fact. */
    std::vector<MidiDeviceBinding> getLastRestoreAttempts() const { return lastRestoreAttempts; }

    /** Exact identifier wins; a different port with the same name is the
        fallback only when the identifier is gone. -1 = not present. */
    static int findDeviceIndex (const std::vector<DeviceInfo>& list,
                                const MidiDeviceBinding& binding, int skipIndex = -1);

    std::vector<DeviceInfo> getDevices() const { return devices; }
    int getNumOpenDevices() const;

    /** Harness only: use this list instead of enumerating hardware, so the
        identity rules can be tested on a machine with no MIDI ports. */
    void useDeviceListForTest (const std::vector<DeviceInfo>& list);

    //-- Monitor --------------------------------------------------------------
    struct Monitor
    {
        int         count        = 0;
        int         lastType     = 0;   // 1 note on, 2 note off, 3 CC, 4 other
        int         lastNote     = 0;
        int         lastVelocity = 0;
        int         lastChannel  = 1;
        juce::String lastDevice;
    };

    /** Safe from either thread: everything in it is atomic except the name,
        which is copied under a try-lock and may come back empty. */
    Monitor getMonitor() const;

    //-- Wiring ---------------------------------------------------------------
    /** pitch, velocity, isNoteOn. MIDI thread. */
    std::function<void (int pitch, float velocity, bool on)> onNote;

    /** Phase 81: controller number, 0..127 value. MIDI thread — the same rule
        as onNote applies: atomics and queues only, never the model.

        Exists because recording automation needs the knob itself, not a
        "something arrived" flag: the monitor's `lastType == 3` says a CC
        happened, which is not enough to draw a curve. */
    std::function<void (int controller, int value)> onControl;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    struct OpenPort
    {
        juce::String                     identifier;
        std::unique_ptr<juce::MidiInput> input;
    };

    void closeAllPorts();

    /** One memory per port. A failed open still leaves the binding in place;
        an explicit untick is the only thing that removes it. */
    void remember (const MidiDeviceBinding& binding);

    /** Re-derives `devices[].open` from the open ports, without re-reading the
        hardware list. Called from inside click handlers that iterate the rows. */
    void syncOpenFlags();

    MidiSources sources;

    std::vector<DeviceInfo>          devices;            // message thread only
    std::vector<DeviceInfo>          injectedDevices;    // harness only
    bool                             useInjected = false;
    std::vector<OpenPort>            ports;              // message thread only
    std::vector<MidiDeviceBinding>   rememberedDevices;  // what the user asked for
    std::vector<MidiDeviceBinding>   lastRestoreAttempts;

    std::atomic<int>  messageCount { 0 };
    std::atomic<int>  lastType     { 0 };
    std::atomic<int>  lastNote     { 0 };
    std::atomic<int>  lastVelocity { 0 };
    std::atomic<int>  lastChannel  { 1 };

    mutable juce::SpinLock nameLock;
    juce::String           lastDeviceName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiInputHub)
};

} // namespace atomic

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
        fail, and a checkbox that lies about that is useless. */
    bool setDeviceOpen (const juce::String& identifier, bool shouldOpen);

    std::vector<DeviceInfo> getDevices() const { return devices; }
    int getNumOpenDevices() const;

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

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    struct OpenPort
    {
        juce::String                     identifier;
        std::unique_ptr<juce::MidiInput> input;
    };

    void closeAllPorts();

    /** Re-derives `devices[].open` from the open ports, without re-reading the
        hardware list. Called from inside click handlers that iterate the rows. */
    void syncOpenFlags();

    MidiSources sources;

    std::vector<DeviceInfo> devices;      // message thread only
    std::vector<OpenPort>   ports;        // message thread only

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

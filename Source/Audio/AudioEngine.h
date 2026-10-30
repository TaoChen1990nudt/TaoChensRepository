#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>

#include "Audio/LoadMeter.h"
#include "Audio/PlaybackEngine.h"
#include "Audio/Recorder.h"
#include "Model/HardwareRack.h"
#include "Model/TempoModel.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"

namespace atomic
{

//==============================================================================
/** Where the transport is. Three states, not two: "stopped" and "paused" both
    mean silence, but only one of them remembers where the playhead was. */
enum class TransportState { stopped, playing, paused };

//==============================================================================
/**
    Owns the audio device and the transport. This is the only class in the
    project that knows a sound card exists.

    It listens to the model and republishes the schedule whenever notes or the
    rack change — which is why editing a note's output while the transport is
    running takes effect on the very next block, without a stop/start.
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          public LayerStack::Listener,
                          public RackModel::Listener,
                          public MixerModel::Listener
{
public:
    AudioEngine (LayerStack& layerStackToUse, RackModel& rackToUse);
    ~AudioEngine() override;

    /** Opens the default output device. Returns false and fills the error text
        when the machine has no usable output — the editor still runs, it just
        cannot be heard. */
    bool start();
    void stop();

    bool isRunning() const noexcept { return deviceOpen; }
    juce::String getStatusText() const;
    juce::String getLastError() const { return lastError; }

    //-- Phase 93: how hard the audio thread is working -----------------------
    /** Everything the two meters need, read once so the bar and the number
        cannot report two different blocks. */
    struct Perf
    {
        /** Smoothed share of the block the callback used: 1.0 is exactly full.
            Measured here rather than read from JUCE so it is the same quantity
            as `peak` — see LoadMeter.h for why JUCE's own number is not. */
        double load = 0.0;

        /** Held peak, in the same units. MAY EXCEED 1.0: a block that took two
            and a half budgets is the one case the user needs to see, and it is
            the one case a clamped meter cannot show. */
        double peak = 0.0;

        /** Callbacks measured since the device started. Distinguishes "0% load"
            from "nobody is measuring". */
        int blocks = 0;

        /** Blocks that missed their deadline, as the device manager counts them
            (`AudioDeviceManager::getXRunCount`): the driver's own tally where
            it has one, JUCE's estimate otherwise. Not counted a second time
            here — our callback runs inside JUCE's timer, so our own tally would
            be a subset of this one, and adding them would double-count every
            overload. */
        int xruns = 0;

        /** 0 when no device is open, which is how the readout knows to say
            "—" instead of lying with a 0%. */
        double sampleRate = 0.0;
        int    blockSize  = 0;
    };

    Perf getPerf() const;

    /** One line for the top bar: "CPU 12% · 峰 41% · 丢音 2". The peak and the
        dropout count only appear when there is something to say. */
    juce::String getPerfText() const;

    /** Test only: the callback needs the device's sample rate to know how long
        a block lasts, and the self-test has no sound card. */
    void setDeviceRateForTest (double sampleRate) noexcept { currentSampleRate = sampleRate; }

    /** Test and snapshot only: feeds one measured block into the meter without a
        device. The callback is the only thing that does this in a real session —
        `--snapshot` never opens a sound card, so every frame it writes would
        otherwise show an empty meter. */
    void simulateBlockForTest (double elapsedMs, int numSamples) noexcept
    {
        loadMeter.pushBlock (elapsedMs,
                             currentSampleRate > 0.0
                               ? (double) numSamples * 1000.0 / currentSampleRate
                               : 0.0);
    }

    //-- Transport ------------------------------------------------------------
    /** Starts, or resumes from wherever the playhead was parked.

        `openDevice` exists so the headless self-test can exercise the state
        machine without grabbing the sound card out from under whatever else
        is using it. */
    void play (bool openDevice = true);

    /** Freezes the playhead where it is. Silence, but no rewind. */
    void pause();

    /** Silences and rewinds to the top. */
    void stopTransport();

    void togglePlayPause();

    TransportState getTransportState() const noexcept
    {
        return (TransportState) transportState.load (std::memory_order_relaxed);
    }

    bool isPlaying() const noexcept { return getTransportState() == TransportState::playing; }
    bool isPaused()  const noexcept { return getTransportState() == TransportState::paused; }

    /** True once the transport has been engaged (playing or paused). The piano
        roll uses this to decide whether notes should show envelope progress:
        after Stop the playhead is at zero and nothing should be "sounding". */
    bool isTransportEngaged() const noexcept { return getTransportState() != TransportState::stopped; }

    void setBpm (double newBpm);
    double getBpm() const noexcept { return bpm; }

    /** The musical position. Authoritative, and unaffected by tempo changes. */
    double getPositionBeats() const { return playback.getPositionBeats(); }
    void   setPositionBeats (double beats);

    double getPositionSeconds() const { return playback.getPosition(); }

    /** Sub-block smoothed position for the UI.

        The audio callback only reports a position once per buffer (~11 ms at
        512 samples), so polling it directly at 60 Hz makes the playhead visibly
        stutter. This extrapolates from the last block's start using the wall
        clock, clamped to one block so a stalled device cannot run away. */
    double getDisplayPositionBeats() const;

    /** Parks the transport at a beat in the paused state, without opening a
        device. Used by --snapshot so the screenshot shows a live transport. */
    void parkAt (double beats);

    PlaybackEngine& getPlayback() noexcept { return playback; }

    /** The device manager behind the transport. The audio settings page edits
        this directly, which is why a change there is audible at once. */
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** Attaches the mixer so inserts and the master fader reach the schedule.
        Optional: without it the engine renders the Phase 6 way (no inserts,
        fixed master trim), which is what the routing tests rely on. */
    void setMixerModel (MixerModel* mixerModel);

    /** Phase 80: attaches the project's tempo automation. Optional exactly like
        the mixer — without it the transport runs at `bpm` and nothing else,
        which is what the routing and envelope tests rely on. */
    void setTempoModel (TempoModel* tempoModelToUse);

    /** The session's VST3 host. Not owned. Optional in exactly the same way as
        the mixer: without it nothing can host a plugin. */
    void setPluginHost (PluginHost* host);

    //-- Phase 81: recording --------------------------------------------------
    /** The input side of the sound card. Phase 81 is the first thing here that
        needed one: until now the device was opened with `initialise (0, 2, ...)`
        — output only — so "record audio" had no wire to stand on.

        False when the device has no usable inputs or the device is offline; the
        caller then records MIDI only rather than pretending a take exists. */
    bool hasAudioInput() const;

    /** Message thread. Clears the tape and arms the callback.

        `maxSeconds` is preallocated up front because the audio thread must not
        allocate: a five-minute ceiling at the device's own sample rate.

        `samplesToSkip` (Phase 88) is the count-in: those input samples are
        dropped, so a take begins on the beat the player was counted in to. */
    bool beginRecording (double maxSeconds = 300.0, int samplesToSkip = 0);

    /** Message thread. Disarms and hands back what was captured. An empty
        buffer means the device had no inputs (or nothing was armed). */
    juce::AudioBuffer<float> endRecording();

    bool isRecording() const noexcept { return recordBuffer.isRecording(); }
    double getRecordSampleRate() const noexcept { return recordBuffer.getSampleRate(); }

    /** Test only: pushes a buffer through the same door the device callback
        uses. The self-test has no microphone, and mocking the device would test
        the mock. */
    void appendRecordedForTest (const juce::AudioBuffer<float>& audio);

    //-- Phase 94: input monitoring and record-side compensation --------------
    /**
        Straight-through monitoring: what arrives on the capture channels is
        summed into the output block.

        Three things are deliberate:

          * **It is added after the transport fade**, exactly like audition,
            live keys and the sample preview. A held microphone is not
            transport audio, so the 8 ms fade that silences a stopped transport
            must not swallow it — and monitoring has to work with the transport
            stopped, which is when most people check their level.
          * **A mono microphone is duplicated**, by the same rule
            `RecordBuffer::append` uses (`min(channel, numInputs - 1)`): one
            voice belongs in both ears, and picking input 0 twice would be a
            different (and wrong) rule for two-channel interfaces.
          * **It is off by default.** A monitoring path that is on without
            being asked for is a feedback loop waiting for the user to point a
            microphone at a speaker.
    */
    void setMonitoringEnabled (bool shouldMonitor) noexcept;
    bool isMonitoringEnabled() const noexcept
    {
        return monitoringEnabled.load (std::memory_order_acquire);
    }

    /** How far the monitoring chain itself delays the signal, in samples.

        Currently nothing can be inserted on that chain, so this is zero — the
        door exists so that the day a plugin lands there, the recording offset
        is already reading from the right place instead of needing a second
        compensation path (@see PdcDelay, which is the same rule one level up:
        one delay table, not two).
    */
    void setMonitorChainLatency (int samples) noexcept;

    /**
        How far a recorded take has to move towards zero to land on the beat the
        player actually played.

        The chain is: the click leaves the DAW, waits out the device's output
        latency, reaches the player's ears, hands answer `output + monitoring`
        later than they heard it, the sound comes back through the microphone and
        the input buffer, and finally reaches the tape. Everything in that
        sentence is latency the take inherits and the player never intended:

            offset = inputLatency + outputLatency + monitorChainLatency

        The monitoring term is not a rounding detail — it is why a linear-phase
        plugin on the monitor path shows up as a late take. The player follows
        what they hear, and what they hear went through that plugin.

        All three terms are unconditional, including the monitoring one. That is
        deliberate: the offset describes the chain the take was played through,
        not the state of a switch at the moment the file lands. `finishRecording`
        closes the monitor path before the take is committed, so making this
        depend on `isMonitoringEnabled()` would zero out precisely the term a
        monitor-path plugin contributed.

        Zero when no device has ever opened, which is also the honest answer:
        with no device there is no chain to compensate. The self-test sets the
        numbers through `setDeviceLatencyForTest`.
    */
    int getRecordOffsetSamples() const noexcept;

    /** The same number in seconds at the rate the callback is running. */
    double getRecordOffsetSeconds() const noexcept;

    /**
        How far a recorded KEYBOARD take has to move towards zero, in samples.

        A different chain, not a fraction of the one above. A microphone take
        pays for the whole round trip: the click leaves late, the sound comes
        back late, the capture buffer adds its own. A keyboard take pays only
        for the click — the player's hand answers what it heard, and the note
        arrives on a MIDI cable with no capture buffer behind it. Subtracting an
        input latency the performance never passed through would put the notes
        *early* by exactly that much.

            offset = outputLatency + monitorChainLatency

        Two numbers instead of one because a take can contain both halves at
        once. Using the audio figure for the notes would slide the two halves of
        one performance apart by the input latency — the one error a recording
        is never allowed to make.
    */
    int getMidiRecordOffsetSamples() const noexcept;

    /** Test only: the self-test has no sound card, so the device's own latency
        numbers have to come from somewhere. Real devices fill these in
        `audioDeviceAboutToStart`. */
    void setDeviceLatencyForTest (int inputSamples, int outputSamples) noexcept;

    //-- Model listeners ------------------------------------------------------
    void layersChanged() override;
    void notesChanged() override;
    void rackChanged() override;
    void mixerChanged() override;

    //-- AudioIODeviceCallback ------------------------------------------------
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void republish();
    void beginFadeOut();

    LayerStack& stack;
    RackModel&  rack;
    MixerModel* mixer = nullptr;
    TempoModel* tempoModel = nullptr;

    juce::AudioDeviceManager deviceManager;
    PlaybackEngine           playback;

    std::atomic<int> transportState { (int) TransportState::stopped };

    /** Ramps the master gain over a few milliseconds on every transport change,
        so pause and stop do not end on a waveform step. */
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterFade;

    /** Where the tail is rendered from after Play is released. In beats, so a
        tempo change during the 8 ms of tail is still consistent. */
    std::atomic<double> fadeOutBeats { 0.0 };

    RecordBuffer recordBuffer;

    //-- Phase 94: monitoring -------------------------------------------------
    /** Off until asked for: an input path that is live without being asked for
        turns a speaker into a microphone's best friend. */
    std::atomic<bool> monitoringEnabled { false };

    /** Samples the monitoring chain adds. Nothing can be inserted there yet
        (P94 is the straight-through path), so this stays zero until a plugin
        chain exists — but the recording offset already reads it, because a
        compensation formula that has to be remembered later is a formula that
        gets forgotten later. */
    std::atomic<int> monitorChainLatency { 0 };

    /** The device's own numbers, taken once in `audioDeviceAboutToStart`.
        Zero until a device opens. */
    std::atomic<int> deviceInputLatency  { 0 };
    std::atomic<int> deviceOutputLatency { 0 };

    /** Phase 93. Fed once per callback by a scope guard, so a block that leaves
        through one of the early returns is still measured — those are the
        cheapest blocks, and leaving them out would bias the average high. */
    LoadMeter loadMeter;

    /** What the device told us in `audioDeviceAboutToStart`. Zero until a device
        opens, which is how the meters know they have nothing to say. */
    double currentSampleRate = 0.0;

    bool        deviceOpen = false;
    double      bpm        = PlaybackEngine::defaultBpm;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace atomic

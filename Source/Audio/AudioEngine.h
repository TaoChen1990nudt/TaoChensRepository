#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>

#include "Audio/PlaybackEngine.h"
#include "Model/HardwareRack.h"
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

    /** The session's VST3 host. Not owned. Optional in exactly the same way as
        the mixer: without it nothing can host a plugin. */
    void setPluginHost (PluginHost* host);

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

    juce::AudioDeviceManager deviceManager;
    PlaybackEngine           playback;

    std::atomic<int> transportState { (int) TransportState::stopped };

    /** Ramps the master gain over a few milliseconds on every transport change,
        so pause and stop do not end on a waveform step. */
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterFade;

    /** Where the tail is rendered from after Play is released. In beats, so a
        tempo change during the 8 ms of tail is still consistent. */
    std::atomic<double> fadeOutBeats { 0.0 };

    bool        deviceOpen = false;
    double      bpm        = PlaybackEngine::defaultBpm;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace atomic

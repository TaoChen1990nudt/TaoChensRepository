#include "AudioEngine.h"

#include "PluginHost.h"

namespace atomic
{

//==============================================================================
AudioEngine::AudioEngine (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    stack.addListener (this);
    rack.addListener (this);

    republish();
}

AudioEngine::~AudioEngine()
{
    stop();
    stack.removeListener (this);
    rack.removeListener (this);
}

//==============================================================================
bool AudioEngine::start()
{
    if (deviceOpen)
        return true;

    // No inputs, stereo out, no saved state, fall back to the default device.
    lastError = deviceManager.initialise (0, 2, nullptr, true);

    if (lastError.isNotEmpty())
    {
        deviceOpen = false;
        return false;
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        lastError = "no output device";
        deviceOpen = false;
        return false;
    }

    deviceManager.addAudioCallback (this);
    deviceOpen = true;
    return true;
}

void AudioEngine::stop()
{
    transportState.store ((int) TransportState::stopped, std::memory_order_relaxed);

    if (deviceOpen)
    {
        deviceManager.removeAudioCallback (this);
        deviceManager.closeAudioDevice();
        deviceOpen = false;
    }

    playback.resetMeters();
}

juce::String AudioEngine::getStatusText() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName() + "  ·  "
                 + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + " kHz";

    return lastError.isNotEmpty() ? lastError : juce::String ("audio offline");
}

//==============================================================================
void AudioEngine::beginFadeOut()
{
    // Remember where the tail has to come from *before* the position moves:
    // Stop rewinds immediately, but the 8 ms of ramp must still be rendered at
    // the moment the user heard.
    fadeOutBeats.store (playback.getPositionBeats(), std::memory_order_relaxed);
}

void AudioEngine::play (bool openDevice)
{
    if (openDevice && ! start())
    {
        transportState.store ((int) TransportState::stopped, std::memory_order_relaxed);
        return;
    }

    // Pressing Play at the very end restarts from the top, like every DAW.
    if (playback.getPositionBeats() >= playback.getLengthBeats() - 1.0e-6)
        playback.setPositionBeats (0.0);

    transportState.store ((int) TransportState::playing, std::memory_order_release);
}

void AudioEngine::pause()
{
    if (isPlaying())
    {
        beginFadeOut();
        transportState.store ((int) TransportState::paused, std::memory_order_release);
    }
}

void AudioEngine::stopTransport()
{
    beginFadeOut();
    transportState.store ((int) TransportState::stopped, std::memory_order_release);
    playback.setPositionBeats (0.0);
}

void AudioEngine::togglePlayPause()
{
    if (isPlaying())
        pause();
    else
        play();
}

void AudioEngine::parkAt (double beats)
{
    playback.setPositionBeats (beats);
    transportState.store ((int) TransportState::paused, std::memory_order_release);
}

void AudioEngine::setPositionBeats (double beats)
{
    playback.setPositionBeats (beats);
}

double AudioEngine::getDisplayPositionBeats() const
{
    if (! isPlaying())
        return playback.getPositionBeats();

    const double blockBeats = playback.getBlockBeats();
    const double spb        = playback.getSecondsPerBeat();

    if (blockBeats <= 0.0 || spb <= 0.0)
        return playback.getPositionBeats();

    const double elapsedMs = juce::Time::getMillisecondCounterHiRes()
                               - playback.getBlockStartTimeMs();

    // Clamp to the block: if the callback stalls (or the device is being
    // reopened) the playhead stops at the block boundary instead of racing
    // ahead of the audio.
    const double advanced = juce::jlimit (0.0, blockBeats, elapsedMs * 0.001 / spb);

    return playback.getBlockStartBeats() + advanced;
}

void AudioEngine::setBpm (double newBpm)
{
    const double clamped = juce::jlimit (30.0, 300.0, newBpm);

    if (juce::approximatelyEqual (clamped, bpm))
        return;

    bpm = clamped;

    // Everything downstream — note start times, loop length, the seconds the
    // playhead derives — is rebuilt from this one number. The transport itself
    // is untouched, because it counts in beats.
    republish();
}

//==============================================================================
void AudioEngine::setMixerModel (MixerModel* mixerModel)
{
    if (mixer == mixerModel)
        return;

    if (mixer != nullptr)
        mixer->removeListener (this);

    mixer = mixerModel;

    if (mixer != nullptr)
        mixer->addListener (this);

    republish();
}

void AudioEngine::layersChanged()   { republish(); }
void AudioEngine::notesChanged()    { republish(); }
void AudioEngine::rackChanged()     { republish(); }
void AudioEngine::mixerChanged()    { republish(); }

void AudioEngine::setPluginHost (PluginHost* host)
{
    playback.setPluginHost (host);

    if (host != nullptr)
        host->setPlayConfig (playback.getSampleRate(), 512);
}

void AudioEngine::republish()
{
    playback.publishSchedule (stack, rack, bpm, PlaybackEngine::timelineBeats, mixer);
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        // prepareToPlay sizes the mixer's scratch bus once, so the callback
        // never allocates, and clears the synth filters' memory.
        playback.prepareToPlay (device->getCurrentSampleRate(),
                                device->getCurrentBufferSizeSamples());

        // Plugins are prepared on the message thread out of the device's
        // numbers, exactly like the mixer's own buffers.
        if (auto* host = playback.getPluginHost())
            host->setPlayConfig (device->getCurrentSampleRate(),
                                 device->getCurrentBufferSizeSamples());

        // 8 ms is short enough to feel instant and long enough that stopping on
        // a square-wave edge does not produce a click.
        masterFade.reset (device->getCurrentSampleRate(), 0.008);
        masterFade.setCurrentAndTargetValue (0.0f);
    }
}

void AudioEngine::audioDeviceStopped()
{
    playback.resetMeters();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (inputChannelData, numInputChannels, context);

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (numOutputChannels <= 0 || numSamples <= 0)
        return;

    const bool running = isPlaying();

    // Phase 19: a preview (drawing or dragging a note) has to be audible with
    // the transport stopped, so "nothing is playing" is no longer a reason to
    // stop rendering. Phase 20 adds the file browser's sample preview, which
    // has the same requirement for the same reason.
    const bool audition = playback.hasAudition();
    const bool preview  = playback.hasSamplePreview();

    masterFade.setTargetValue (running ? 1.0f : 0.0f);

    // Once the release ramp has reached zero there is nothing left to render.
    if (! running && ! audition && ! preview
          && ! masterFade.isSmoothing() && masterFade.getCurrentValue() < 1.0e-4f)
    {
        playback.decayMeters (0.90f);
        return;
    }

    juce::AudioBuffer<float> output (outputChannelData, numOutputChannels, numSamples);

    if (running)
    {
        playback.renderNextBlock (output, numSamples);
    }
    else if (masterFade.isSmoothing() || masterFade.getCurrentValue() > 1.0e-4f)
    {
        // Still playing out the tail of what the user just paused or stopped.
        // renderWindow does not touch the transport, so Stop can rewind to zero
        // immediately while the last few milliseconds ring out cleanly.
        playback.renderWindow (fadeOutBeats.load (std::memory_order_relaxed)
                                 * playback.getSecondsPerBeat(),
                               numSamples, output, nullptr);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float gain = masterFade.getNextValue();

        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                outputChannelData[ch][i] *= gain;
    }

    // Added AFTER the transport fade: the preview is not the transport, so the
    // 8 ms fade-out that silences a stopped transport must not swallow it.
    if (audition)
        playback.renderAudition (output, numSamples);

    if (preview)
        playback.renderSamplePreview (output, numSamples);
}

} // namespace atomic

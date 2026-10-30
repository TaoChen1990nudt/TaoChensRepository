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

    // Phase 81: ask for inputs as well — recording needs a wire, and a device
    // opened with zero inputs can never give one. The fallback matters more
    // than the ask: plenty of machines have no capture channel at all (or have
    // it held by another app), and "no microphone" must not mean "no DAW".
    lastError = deviceManager.initialise (2, 2, nullptr, true);

    if (lastError.isNotEmpty())
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
bool AudioEngine::hasAudioInput() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getActiveInputChannels().countNumberOfSetBits() > 0;

    return false;
}

bool AudioEngine::beginRecording (double maxSeconds, int samplesToSkip)
{
    if (! deviceOpen && ! start())
        return false;

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return false;

    const int inputs = device->getActiveInputChannels().countNumberOfSetBits();

    if (inputs <= 0)
    {
        // No capture channel: arm nothing. A take of digital silence would be
        // indistinguishable from "recorded a quiet room" and would land an empty
        // clip on the timeline.
        recordBuffer.prepare (0.0, 0, 0.0);
        return false;
    }

    // Always two tracks. A mono microphone is duplicated by RecordBuffer::append
    // — the loader downmixes to stereo for the same reason: one shape downstream.
    recordBuffer.prepare (device->getCurrentSampleRate(), 2, maxSeconds);
    recordBuffer.begin (samplesToSkip);
    return recordBuffer.isRecording();
}

juce::AudioBuffer<float> AudioEngine::endRecording()
{
    recordBuffer.finish();
    return recordBuffer.takeAudio();
}

void AudioEngine::appendRecordedForTest (const juce::AudioBuffer<float>& audio)
{
    // The self-test has no microphone. Rather than mocking the device, the tape
    // is fed through the same entry point the callback uses, one block at a time.
    if (! recordBuffer.isRecording() || audio.getNumSamples() <= 0)
        return;

    std::vector<const float*> channels;
    channels.reserve ((size_t) audio.getNumChannels());

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        channels.push_back (audio.getReadPointer (ch));

    recordBuffer.append (channels.data(), (int) channels.size(), audio.getNumSamples());
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
        // Phase 88: pausing during a count-in stops the counting. Resuming
        // starts the take, not the count — the player has already been given it.
        playback.cancelPreRoll();
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

void AudioEngine::setTempoModel (TempoModel* tempoModelToUse)
{
    tempoModel = tempoModelToUse;
    republish();
}

void AudioEngine::republish()
{
    playback.publishSchedule (stack, rack, bpm, PlaybackEngine::timelineBeats, mixer, tempoModel);
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
    juce::ignoreUnused (context);

    // Phase 81: the tape. Runs first and unconditionally (it checks its own
    // armed flag) so a recorded take is aligned with the callbacks that produced
    // it rather than with whenever the render path finished.
    recordBuffer.append (inputChannelData, numInputChannels, numSamples);

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (numOutputChannels <= 0 || numSamples <= 0)
        return;

    // Phase 39: the live queue is drained exactly once per callback, before
    // anything reads it — the transport's hosted-instrument path and the live
    // renderer both need the same note-ons, and a second drain would send each
    // one twice.
    playback.beginLiveBlock();

    // Phase 42: the meters fall on EVERY callback, not only the idle one. The
    // render paths store peaks with `jmax`, so a block with nothing in it used
    // to leave the bar exactly where the last loud block put it — the transport
    // rolling over silence looked identical to a mastered wall of sound. Doing
    // it here, before any render, is also what makes it reach zero: nothing
    // below writes a peak when there is no peak.
    playback.decayMeters (numSamples);

    const bool running = isPlaying();

    // Phase 88: a count-in is not the transport rolling, but it is the one
    // thing that must be heard while it happens. Checked before `running`: when
    // the count-in ends inside a block, `renderPreRoll` renders the rest of that
    // block itself, so the branch must not be taken twice.
    const bool preRolling = playback.isPreRolling();

    // Phase 19: a preview (drawing or dragging a note) has to be audible with
    // the transport stopped, so "nothing is playing" is no longer a reason to
    // stop rendering. Phase 20 adds the file browser's sample preview, which
    // has the same requirement for the same reason.
    const bool audition = playback.hasAudition();
    const bool preview  = playback.hasSamplePreview();

    // Phase 39: nor is a key held down on a MIDI controller or the typing
    // keyboard — playing an instrument with the transport stopped is the whole
    // point of it.
    const bool live = playback.hasLiveNotes();

    if (preRolling)
    {
        // A count-in has no music to fade in, and fading it would swallow the
        // downbeat — the one click that has to be unmissable. Snap the ramp to
        // unity so the first count is at full level, and so the first beat of
        // the take that follows does not arrive through a swelling fade.
        masterFade.setCurrentAndTargetValue (1.0f);
    }
    else
    {
        masterFade.setTargetValue (running ? 1.0f : 0.0f);
    }

    // Once the release ramp has reached zero there is nothing left to render.
    if (! running && ! preRolling && ! audition && ! preview && ! live
          && ! masterFade.isSmoothing() && masterFade.getCurrentValue() < 1.0e-4f)
        return;

    juce::AudioBuffer<float> output (outputChannelData, numOutputChannels, numSamples);

    if (preRolling)
    {
        playback.renderPreRoll (output, numSamples);
    }
    else if (running)
    {
        playback.renderNextBlock (output, numSamples);
    }
    else if (masterFade.isSmoothing() || masterFade.getCurrentValue() > 1.0e-4f)
    {
        // Still playing out the tail of what the user just paused or stopped.
        // renderWindow does not touch the transport, so Stop can rewind to zero
        // immediately while the last few milliseconds ring out cleanly.
        playback.renderWindow (playback.secondsAtBeat (fadeOutBeats.load (std::memory_order_relaxed)),
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

    // Same rule as the preview: a held key is not transport audio, so the
    // fade-out that silences a stopped transport must not swallow it.
    if (live)
        playback.renderLive (output, numSamples);

    if (preview)
        playback.renderSamplePreview (output, numSamples);
}

} // namespace atomic

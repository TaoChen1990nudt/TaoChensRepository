#include "Metronome.h"

namespace atomic
{

namespace
{
    /** One click: a sine at `frequency` under an exponential decay, faded in
        over the first half millisecond.

        The fade-in is not cosmetic. A sine that starts at full amplitude in
        phase zero still starts at zero, but the *decay* envelope starts at one,
        so the first sample is 0 and the second is some fraction of full scale —
        a step, and a step is the broadband part of a click that the ear reads
        as "harsh" rather than "precise". */
    std::vector<float> makeClickShape (double sampleRate, double frequency, float peak)
    {
        const int length = juce::jmax (16, (int) std::lround (sampleRate * Metronome::clickLengthMs * 0.001));
        std::vector<float> shape ((size_t) length, 0.0f);

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double decayTau = 0.0035;                          // 3.5 ms
        const double fadeIn = juce::jmin (0.0005, Metronome::clickLengthMs * 0.001 * 0.25);

        for (int i = 0; i < length; ++i)
        {
            const double t = (double) i / sampleRate;
            const double envelope = std::exp (-t / decayTau);
            const double attack = fadeIn > 0.0 ? juce::jmin (1.0, t / fadeIn) : 1.0;

            shape[(size_t) i] = (float) (std::sin (twoPi * frequency * t) * envelope * attack * peak);
        }

        return shape;
    }
}

//==============================================================================
Metronome::Metronome()
{
    prepare (sampleRate);
}

void Metronome::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    accentShape = makeClickShape (sampleRate, accentFrequency, accentGain);
    beatShape   = makeClickShape (sampleRate, beatFrequency,   beatGain);

    silence();
}

int Metronome::getClickLengthSamples() const noexcept
{
    return (int) accentShape.size();
}

int Metronome::getNumSoundingVoices() const noexcept
{
    int count = 0;

    for (const auto& v : voices)
        if (v.length > 0)
            ++count;

    return count;
}

void Metronome::silence() noexcept
{
    for (auto& v : voices)
        v = Voice();
}

void Metronome::trigger (bool accent, int startAt, float volumeGain) noexcept
{
    Voice* free = nullptr;

    for (auto& v : voices)
    {
        if (v.length <= 0)
        {
            free = &v;
            break;
        }
    }

    if (free == nullptr)
    {
        // All four are sounding, which means the block holds four clicks. Drop
        // the oldest: its decay is the one already past audibility.
        free = &voices[0];

        for (auto& v : voices)
            if (v.position > free->position)
                free = &v;
    }

    const auto& shape = accent ? accentShape : beatShape;

    free->shape    = shape.data();
    free->length   = (int) shape.size();
    free->position = 0;
    free->startAt  = juce::jmax (0, startAt);
    free->gain     = volumeGain;
}

void Metronome::mixVoices (juce::AudioBuffer<float>& output, int numSamples) noexcept
{
    const int channels = output.getNumChannels();

    for (auto& v : voices)
    {
        if (v.length <= 0)
            continue;

        // A voice that has not written anything yet starts at its offset; one
        // that is mid-decay carries on from the top of the block.
        int i = v.position == 0 ? juce::jmax (0, v.startAt) : 0;

        while (i < numSamples && v.position < v.length)
        {
            const float sample = v.shape[v.position] * v.gain;

            for (int ch = 0; ch < channels; ++ch)
                output.getWritePointer (ch)[i] += sample;

            ++i;
            ++v.position;
        }

        if (v.position >= v.length)
            v = Voice();
    }
}

void Metronome::render (juce::AudioBuffer<float>& output, int numSamples,
                        double fromBeat, double beatsPerSample, bool force)
{
    if (numSamples <= 0 || output.getNumSamples() < numSamples || output.getNumChannels() <= 0)
        return;

    const float gain = getVolume();

    // A silent click still has to stop the voices that are already ringing —
    // otherwise turning the volume down mid-decay leaves the tail frozen and it
    // resumes the moment the knob goes back up.
    if (gain <= 0.0f)
    {
        silence();
        return;
    }

    if (! force && ! isEnabled())
        return;

    if (accentShape.empty() || beatsPerSample <= 0.0)
        return;

    const int perBar = getBeatsPerBar();
    const double toBeat = fromBeat + (double) numSamples * beatsPerSample;

    // Whole beats only: the first integer beat at or after the block's start,
    // then every one before its end. `- 1e-9` keeps a beat that lands exactly
    // on a block boundary from being counted twice — once as this block's last
    // beat and once as the next block's first.
    for (int beat = (int) std::ceil (fromBeat - 1.0e-9); (double) beat < toBeat; ++beat)
    {
        const int at = (int) std::floor (((double) beat - fromBeat) / beatsPerSample);

        if (at < 0 || at >= numSamples)
            continue;

        trigger (beat % perBar == 0, at, gain);
    }

    mixVoices (output, numSamples);
}

} // namespace atomic

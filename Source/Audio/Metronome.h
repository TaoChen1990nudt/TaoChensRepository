#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace atomic
{

//==============================================================================
/**
    Phase 88: the click track.

    A metronome is the cheapest thing here with the most exacting requirement:
    the ear forgives a 5 ms late note, and it does not forgive a 5 ms late
    click — a click that drifts against what the player is playing IS the
    mistake. So this is driven by beats and a beats-per-sample figure supplied
    per block, never by a timer, and every click is placed on the sample it
    belongs to rather than at the start of the block it happens to fall in.

    Three decisions worth knowing:

      * **It is mixed into the transport block but never into `renderInto`** —
        so an export, a stem render and an offline analysis cannot contain a
        click, structurally, rather than by remembering to switch it off.

      * **The two sounds are the same shape at different pitches.** 1600 Hz for
        the bar's first beat, 1000 Hz for the rest. A different waveform for the
        accent would be a second thing to keep tuned; a higher pitch is the
        difference every musician already listens for.

      * **Voices, not one voice.** A click is 12 ms and a block can be 46 ms, so
        three beats can land in one callback at a fast tempo. One voice would
        cut the previous click off mid-decay, which sounds like a stumble.

    The click is monitor-only: its level is its own (`setVolume`), not the
    master fader's. Riding the master would mean a mixer change alters what the
    player hears as their own timing reference.
*/
class Metronome
{
public:
    /** 12 ms. Long enough to have a pitch, short enough that at 300 BPM the
        decay is over before the next beat. */
    static constexpr double clickLengthMs = 12.0;

    static constexpr double accentFrequency = 1600.0;
    static constexpr double beatFrequency   = 1000.0;

    /** Relative to `setVolume`: the accent is louder as well as higher, because
        the bar line has to be findable while the player is looking at their
        hands rather than at the screen. */
    static constexpr float accentGain = 0.95f;
    static constexpr float beatGain   = 0.62f;

    Metronome();

    /** Message thread. Builds the two click shapes at this sample rate. */
    void prepare (double newSampleRate);

    double getSampleRate() const noexcept { return sampleRate; }

    void  setEnabled (bool shouldClick) noexcept { enabled.store (shouldClick, std::memory_order_relaxed); }
    bool  isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    /** Message thread. Linear 0..1. */
    void  setVolume (float newVolume) noexcept
    {
        volume.store (juce::jlimit (0.0f, 1.0f, newVolume), std::memory_order_relaxed);
    }
    float getVolume() const noexcept { return volume.load (std::memory_order_relaxed); }

    /** Which beat is accented. Comes from the project's time signature rather
        than being fixed at 4 — a 3/4 click that accents every fourth beat is
        worse than no click at all. */
    void setBeatsPerBar (int beatsPerBarToUse) noexcept
    {
        beatsPerBar.store (juce::jlimit (1, 32, beatsPerBarToUse), std::memory_order_relaxed);
    }
    int getBeatsPerBar() const noexcept { return beatsPerBar.load (std::memory_order_relaxed); }

    /** Audio thread. Adds a click for every whole beat inside the block.

        `fromBeat` is the block's first beat and `beatsPerSample` the tempo at
        that point; a block that spans a tempo change is drawn at the tempo it
        started with, which is at most one block of error in the *gap* between
        two clicks — never in the placement of a click that is already inside
        the block.

        `force` is what a count-in passes: the player asked to be counted in,
        so the click sounds even when the metronome is switched off. */
    void render (juce::AudioBuffer<float>& output, int numSamples,
                 double fromBeat, double beatsPerSample, bool force);

    /** Drops anything still ringing. Called on locate, stop and pre-roll start
        — a click whose tail lands after the transport jumped is a click that
        says the beat is somewhere it is not. */
    void silence() noexcept;

    int getNumSoundingVoices() const noexcept;

    /** Test aid: the pitch of one of the two shapes. */
    static double getClickFrequency (bool accent) noexcept
    {
        return accent ? accentFrequency : beatFrequency;
    }

    int getClickLengthSamples() const noexcept;

private:
    struct Voice
    {
        const float* shape = nullptr;
        int          length = 0;
        int          position = 0;   // samples of the shape already written
        int          startAt = 0;    // block-relative offset, used while position == 0
        float        gain = 0.0f;
    };

    static constexpr int maxVoices = 4;

    void trigger (bool accent, int startAt, float volumeGain) noexcept;
    void mixVoices (juce::AudioBuffer<float>& output, int numSamples) noexcept;

    std::vector<float> accentShape;
    std::vector<float> beatShape;

    std::array<Voice, maxVoices> voices {};

    double sampleRate = 44100.0;

    std::atomic<bool>  enabled { false };
    std::atomic<float> volume  { 0.5f };
    std::atomic<int>   beatsPerBar { 4 };

    JUCE_LEAK_DETECTOR (Metronome)
};

} // namespace atomic

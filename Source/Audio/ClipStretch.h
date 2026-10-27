#pragma once

#include "Model/LayerModel.h"

#include <cmath>

namespace atomic::clipstretch
{

//==============================================================================
/**
    Phase 31: what dragging a block's edge does to the AUDIO.

    Phase 30 made a block a pure window: shortening it cut the picture and the
    sound, and the file played at its own rate. That is one useful answer, not
    the only one — a sampler user expects to be able to choose what "make it
    twice as long" means. Three musical answers, plus the window:

      * `none`     原速窗口 — nothing changes but where the audio stops. The
                   Phase 30 behaviour, and the default, because it is the one
                   that cannot surprise anybody.
      * `resample` 变速变调 — the classic tape answer: half the length, twice
                   the speed AND an octave up. One read rate drives both.
      * `tempo`    只变速不变调 — the whole file squeezed into the block, pitch
                   untouched. This is what "time-stretch" means to a musician.
      * `pitch`    不变速只变调 — the source still passes at its own speed (so
                   the block still shows only the part that fits), but it is
                   transposed by the length ratio.

    `speed` and `pitch` are the only two numbers the renderer needs:
    `speed` is source seconds consumed per output second, `pitch` is the read
    rate inside a grain. Every mode is a point in that plane, which is why the
    engine has one code path and three presets instead of three engines.

    The enum itself lives on the model (`atomic::StretchMode`, next to
    SampleEvent) because the choice is part of the clip and has to survive a
    save. Here it only gets a shorter name.
*/
using Mode = StretchMode;

/** The frozen answer for one clip, computed at publish time.

    `ratio` is natural length / drawn length: 1.0 means "the block is exactly as
    long as the audio it holds", 2.0 means "half as long, so twice as fast".
    Clamped, because a block dragged down to a 1/16 note would otherwise ask for
    a 32x read rate out of a three-minute file. */
struct Playback
{
    Mode   mode  = Mode::none;
    double speed = 1.0;
    double pitch = 1.0;

    bool isIdentity() const noexcept
    {
        return std::abs (speed - 1.0) < 1.0e-12 && std::abs (pitch - 1.0) < 1.0e-12;
    }

    static Playback make (Mode requested, double ratio,
                          int pitchRangeSemitones = SampleEvent::maxPitchRangeSemitones) noexcept
    {
        Playback p;
        p.mode = requested;

        const double r = juce::jlimit (0.125, 8.0, ratio);

        switch (requested)
        {
            case Mode::none:     break;
            case Mode::resample: p.speed = r; p.pitch = r; break;
            case Mode::tempo:    p.speed = r; break;      // pitch stays 1

            case Mode::pitch:
            {
                // Phase 52: the ratio is unbounded, the transposition is not.
                // A range of N semitones means "the read rate may move between
                // 2^(-N/12) and 2^(+N/12) and no further" — past that the block
                // keeps playing at its own rate and the overshoot becomes
                // silence or truncation, which is what "I capped the range"
                // has to mean. The default (36) leaves the old behaviour alone.
                const int    semis = juce::jlimit (SampleEvent::minPitchRangeSemitones,
                                                   SampleEvent::maxPitchRangeSemitones,
                                                   pitchRangeSemitones);
                const double limit = std::pow (2.0, (double) semis / 12.0);

                p.pitch = juce::jlimit (1.0 / limit, limit, r);
                break;
            }
        }

        return p;
    }
};

/** Grain length for the two modes that need one, in seconds.

    50 ms is the usual trade: long enough that a tonal source keeps a stable
    pitch across a grain boundary, short enough that a drum hit does not get
    smeared into the next one. It is a length in SECONDS, not samples, so the
    same setting sounds the same at every sample rate. */
inline constexpr double defaultGrainSeconds = 0.05;

//==============================================================================
/** One interpolated frame, silence outside the buffer.

    Reads by POSITION IN FRAMES, because that is the one unit that means the
    same thing to the source and to the caller: `elapsed` is already seconds,
    and multiplying by the file's own rate is what keeps a 48 kHz file playing
    at its real pitch on a 44.1 kHz device. */
inline void readFrame (const SampleBuffer& buffer, double framePos,
                       float& left, float& right) noexcept
{
    left  = 0.0f;
    right = 0.0f;

    const int frames = buffer.numFrames();

    if (frames < 2 || buffer.channels <= 0 || framePos < 0.0 || framePos >= (double) (frames - 1))
        return;

    const int   i0   = (int) framePos;
    const float frac = (float) (framePos - (double) i0);

    const float* a = buffer.frames.data() + (size_t) i0 * (size_t) buffer.channels;
    const float* b = a + (size_t) buffer.channels;

    left  = a[0] + (b[0] - a[0]) * frac;
    right = buffer.channels > 1 ? a[1] + (b[1] - a[1]) * frac : left;
}

struct Stereo
{
    float left  = 0.0f;
    float right = 0.0f;
};

/** One output frame of a clip.

    Pure function of its arguments — no read head, no phase accumulator, no
    per-voice state. That is deliberate and it is the same rule the synth voices
    follow: `elapsed` is the clip's own output time, so the grain index, the
    grain phase and both read positions are all *derived* rather than
    remembered. A seek, a loop and an offline export therefore cost nothing and
    cannot drift apart.

    Grain overlap-add, the cheap honest version: two read heads half a grain
    apart, crossfaded with a smoothstep whose two weights always sum to 1 (so a
    correlated source does not pulse). No correlation search (WSOLA) — a beat
    of comb filtering on very tonal material is the price of staying stateless,
    and it is the right price here.
*/
inline Stereo read (const SampleBuffer& buffer, const Playback& pb,
                    double startSeconds, double elapsedSeconds,
                    double grainSeconds = defaultGrainSeconds) noexcept
{
    Stereo out;

    const double sr = buffer.sampleRate;

    if (sr <= 0.0 || buffer.numFrames() < 2)
        return out;

    const double startFrame = juce::jmax (0.0, startSeconds) * sr;

    //-- Pure resample ------------------------------------------------------
    // `none` and `resample` both live here: when speed == pitch the two read
    // heads are mathematically the same head, so running the grain path would
    // be a wasted second read and a rounding error. At 1:1 this is bit-exact
    // with the source, which is the identity a Phase 30 block has to keep.
    if (std::abs (pb.speed - pb.pitch) < 1.0e-12)
    {
        readFrame (buffer, startFrame + elapsedSeconds * pb.pitch * sr, out.left, out.right);
        return out;
    }

    //-- Grain path ---------------------------------------------------------
    const double grain  = juce::jmax (0.005, grainSeconds);
    const double hopOut = grain * 0.5;             // output seconds per grain
    const double hopIn  = hopOut * pb.speed;       // source seconds per grain

    const double k = std::floor (elapsedSeconds / hopOut);
    const double p = elapsedSeconds / hopOut - k;  // 0..1 inside this grain

    // The head advances at `pitch` within a grain; the grain's START advances
    // at `speed`. Those are the two knobs, and they are independent.
    const double intoGrain = (elapsedSeconds - k * hopOut) * pb.pitch;

    const double headA = startFrame + (k * hopIn + intoGrain) * sr;

    // Grain zero has no predecessor. Pointing the "previous" head at the same
    // place makes the crossfade a no-op instead of fading in from silence.
    const double headB = k >= 1.0
        ? startFrame + ((k - 1.0) * hopIn
                          + (elapsedSeconds - (k - 1.0) * hopOut) * pb.pitch) * sr
        : headA;

    float lA, rA, lB, rB;
    readFrame (buffer, headA, lA, rA);
    readFrame (buffer, headB, lB, rB);

    // Smoothstep, not a straight line: zero slope at both ends means the weight
    // has no corner where the grains meet, and the sum is 1 either way.
    const double w = p * p * (3.0 - 2.0 * p);

    out.left  = (float) (lB + ((double) lA - (double) lB) * w);
    out.right = (float) (rB + ((double) rA - (double) rB) * w);
    return out;
}

/** Semitones a pitch ratio describes, for the block's badge. */
inline double semitones (double ratio) noexcept
{
    return 12.0 * std::log2 (juce::jmax (1.0e-6, ratio));
}

} // namespace atomic::clipstretch

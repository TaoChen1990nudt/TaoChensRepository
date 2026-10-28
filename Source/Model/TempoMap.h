#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "AutomationLane.h"

namespace atomic
{

//==============================================================================
/** The tempo range the whole app agrees on. One set of numbers, because the
    BPM field's clamp, the curve's endpoints and the self-test's expectations
    must not drift apart. */
namespace tempo
{
    inline constexpr double minBpm     = 30.0;
    inline constexpr double maxBpm     = 300.0;
    inline constexpr double defaultBpm = 120.0;

    /** The curve value that means `defaultBpm`, and the values at 0 and 1.

        Every automation lane is 0..1; tempo has to fit in that box too. The
        mapping is 60 * 4^v, i.e. BPM doubles for every half of the lane:

            v = 0.0 ->  60 BPM
            v = 0.5 -> 120 BPM      (the project default sits at the middle)
            v = 1.0 -> 240 BPM

        NOT a linear 30..300: a user who draws a point halfway up means "half
        the tempo", not "the middle of the ruler". Doubling is also how tempo is
        heard — 60 to 120 is an octave, 60 to 180 is not anything. */
    inline double bpmForValue (float v) noexcept
    {
        return juce::jlimit (minBpm, maxBpm, 60.0 * std::pow (4.0, (double) v));
    }

    /** The inverse, for showing a BPM as a handle position. */
    inline float valueForBpm (double bpm) noexcept
    {
        const double safe = juce::jlimit (minBpm, maxBpm, bpm);
        return (float) juce::jlimit (0.0, 1.0, std::log (safe / 60.0) / std::log (4.0));
    }
}

//==============================================================================
/** One breakpoint of a tempo map: a BPM at an absolute beat.

    Not an AutomationPoint: that one carries 0..1 because it multiplies a fader
    or offsets a pan. Tempo is BPM, and a map that stored 0..1 would push the
    value -> BPM conversion into the audio thread, which is exactly where a
    `std::pow` per block does not belong. The conversion happens ONCE, here, on
    the message thread, when the map is built. */
struct TempoPoint
{
    double beat = 0.0;
    double bpm  = tempo::defaultBpm;
};

//==============================================================================
/**
    Beat <-> second conversion for a project whose tempo may change over time.

    A flat map is the pre-Phase-80 world: `secondsPerBeat` was one number and
    everything multiplied by it. That works exactly as long as the tempo never
    moves, and not one sample longer — the moment a curve drives the transport,
    "seconds at beat 40" is an INTEGRAL of 1/tempo over beats 0..40, and there
    is no single number left to multiply by.

    Within a segment the tempo is linear in the beat, so the integral has a
    closed form:

        bpm(x)  = p0 + q * (x - b0)                    q = (p1 - p0) / (b1 - b0)
        t(x)    = t0 + (60 / q) * ln (bpm(x) / p0)    (q != 0)
        t(x)    = t0 + (x - b0) * 60 / p0             (q == 0, the flat case)

    and the inverse is the same expression solved for x:

        bpm(t)  = p0 * exp (q * (t - t0) / 60)
        x(t)    = b0 + (bpm(t) - p0) / q

    Both directions are used on the audio thread every block (beats in, seconds
    out for the renderer; seconds back in, for the curves), so both are kept
    exact inverses of each other rather than two approximations that drift.

    **Everything is precomputed at build time.** `secondsAt` holds the elapsed
    seconds at each breakpoint, so evaluation is a short scan plus one log — no
    allocation, no vectors grown while a block is being rendered.
*/
class TempoMap
{
public:
    /** A map with one tempo everywhere. This is what a project without tempo
        automation gets, and it is bit-for-bit the old behaviour. */
    static TempoMap flat (double bpm);

    /** The map a project's tempo lane describes.

        `manualBpm` is the number in the BPM field: it is the answer whenever
        the lane is switched off, which is the same rule every other lane
        follows ("automation off restores the control").

        The lane stores 0..1 values; conversion to BPM happens here. The curve's
        breakpoints become the map's breakpoints, so the map is linear in BPM
        where the lane is linear in value — a straight line drawn on screen is a
        (very nearly) exponential accelerando in BPM, which is what the user
        meant by drawing one. */
    static TempoMap fromLane (const AutomationLane& lane, double manualBpm,
                              double lengthBeats);

    TempoMap() { setPoints ({}); }

    /** Sorts, de-duplicates by beat, clamps to >= 0 beat and to the tempo
        range, and makes sure the map starts at beat 0 (tempo is constant before
        the first breakpoint, exactly like a lane's value is). Rebuilds the
        cumulative seconds table. */
    void setPoints (std::vector<TempoPoint> newPoints);

    bool isFlat() const noexcept;

    /** Tempo at a beat. Flat either side of the ends. */
    double bpmAtBeat (double beat) const noexcept;

    double secondsPerBeatAt (double beat) const noexcept
    {
        return 60.0 / bpmAtBeat (beat);
    }

    /** Elapsed seconds from beat 0 to `beat`. The one conversion every
        beat-stamped number (note starts, clip ends, the playhead) goes through. */
    double secondsAtBeat (double beat) const noexcept;

    /** The inverse. `seconds` is measured from beat 0, like the playhead. */
    double beatAtSeconds (double seconds) const noexcept;

    /** How long the span between two beats lasts. Negative spans answer
        negative, so a caller that swapped its ends gets a number it can see. */
    double secondsBetween (double fromBeat, double toBeat) const noexcept
    {
        return secondsAtBeat (toBeat) - secondsAtBeat (fromBeat);
    }

    const std::vector<TempoPoint>& getPoints() const noexcept { return points; }

    /** Shape equality, for the self-test and for "did the map actually change"
        checks that must not republish on a no-op. */
    bool hasSameShapeAs (const TempoMap& other) const noexcept;

private:
    std::vector<TempoPoint> points;
    /** Elapsed seconds at each point. Same length as `points`. */
    std::vector<double> secondsAt;

    JUCE_LEAK_DETECTOR (TempoMap)
};

} // namespace atomic

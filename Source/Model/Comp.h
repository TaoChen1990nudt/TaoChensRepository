#pragma once

#include <juce_core/juce_core.h>
#include <vector>

#include "NoteEnvelope.h"

namespace atomic
{
/** Phase 96: comping — "take 3 for bars 5-8, take 1 for the rest".

    Phase 95 gave several passes one container and one voice. This is the other
    half of the same job: an edit that takes its bars from different passes and
    leaves one performance you can copy, loop and drag like any group.

    Two decisions live here, and only these two:

    **Where the seam is cut.** A splice plays take A up to the seam and take B
    from it. Cut both exactly at the seam and the signal steps from one
    performance to the other between two samples — a click, and the one thing a
    splice is judged on. So the incoming take is grown `crossfadeSeconds` on
    each side and its neighbours keep their own tail/head: over those four
    milliseconds both takes are present and their gains are complementary, which
    is a crossfade rather than a butt join through silence.

    **How much of a block the fade covers.** A block's curves are normalised to
    its own length (that is the whole trick of NoteEnvelope), so four
    milliseconds is not a constant fraction of anything: it is `fadeBeats` which
    becomes a fraction only once a particular block's length is known. That is
    why the fraction, not the beats, is what reaches the curve.

    What is deliberately absent: any notion of which take "owns" a stretch. A
    splice is "replace this range with that take's material", applied to a group
    that happens to be the comp. Repeat it and it still means the same thing,
    in any order — which is what makes three seams in a row behave like three
    edits and not like a script that has to be replayed from the start.
*/
namespace comping
{
    /** Four milliseconds: inside the 3-5 ms window a splice wants, and long
        enough that 48 kHz still has ~190 samples to ramp through. */
    inline constexpr double crossfadeSeconds = 0.004;

    /** The shortest range worth splicing. Same sixteenth-of-a-beat as a loop
        region, for the same reason: below that a "range" is a pixel. */
    inline constexpr double minRangeBeats = 0.25;

    /** A fade never eats more than this share of a block. A splice between two
        neighbours this short is a crossfade of the whole thing either way, and
        a fraction of 1.0 would make the block a triangle that never sounds. */
    inline constexpr float maxFadeFraction = 0.25f;

    struct Range
    {
        double start = 0.0;
        double end   = 0.0;
    };

    inline double lengthOf (const Range& r) noexcept
    {
        return juce::jmax (0.0, r.end - r.start);
    }

    inline bool isUsable (const Range& r) noexcept
    {
        return lengthOf (r) >= minRangeBeats;
    }

    /** Four milliseconds as a number of beats. Zero tempo answers zero rather
        than dividing — an unusable project should lose the fade, not the edit. */
    inline double fadeBeats (double secondsPerBeat) noexcept
    {
        if (secondsPerBeat <= 0.0)
            return 0.0;

        return crossfadeSeconds / secondsPerBeat;
    }

    /** That fade as a fraction of one block's length, clamped. */
    inline float fadeFraction (double fadeBeats_, double memberLengthBeats) noexcept
    {
        if (memberLengthBeats <= 0.0)
            return 0.0f;

        return (float) juce::jlimit (0.0, (double) maxFadeFraction,
                                     fadeBeats_ / memberLengthBeats);
    }

    /** Does a note belong to the incoming side? Whole notes only: a note that
        starts inside the range is taken, one that started earlier and still
        sounds is left where it was. Cutting a note in half because a seam
        fell inside it would turn a legato line into two attacks. */
    inline bool noteJoins (double noteStartBeat, const Range& r) noexcept
    {
        return noteStartBeat >= r.start && noteStartBeat < r.end;
    }

    /** Does a block touch the range at all? Blocks are trimmed rather than
        taken whole, so any overlap is enough. */
    inline bool blockJoins (double startBeat, double lengthBeats, const Range& r) noexcept
    {
        return juce::jmin (startBeat + lengthBeats, r.end) - juce::jmax (startBeat, r.start) > 0.0;
    }

    /** Multiplies `env` by a linear ramp down to zero at one edge.

        Written as a value edit — points are scaled, not replaced — because the
        seam is a *shorter* version of the block's own shape, not a new shape:
        a block whose volume already ramps must keep ramping through the fade.

        The shoulder is inserted as a real point. Without it the ramp would run
        from the block's edge to whatever the curve's next point happens to be
        (often the far end of the block), and four milliseconds would become
        four bars. */
    inline void addEdgeFade (NoteEnvelope& env, bool atHead, float fraction)
    {
        if (fraction <= 0.0f)
            return;

        const float f = juce::jlimit (1.0e-4f, 1.0f, fraction);
        const float shoulderTime = atHead ? f : 1.0f - f;
        const float shoulderValue = env.valueAt (shoulderTime);

        std::vector<EnvelopePoint> points;
        points.reserve (env.getPoints().size() + 2);

        for (const auto& p : env.getPoints())
        {
            // `u` is the distance from the fading edge, in the curve's own
            // normalised time — the same number whichever edge we fade.
            const float u    = atHead ? p.time : 1.0f - p.time;
            const float gain = u >= f ? 1.0f : juce::jmax (0.0f, u / f);

            points.push_back ({ p.time, p.value * gain });
        }

        bool hasShoulder = false;

        for (const auto& p : points)
            if (std::abs (p.time - shoulderTime) < 1.0e-4f)
                hasShoulder = true;

        if (! hasShoulder)
            points.push_back ({ shoulderTime, shoulderValue });

        // loadPoints sorts and re-pins the ends, so a full curve silently loses
        // the shoulder rather than the shape — the fade degrades to "as steep as
        // the curve allows", which is the only honest answer at 24 points.
        env.loadPoints (std::move (points));
        env.setEnabled (true);            // a fade that is bypassed is not a fade
    }
} // namespace compinging
} // namespace atomic

#pragma once

#include <juce_core/juce_core.h>

namespace atomic
{
//==============================================================================
/** Phase 87: the stretch of the timeline the transport repeats.

    Until now "loop" meant one thing and it was not a choice: the transport
    always repeated the whole project, because the wrap point was
    `lengthBeats` and nothing else could name a beat. That is the right
    behaviour for listening to a finished piece and the wrong one for making
    one — "play these eight bars over and over while I fix the drums" is the
    most repeated gesture in the whole activity.

    A region is a pair of beats plus one flag, and the interesting part is that
    every rule below is about ORDER and MINIMUM, not about the two numbers:

    - the start is always before the end, whoever dragged what;
    - the region is never shorter than `minLengthBeats`, so a stray click cannot
      produce a loop that fits between two samples and clicks forever;
    - nothing is ever negative. Beat 0 is the beginning of the timeline; a
      region that ran off the front would be a wrap to a place that has no
      audio and no grid.

    The flag is separate from the numbers on purpose. Turning the loop off must
    not forget where it was — a user who switches it off to listen through and
    then back on expects the same eight bars, not a default.
*/
struct LoopRegion
{
    /** A sixteenth of a beat. Small enough to loop a drum hit, large enough
        that a stray click at the wrong pixel is not a loop. */
    static constexpr double minLengthBeats = 0.25;

    bool   enabled   = false;
    double startBeat = 0.0;
    double endBeat   = 4.0;

    /** The region the transport should honour: enabled AND big enough to be a
        loop rather than a point. `isUsable` is what the renderer asks, so a
        half-dragged region can never stall the playhead. */
    bool isUsable() const noexcept
    {
        return enabled && endBeat - startBeat >= minLengthBeats;
    }

    double lengthBeats() const noexcept { return endBeat - startBeat; }

    bool contains (double beat) const noexcept
    {
        return beat >= startBeat && beat < endBeat;
    }

    /** Where `beat` belongs after the wrap. Outside a usable region this is the
        identity, so callers can apply it unconditionally. */
    double wrapped (double beat) const noexcept
    {
        return isUsable() && beat >= endBeat ? startBeat : beat;
    }

    //-- Editing --------------------------------------------------------------
    /** Drags the left edge. The right edge is pushed along when the start would
        pass it, which is what "the loop has a minimum length" means when the
        pointer is already inside the old region. */
    void setStart (double beat) noexcept
    {
        startBeat = juce::jmax (0.0, beat);

        if (endBeat - startBeat < minLengthBeats)
            endBeat = startBeat + minLengthBeats;
    }

    void setEnd (double beat) noexcept
    {
        endBeat = juce::jmax (0.0, beat);

        if (endBeat - startBeat < minLengthBeats)
            startBeat = juce::jmax (0.0, endBeat - minLengthBeats);
    }

    /** A drag from one beat to another. The direction of the drag says nothing
        about which end is which — a user who drags right-to-left means the same
        region as one who drags left-to-right. */
    void setRange (double a, double b) noexcept
    {
        const auto lo = juce::jmax (0.0, juce::jmin (a, b));
        const auto hi = juce::jmax (lo + minLengthBeats, juce::jmax (a, b));

        startBeat = lo;
        endBeat   = hi;
    }

    /** Slides the whole region, keeping its length. It may not run off the
        front; it may run off the end (a loop over silence is a legal way to
        count yourself in). */
    void moveBy (double delta) noexcept
    {
        const auto shift = juce::jmax (delta, -startBeat);

        startBeat += shift;
        endBeat   += shift;
    }

    /** The region a double click asks for: one bar, starting where the pointer
        is, snapped by the caller if it wants snapping. */
    static LoopRegion around (double at, double barBeats = 4.0)
    {
        LoopRegion region;
        region.enabled = true;
        region.startBeat = juce::jmax (0.0, at);
        region.endBeat   = region.startBeat + juce::jmax (minLengthBeats, barBeats);
        return region;
    }

    bool operator== (const LoopRegion& other) const noexcept
    {
        return enabled == other.enabled
                 && startBeat == other.startBeat
                 && endBeat == other.endBeat;
    }

    bool operator!= (const LoopRegion& other) const noexcept { return ! operator== (other); }
};

} // namespace atomic

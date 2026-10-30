#pragma once

#include <cmath>

namespace atomic
{
/** Phase 90: putting notes back on the grid they were played against.

    One pure function, because "quantise" is a claim about WHERE a note goes and
    that claim has to be the same one every time it is made. The menu, the
    self-test and any future "quantise on record" all call this; there is no
    second implementation to drift.

    Strength is the part that makes this usable on recorded material. Snapping a
    played phrase 100% to the grid replaces the performance with the grid — you
    get a drum machine where a drummer used to be. 60% moves the late note most
    of the way and the early one most of the way back, which reads as "tighter"
    instead of "robot". Full strength is therefore a special case of the general
    rule rather than the only case. */
namespace quantize
{
    /** The strength the menu starts at. 100% because a first use should produce
        the thing the word means; the slider exists for the second use. */
    inline constexpr double defaultStrength = 1.0;

    /** The nearest point on the grid, as a beat.

        `std::round` and not `std::floor`: a note played 6 ms before the beat is
        late by nothing anyone can hear, and flooring would push it a whole
        step forward. Half-way rounds up, which is the only choice that is
        symmetric when a phrase sits exactly between two grid points.

        A grid of zero (or less) is "no grid", and the answer is the input —
        bit for bit, because a caller that forgot to set one should get its
        notes back untouched rather than all stacked on beat 0. */
    inline double nearestGridPoint (double beat, double grid) noexcept
    {
        if (! (grid > 0.0))
            return beat;

        return std::round (beat / grid) * grid;
    }

    /** Where `beat` ends up at the given strength: 0 = untouched, 1 = exactly on
        the grid, 0.6 = 60% of the way there.

        Strength above 1 is clamped rather than extrapolated — overshooting the
        grid on purpose is not a thing anyone wants, and a strength of 2 through
        the same formula would move a note PAST the grid point to the next one,
        which looks like quantise working backwards. */
    inline double apply (double beat, double grid, double strength) noexcept
    {
        if (! (grid > 0.0) || ! (strength > 0.0))
            return beat;

        // Full strength is the early return, not the general formula with 1.0
        // plugged in — and that is also what clamps a strength above 1: a blend
        // factor of 2 would carry the note PAST the grid point it was heading
        // for, which reads as quantise working backwards.
        if (strength >= 1.0)
            return nearestGridPoint (beat, grid);

        return beat + (nearestGridPoint (beat, grid) - beat) * strength;
    }
} // namespace quantize
} // namespace atomic

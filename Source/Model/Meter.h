#pragma once

#include <algorithm>
#include <cmath>

namespace atomic
{
/** Phase 91: the project's metre, and the bar arithmetic that follows from it.

    Until this existed the metre was one `constexpr double beatsPerBar = 4.0` in
    `Util/SharedRules.h`, read by the ruler, by "复制到后一小节" and by the
    metronome's accent. That comment even said what to do: "there is no
    time-signature setting yet; when there is, this is the one line that becomes
    a value". It is now a value — one value, owned by the project, written to the
    document and pushed to the views, exactly like the tempo.

    What it is NOT is general. A bar is `beatsPerBar` QUARTER notes: 2/4 … 12/4,
    which covers every metre whose pulse is the beat the timeline already
    counts. Six-eight is a different question — it needs a pulse of its own
    before it can be a different number, and pretending `beatsPerBar = 3` is
    6/8 would give the metronome three clicks where the music has two.
*/
namespace meter
{
    inline constexpr int minBeatsPerBar = 1;
    inline constexpr int maxBeatsPerBar = 16;
    inline constexpr int defaultBeatsPerBar = 4;

    /** Anything a document, a menu or a test hands over becomes a usable metre.
        One function, because "how many beats in a bar" is asked in four places
        and a zero bar would divide the whole timeline by nothing. */
    inline int clamp (int beats) noexcept
    {
        return std::max (minBeatsPerBar, std::min (maxBeatsPerBar, beats));
    }

    /** The first bar line strictly after `beat`.

        Snapped to the grid first, because "the next bar" means the bar line the
        ruler draws, not "beat + N" — those differ the moment the source starts
        off the grid. */
    inline double nextBarLineAfter (double beat, double grid, int beatsPerBar) noexcept
    {
        const double bar = (double) clamp (beatsPerBar);
        const double g   = grid > 0.0 ? grid : 1.0e-6;
        const double snapped = std::round (beat / g) * g;

        return (std::floor (snapped / bar) + 1.0) * bar;
    }

    /** The first bar line at or after `beat` — "clear of here, but not a bar
        later than it has to be".

        The `- 1e-9` is the same trick the metronome uses on its downbeats: a
        beat that is already ON a bar line must answer that line, not the next
        one, and floating point will not put it there exactly. */
    inline double barLineAtOrAfter (double beat, int beatsPerBar) noexcept
    {
        const double bar = (double) clamp (beatsPerBar);

        return std::ceil (beat / bar - 1.0e-9) * bar;
    }

    /** The bar number a human would read off the ruler for this beat: 1-based. */
    inline int barNumberAt (double beat, int beatsPerBar) noexcept
    {
        const double bar = (double) clamp (beatsPerBar);

        return (int) std::floor (beat / bar + 1.0e-9) + 1;
    }

    /** A position in the units a musician reads: bar.beat.tick, both 1-based,
        four ticks to the beat.

        Phase 93: this arithmetic used to be four lines in `MainComponent` with a
        literal 4.0 in them, so a 3/4 song counted its bars in fours for two
        phases — P91 turned the metre into a project value and missed the one
        reader that puts it into words. It lives here because the other three
        readers of that number live here, and because a rule with a `constexpr`
        copy in it is a rule with two implementations.

        Ticks are quarter-beats, matching the ruler's own subdivision; the
        rounding is asked for with the same `+ 1e-9` the other functions use,
        since 2.9999999 beats is beat 3 and not beat 2. */
    struct Position
    {
        int bar  = 1;
        int beat = 1;
        int tick = 1;
    };

    inline Position positionAt (double beats, int beatsPerBar) noexcept
    {
        const double bar  = (double) clamp (beatsPerBar);
        const double safe = std::max (0.0, beats);

        Position p;
        p.bar = (int) std::floor (safe / bar) + 1;

        const double inBar = safe - (double) (p.bar - 1) * bar;

        p.beat = (int) std::floor (inBar + 1.0e-9) + 1;

        const double fraction = std::max (0.0, inBar - std::floor (inBar));
        p.tick = (int) std::floor (fraction * 4.0) + 1;

        return p;
    }
} // namespace meter
} // namespace atomic

#pragma once

#include <algorithm>
#include <vector>

namespace atomic
{
/** Phase 95: when does a second pass join the first, instead of piling on it?

    Before this, recording the same eight bars twice produced two groups that
    both sounded — a chord of two performances. The fix is a container: several
    groups that are takes of one thing, of which exactly one is the one you
    hear. That container is DERIVED (a shared `takeSetId` on the groups, the
    same way an appearance is derived from its members), so there is no set
    entity to keep in step.

    This header owns the single question the container cannot answer by itself:
    when a new take arrives, does it belong to the set that is already there?

    The rule is coverage measured against the LONGER of the two spans, and the
    first version of this file got it wrong in a way worth recording: measured
    against the SHORTER, a two-bar punch inside an eight-bar set scores 2/2 =
    100% and joins — which silences the other six bars. The shorter span is
    exactly the thing that must not be allowed to speak for the pair.

    Against the longer one, every case lands where a musician would put it:

      * 8 bars over 8 bars        → 100%, joins
      * 8 bars over 8.5 bars      → 94%, joins (they played slightly long)
      * 7.2 bars over 8 bars      → 90%, joins (just under, still the whole thing)
      * 2 bars inside 8 bars      → 25%, does NOT join

    What it deliberately refuses: that last line. "Replace bars 3-4 of the comp"
    is comping — a splice with a crossfade at each seam — and a rule that
    quietly silenced the other six bars would be a rule that deletes work.
    Until comping exists, a partial pass is simply its own take, audible and
    harmless.
*/
namespace takes
{
    /** Nine tenths of the longer span. High enough that a punch-in never
        qualifies, low enough that playing a few milliseconds past the end of
        the region does. */
    inline constexpr double joinRatio = 0.9;

    struct Span
    {
        double start = 0.0;
        double end   = 0.0;
    };

    inline double lengthOf (const Span& s) noexcept
    {
        return std::max (0.0, s.end - s.start);
    }

    inline bool isEmpty (const Span& s) noexcept
    {
        return lengthOf (s) <= 0.0;
    }

    /** How much of the timeline the two share, in beats. */
    inline double overlapOf (const Span& a, const Span& b) noexcept
    {
        return std::max (0.0, std::min (a.end, b.end) - std::max (a.start, b.start));
    }

    /** Overlap as a fraction of the LONGER span, 0 … 1.

        A zero-length span answers 0 rather than dividing: a take that captured
        silence has no extent to be covered by, and "infinitely covered" is not
        an answer anything downstream could use. */
    inline double coverage (const Span& a, const Span& b) noexcept
    {
        const double longer = std::max (lengthOf (a), lengthOf (b));

        if (longer <= 0.0)
            return 0.0;

        return overlapOf (a, b) / longer;
    }

    inline bool shouldJoin (const Span& existing, const Span& incoming,
                            double ratio = joinRatio) noexcept
    {
        if (isEmpty (existing) || isEmpty (incoming))
            return false;

        return coverage (existing, incoming) >= ratio;
    }

    /** Which existing span the incoming take belongs to, or -1.

        The best-covered one, not the first one that qualifies: a layer with two
        overlapping sets (a doubled chorus, say) would otherwise send every new
        take to whichever was created first. Ties keep the earlier entry, which
        makes the answer stable across runs — a join that depended on vector
        order would make a test that passes today fail tomorrow. */
    inline int bestCandidate (const std::vector<Span>& existing, const Span& incoming,
                              double ratio = joinRatio)
    {
        int    best      = -1;
        double bestScore = 0.0;

        for (int i = 0; i < (int) existing.size(); ++i)
        {
            if (! shouldJoin (existing[(size_t) i], incoming, ratio))
                continue;

            const double score = coverage (existing[(size_t) i], incoming);

            if (score > bestScore)
            {
                bestScore = score;
                best      = i;
            }
        }

        return best;
    }
} // namespace takes
} // namespace atomic

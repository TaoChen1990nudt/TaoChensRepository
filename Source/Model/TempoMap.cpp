#include "TempoMap.h"

#include <algorithm>
#include <cmath>

namespace atomic
{

namespace
{
    /** Below this, a segment's tempo is "not moving" and the log/exp form is
        numerically worse than the linear one. 1e-9 BPM per beat is far below
        anything a curve can express (60 BPM over one beat is q = 60). */
    constexpr double flatSlope = 1.0e-9;

    /** A segment shorter than this has no integral to speak of — two points on
        the same beat, which setPoints already collapses. Belt and braces so a
        degenerate map can never divide by zero on the audio thread. */
    constexpr double tinySpan = 1.0e-12;
}

//==============================================================================
TempoMap TempoMap::flat (double bpm)
{
    TempoMap map;
    map.setPoints ({ { 0.0, juce::jlimit (tempo::minBpm, tempo::maxBpm, bpm) } });
    return map;
}

TempoMap TempoMap::fromLane (const AutomationLane& lane, double manualBpm, double lengthBeats)
{
    juce::ignoreUnused (lengthBeats);

    // "Automation off restores the control" — the same law every other lane
    // follows. A lane that is on but holds a single point is a flat line too,
    // and resetFlat always leaves two, so this is the whole test.
    if (! lane.isEnabled() || lane.getPoints().size() < 2)
        return flat (manualBpm);

    std::vector<TempoPoint> pts;
    pts.reserve (lane.getPoints().size());

    for (const auto& point : lane.getPoints())
        pts.push_back ({ point.beat, tempo::bpmForValue (point.value) });

    if (pts.empty())
        return flat (manualBpm);

    TempoMap map;
    map.setPoints (std::move (pts));
    return map;
}

//==============================================================================
void TempoMap::setPoints (std::vector<TempoPoint> newPoints)
{
    points.clear();
    secondsAt.clear();

    for (auto point : newPoints)
    {
        if (! std::isfinite (point.beat) || ! std::isfinite (point.bpm))
            continue;

        point.beat = juce::jmax (0.0, point.beat);
        point.bpm  = juce::jlimit (tempo::minBpm, tempo::maxBpm, point.bpm);
        points.push_back (point);
    }

    std::stable_sort (points.begin(), points.end(),
                      [] (const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });

    // Two points on the same beat describe a zero-length segment. There is no
    // integral across it, and keeping both would make the beat ambiguous — the
    // LATER one wins, because that is the tempo the curve holds from that beat
    // on, and it is the reading a user dragging one point onto another expects.
    for (int i = (int) points.size() - 1; i > 0; --i)
        if (juce::approximatelyEqual (points[(size_t) i].beat, points[(size_t) i - 1].beat))
            points.erase (points.begin() + (i - 1));

    if (points.empty())
        points.push_back ({ 0.0, tempo::defaultBpm });

    // Tempo is constant before the first breakpoint (a lane's value is too), so
    // the map always starts at beat 0. Without this, secondsAtBeat(0) would
    // extrapolate backwards and every note before the first point would land at
    // a negative second.
    if (points.front().beat > 0.0)
        points.insert (points.begin(), { 0.0, points.front().bpm });

    secondsAt.assign (points.size(), 0.0);

    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto& a    = points[i - 1];
        const auto& b    = points[i];
        const double span = b.beat - a.beat;
        const double q    = (b.bpm - a.bpm) / span;

        secondsAt[i] = secondsAt[i - 1] + (std::abs (q) < flatSlope
                                             ? span * 60.0 / a.bpm
                                             : (60.0 / q) * std::log (b.bpm / a.bpm));
    }
}

bool TempoMap::isFlat() const noexcept
{
    for (size_t i = 1; i < points.size(); ++i)
        if (std::abs (points[i].bpm - points.front().bpm) > 1.0e-9)
            return false;

    return true;
}

bool TempoMap::hasSameShapeAs (const TempoMap& other) const noexcept
{
    if (points.size() != other.points.size())
        return false;

    for (size_t i = 0; i < points.size(); ++i)
        if (! juce::approximatelyEqual (points[i].beat, other.points[i].beat)
            || ! juce::approximatelyEqual (points[i].bpm, other.points[i].bpm))
            return false;

    return true;
}

//==============================================================================
double TempoMap::bpmAtBeat (double beat) const noexcept
{
    if (points.empty())
        return tempo::defaultBpm;

    if (beat <= points.front().beat)
        return points.front().bpm;

    for (size_t i = 1; i < points.size(); ++i)
    {
        if (beat <= points[i].beat)
        {
            const auto& a    = points[i - 1];
            const auto& b    = points[i];
            const double span = b.beat - a.beat;

            if (span <= tinySpan)
                return b.bpm;

            return a.bpm + (b.bpm - a.bpm) * ((beat - a.beat) / span);
        }
    }

    return points.back().bpm;
}

//==============================================================================
double TempoMap::secondsAtBeat (double beat) const noexcept
{
    if (points.empty())
        return beat * 60.0 / tempo::defaultBpm;

    if (beat <= points.front().beat)
    {
        // points.front().beat is always 0 after setPoints, so this branch only
        // serves a negative beat (a caller that subtracted from the start).
        const auto& first = points.front();
        return secondsAt.front() + (beat - first.beat) * 60.0 / first.bpm;
    }

    for (size_t i = 1; i < points.size(); ++i)
    {
        if (beat <= points[i].beat)
        {
            const auto& a    = points[i - 1];
            const auto& b    = points[i];
            const double span = b.beat - a.beat;

            if (span <= tinySpan)
                return secondsAt[i];

            const double q  = (b.bpm - a.bpm) / span;
            const double dx = beat - a.beat;

            if (std::abs (q) < flatSlope)
                return secondsAt[i - 1] + dx * 60.0 / a.bpm;

            // bpm is linear in beats and therefore exponential in seconds, so the
            // integral of 60/bpm across the segment is a log of the bpm ratio.
            return secondsAt[i - 1] + (60.0 / q) * std::log ((a.bpm + q * dx) / a.bpm);
        }
    }

    // Past the last point the tempo holds, so the tail is linear in seconds.
    const auto& last = points.back();
    return secondsAt.back() + (beat - last.beat) * 60.0 / last.bpm;
}

double TempoMap::beatAtSeconds (double seconds) const noexcept
{
    if (points.empty())
        return seconds * tempo::defaultBpm / 60.0;

    if (seconds <= 0.0)
    {
        const auto& first = points.front();
        return first.beat + seconds * first.bpm / 60.0;
    }

    for (size_t i = 1; i < points.size(); ++i)
    {
        if (seconds <= secondsAt[i])
        {
            const auto& a    = points[i - 1];
            const auto& b    = points[i];
            const double span = b.beat - a.beat;

            if (span <= tinySpan)
                return b.beat;

            const double q  = (b.bpm - a.bpm) / span;
            const double dt = seconds - secondsAt[i - 1];

            if (std::abs (q) < flatSlope)
                return a.beat + dt * a.bpm / 60.0;

            // The exact inverse of the log above: bpm grows exponentially in
            // seconds when it grows linearly in beats.
            const double bpmHere = a.bpm * std::exp (q * dt / 60.0);
            return a.beat + (bpmHere - a.bpm) / q;
        }
    }

    const auto& last = points.back();
    return last.beat + (seconds - secondsAt.back()) * last.bpm / 60.0;
}

} // namespace atomic

#include "NoteEnvelope.h"

#include <algorithm>
#include <cmath>

namespace atomic
{

namespace
{
    constexpr float kMinGap = 0.005f;

    float clamp01 (float v) noexcept { return juce::jlimit (0.0f, 1.0f, v); }
}

//==============================================================================
NoteEnvelope::NoteEnvelope()
{
    // Default automation for a brand new note: a plain ADSR.
    points = { { 0.00f, 0.00f },
               { 0.08f, 1.00f },
               { 0.34f, 0.62f },
               { 0.82f, 0.58f },
               { 1.00f, 0.00f } };
}

NoteEnvelope NoteEnvelope::makeADSR()
{
    NoteEnvelope env (EmptyTag{});
    env.points = { { 0.00f, 0.00f },    // key down
                   { 0.08f, 1.00f },    // attack peak
                   { 0.34f, 0.62f },    // decay to sustain
                   { 0.82f, 0.58f },    // sustain hold
                   { 1.00f, 0.00f } };  // release
    return env;
}

NoteEnvelope NoteEnvelope::makeFadeOut()
{
    NoteEnvelope env (EmptyTag{});
    env.points = { { 0.0f, 1.0f }, { 1.0f, 0.0f } };
    return env;
}

NoteEnvelope NoteEnvelope::makeFlat()
{
    NoteEnvelope env (EmptyTag{});
    env.points = { { 0.0f, 0.7f }, { 1.0f, 0.7f } };
    return env;
}

void NoteEnvelope::resetToDefault()
{
    *this = makeADSR();
}

void NoteEnvelope::loadPoints (std::vector<EnvelopePoint> restoredPoints)
{
    if (restoredPoints.size() > (size_t) maxPoints)
        restoredPoints.resize ((size_t) maxPoints);

    // A curve needs a head and a tail. A one-point file is repaired into a flat
    // line rather than rejected: the note still plays, it just has no shape.
    if (restoredPoints.size() < 2)
    {
        const float value = restoredPoints.empty() ? 1.0f : clamp01 (restoredPoints.front().value);
        points = { { 0.0f, value }, { 1.0f, value } };
        return;
    }

    for (auto& p : restoredPoints)
    {
        p.time  = clamp01 (p.time);
        p.value = clamp01 (p.value);
    }

    points = std::move (restoredPoints);
    sortPoints();

    points.front().time = 0.0f;
    points.back().time  = 1.0f;
}

//==============================================================================
float NoteEnvelope::valueAt (float t) const noexcept
{
    if (points.empty())
        return 1.0f;

    if (t <= points.front().time)
        return points.front().value;

    if (t >= points.back().time)
        return points.back().value;

    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto& b = points[i];

        if (t <= b.time)
        {
            const auto& a = points[i - 1];
            const float span = b.time - a.time;

            if (span <= 0.0f)
                return b.value;

            const float k = (t - a.time) / span;
            return a.value + (b.value - a.value) * k;
        }
    }

    return points.back().value;
}

//==============================================================================
int NoteEnvelope::addPoint (float time, float value)
{
    if ((int) points.size() >= maxPoints)
        return -1;

    EnvelopePoint p { clamp01 (time), clamp01 (value) };

    auto insertAt = std::lower_bound (points.begin(), points.end(), p,
                                      [] (const EnvelopePoint& a, const EnvelopePoint& b)
                                      { return a.time < b.time; });

    const int index = (int) std::distance (points.begin(), insertAt);
    points.insert (insertAt, p);

    // First / last points own the endpoints of the note.
    if (index == 0)             points.front().time = 0.0f;
    if (index == (int) points.size() - 1) points.back().time = 1.0f;

    constrainPoint (index);
    return index;
}

void NoteEnvelope::removePoint (int index)
{
    if (points.size() <= 2)
        return;

    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return;

    points.erase (points.begin() + index);

    if (! points.empty())
    {
        points.front().time = 0.0f;
        points.back().time = 1.0f;
    }
}

void NoteEnvelope::movePoint (int index, float time, float value)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return;

    points[(size_t) index].time  = clamp01 (time);
    points[(size_t) index].value = clamp01 (value);
    constrainPoint (index);
}

void NoteEnvelope::constrainPoint (int index)
{
    if (points.empty())
        return;

    index = juce::jlimit (0, (int) points.size() - 1, index);

    auto& p = points[(size_t) index];

    if (index == 0)
    {
        p.time = 0.0f;
    }
    else if (index == (int) points.size() - 1)
    {
        p.time = 1.0f;
    }
    else
    {
        const float lo = points[(size_t) index - 1].time + kMinGap;
        const float hi = points[(size_t) index + 1].time - kMinGap;
        p.time = (lo <= hi) ? juce::jlimit (lo, hi, p.time)
                            : (lo + hi) * 0.5f;
    }
}

void NoteEnvelope::sortPoints()
{
    std::stable_sort (points.begin(), points.end(),
                      [] (const EnvelopePoint& a, const EnvelopePoint& b) { return a.time < b.time; });
}

bool NoteEnvelope::hasSameShapeAs (const NoteEnvelope& other) const noexcept
{
    if (points.size() != other.points.size() || enabled != other.enabled)
        return false;

    for (size_t i = 0; i < points.size(); ++i)
    {
        if (std::abs (points[i].time  - other.points[i].time)  > 1.0e-5f) return false;
        if (std::abs (points[i].value - other.points[i].value) > 1.0e-5f) return false;
    }

    return true;
}

} // namespace atomic

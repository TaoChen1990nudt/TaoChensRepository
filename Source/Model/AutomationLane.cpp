#include "AutomationLane.h"

#include <algorithm>

namespace atomic
{

AutomationLane::AutomationLane()
{
    resetFlat (1.0f, timeline::defaultLengthBeats);
}

float AutomationLane::valueAtBeat (double beat) const noexcept
{
    return automationValueAt (points, beat);
}

void AutomationLane::resetFlat (float value, double lengthBeats)
{
    points.clear();
    points.push_back ({ 0.0, juce::jlimit (0.0f, 1.0f, value) });
    points.push_back ({ juce::jmax (1.0, lengthBeats), juce::jlimit (0.0f, 1.0f, value) });
}

int AutomationLane::addPoint (double beat, float value, double lengthBeats)
{
    if ((int) points.size() >= maxPoints)
        return -1;

    points.push_back ({ juce::jlimit (0.0, juce::jmax (1.0, lengthBeats), beat),
                        juce::jlimit (0.0f, 1.0f, value) });

    sortPoints();

    for (int i = 0; i < (int) points.size(); ++i)
        if (juce::approximatelyEqual (points[(size_t) i].beat,
                                      juce::jlimit (0.0, juce::jmax (1.0, lengthBeats), beat)))
            return i;

    return (int) points.size() - 1;
}

void AutomationLane::removePoint (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()) || points.size() <= 2)
        return;

    points.erase (points.begin() + index);
}

int AutomationLane::movePoint (int index, double beat, float value, double lengthBeats)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return -1;

    points[(size_t) index].beat  = juce::jlimit (0.0, juce::jmax (1.0, lengthBeats), beat);
    points[(size_t) index].value = juce::jlimit (0.0f, 1.0f, value);

    const double movedBeat = points[(size_t) index].beat;
    const float  movedValue = points[(size_t) index].value;

    sortPoints();

    // Follow the handle the user is holding: after a re-sort its index may have
    // changed, and a drag that jumps to a different point is maddening.
    for (int i = 0; i < (int) points.size(); ++i)
        if (juce::approximatelyEqual (points[(size_t) i].beat, movedBeat)
             && juce::approximatelyEqual (points[(size_t) i].value, movedValue))
            return i;

    return index;
}

void AutomationLane::loadPoints (std::vector<AutomationPoint> restoredPoints)
{
    if (restoredPoints.size() > (size_t) maxPoints)
        restoredPoints.resize ((size_t) maxPoints);

    if (restoredPoints.size() < 2)
    {
        const float value = restoredPoints.empty() ? 1.0f
                                                   : juce::jlimit (0.0f, 1.0f, restoredPoints.front().value);
        resetFlat (value, timeline::defaultLengthBeats);
        return;
    }

    for (auto& p : restoredPoints)
    {
        p.beat  = juce::jmax (0.0, p.beat);
        p.value = juce::jlimit (0.0f, 1.0f, p.value);
    }

    points = std::move (restoredPoints);
    sortPoints();
}

bool AutomationLane::hasSameShapeAs (const AutomationLane& other) const noexcept
{
    if (points.size() != other.points.size())
        return false;

    for (size_t i = 0; i < points.size(); ++i)
        if (! juce::approximatelyEqual (points[i].beat, other.points[i].beat)
             || ! juce::approximatelyEqual (points[i].value, other.points[i].value))
            return false;

    return true;
}

void AutomationLane::sortPoints()
{
    std::stable_sort (points.begin(), points.end(),
                      [] (const AutomationPoint& a, const AutomationPoint& b)
                      { return a.beat < b.beat; });
}

} // namespace atomic

#include "AutomationLaneComponent.h"

#include "Util/Metrics.h"

namespace atomic
{

namespace
{
    const juce::Colour kLaneBackground { 0xff12131a };
    const juce::Colour kLaneBorder     { 0xff2a2d38 };
    const juce::Colour kGrid           { 0xff20222c };

    constexpr float kHandleRadius = atomic::ui::pf (3.4);
    constexpr float kHitRadius    = atomic::ui::pf (8.0);
}

//==============================================================================
AutomationLaneComponent::AutomationLaneComponent()
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void AutomationLaneComponent::setLaneSource (std::function<AutomationLane*()> source)
{
    laneSource = std::move (source);
    repaint();
}

void AutomationLaneComponent::setAccentColour (juce::Colour colour)
{
    accent = colour;
    repaint();
}

void AutomationLaneComponent::setTimeline (double newLengthBeats)
{
    lengthBeats = juce::jmax (1.0, newLengthBeats);
    repaint();
}

void AutomationLaneComponent::setPlayheadBeat (double beats)
{
    if (juce::approximatelyEqual (beats, playheadBeat))
        return;

    playheadBeat = beats;
    repaint();
}

AutomationLane* AutomationLaneComponent::resolve() const
{
    return laneSource != nullptr ? laneSource() : nullptr;
}

//==============================================================================
juce::Rectangle<float> AutomationLaneComponent::getCurveArea() const
{
    return getLocalBounds().toFloat().reduced (5.0f, 7.0f);
}

juce::Point<float> AutomationLaneComponent::beatToPoint (double beat, float value) const
{
    auto area = getCurveArea();

    const float x = area.getX() + (float) (juce::jlimit (0.0, lengthBeats, beat) / lengthBeats) * area.getWidth();
    const float y = area.getBottom() - juce::jlimit (0.0f, 1.0f, value) * area.getHeight();

    return { x, y };
}

double AutomationLaneComponent::pointToBeat (float x) const
{
    auto area = getCurveArea();

    if (area.getWidth() <= 0.0f)
        return 0.0;

    return juce::jlimit (0.0, lengthBeats, (double) ((x - area.getX()) / area.getWidth()) * lengthBeats);
}

float AutomationLaneComponent::pointToValue (float y) const
{
    auto area = getCurveArea();

    if (area.getHeight() <= 0.0f)
        return 1.0f;

    return juce::jlimit (0.0f, 1.0f, (area.getBottom() - y) / area.getHeight());
}

int AutomationLaneComponent::findPointAt (juce::Point<float> position) const
{
    auto* lane = resolve();

    if (lane == nullptr)
        return -1;

    int best = -1;
    float bestDistance = kHitRadius;

    for (int i = 0; i < (int) lane->getPoints().size(); ++i)
    {
        const auto& point = lane->getPoints()[(size_t) i];
        const auto centre = beatToPoint (point.beat, point.value);
        const float distance = centre.getDistanceFrom (position);

        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

//==============================================================================
void AutomationLaneComponent::paint (juce::Graphics& g)
{
    auto* lane = resolve();

    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    auto area   = getCurveArea();

    g.setColour (kLaneBackground);
    g.fillRoundedRectangle (bounds, 5.0f);

    g.setColour (kLaneBorder);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    if (lane == nullptr)
        return;

    //-- Grid: every 4 beats, so a curve can be read against the bar lines ----
    g.setColour (kGrid);

    for (double beat = 0.0; beat <= lengthBeats + 0.001; beat += 4.0)
    {
        const float x = beatToPoint (beat, 0.0f).getX();

        if (x <= area.getX() + 0.5f || x >= area.getRight() - 0.5f)
            continue;

        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
    }

    // Centre line (0.5 = "no change"), useful for pan lanes especially.
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    const bool enabled = lane->isEnabled();
    const auto ink = enabled ? accent : juce::Colours::white.withAlpha (0.36f);

    //-- Curve -----------------------------------------------------------------
    const auto& points = lane->getPoints();

    if (points.size() >= 2)
    {
        juce::Path curve;
        auto first = beatToPoint (points.front().beat, points.front().value);
        curve.startNewSubPath (first);

        for (size_t i = 0; i < points.size(); ++i)
        {
            auto p = beatToPoint (points[i].beat, points[i].value);
            curve.lineTo (p);
        }

        // Extend to the lane edges so it is obvious the value holds.
        curve.lineTo (area.getRight(), beatToPoint (lengthBeats, points.back().value).getY());

        // Fill under the curve: the readout is a level, so show the area it
        // encloses rather than a bare line.
        juce::Path filled (curve);
        filled.lineTo (area.getRight(), area.getBottom());
        filled.lineTo (area.getX(), area.getBottom());
        filled.closeSubPath();

        g.setColour (ink.withAlpha (enabled ? 0.13f : 0.06f));
        g.fillPath (filled);

        g.setColour (ink.withAlpha (enabled ? 0.95f : 0.55f));
        g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    //-- Handles ---------------------------------------------------------------
    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto centre = beatToPoint (points[i].beat, points[i].value);
        const bool active = (i == hoveredIndex || i == draggedIndex);
        const float r = active ? kHandleRadius + 1.2f : kHandleRadius;

        g.setColour (kLaneBackground);
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre));

        g.setColour (active ? juce::Colours::white : ink);
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre),
                       active ? 1.6f : 1.2f);

        if (active)
            g.fillEllipse (juce::Rectangle<float> (2.6f, 2.6f).withCentre (centre));
    }

    //-- Playhead --------------------------------------------------------------
    if (playheadBeat >= 0.0 && playheadBeat <= lengthBeats)
    {
        const float x = beatToPoint (playheadBeat, 0.0f).getX();

        g.setColour (juce::Colour (0xffff5f6d).withAlpha (0.75f));
        g.drawVerticalLine ((int) x, area.getY() - 3.0f, area.getBottom() + 3.0f);
    }

    //-- Off watermark ---------------------------------------------------------
    if (! enabled)
    {
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (8.5f)).withStyle ("Bold")));
        g.drawText ("AUTOMATION OFF", getLocalBounds().reduced (8, 4),
                    juce::Justification::topRight, false);
    }
}

//==============================================================================
void AutomationLaneComponent::mouseDown (const juce::MouseEvent& e)
{
    auto* lane = resolve();

    if (lane == nullptr)
        return;

    const int hit = findPointAt (e.position);

    // Right-click or Alt-click removes a handle. Keep two: a lane with fewer
    // is not a curve any more.
    if (hit >= 0 && (e.mods.isRightButtonDown() || e.mods.isAltDown()))
    {
        lane->removePoint (hit);
        hoveredIndex = -1;

        if (onChange != nullptr)
            onChange();

        repaint();
        return;
    }

    if (hit >= 0)
    {
        draggedIndex = hit;
        repaint();
        return;
    }

    // Empty space: add a breakpoint right where the user clicked and drag it.
    draggedIndex = lane->addPoint (pointToBeat (e.position.x),
                                   pointToValue (e.position.y),
                                   lengthBeats);

    if (onChange != nullptr)
        onChange();

    repaint();
}

void AutomationLaneComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto* lane = resolve();

    if (lane == nullptr || draggedIndex < 0)
        return;

    draggedIndex = lane->movePoint (draggedIndex,
                                    pointToBeat (e.position.x),
                                    pointToValue (e.position.y),
                                    lengthBeats);

    if (onChange != nullptr)
        onChange();

    repaint();
}

void AutomationLaneComponent::mouseUp (const juce::MouseEvent&)
{
    draggedIndex = -1;
    repaint();
}

void AutomationLaneComponent::mouseMove (const juce::MouseEvent& e)
{
    const int hit = findPointAt (e.position);

    if (hit != hoveredIndex)
    {
        hoveredIndex = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void AutomationLaneComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredIndex != -1)
    {
        hoveredIndex = -1;
        repaint();
    }
}

} // namespace atomic

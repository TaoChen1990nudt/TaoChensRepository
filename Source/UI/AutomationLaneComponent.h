#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Model/AutomationLane.h"

namespace atomic
{

//==============================================================================
/**
    The editable canvas of one macro automation lane.

    Like EnvelopeEditor it holds a RESOLVER, never an AutomationLane*. The lane
    lives inside a Layer or a SynthSlot that the user can delete at any moment;
    a cached pointer would be a crash waiting for a right-click.

    Two curve types exist in this project and their time bases differ on
    purpose: note envelopes are normalised 0..1 inside their note, these are in
    absolute beats. Here a drag writes a beat, so the curve stays where it was
    drawn even when the material underneath it moves.
*/
class AutomationLaneComponent final : public juce::Component
{
public:
    AutomationLaneComponent();

    void setLaneSource (std::function<AutomationLane*()> source);
    void setAccentColour (juce::Colour colour);
    void setTimeline (double lengthBeats);

    /** Draws a vertical marker. Negative hides it. */
    void setPlayheadBeat (double beats);

    /** Fired after every edit — while dragging too, so automation is audible
        as it is being drawn. */
    std::function<void()> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    AutomationLane* resolve() const;

    juce::Rectangle<float> getCurveArea() const;
    juce::Point<float> beatToPoint (double beat, float value) const;
    double pointToBeat (float x) const;
    float  pointToValue (float y) const;
    int    findPointAt (juce::Point<float>) const;

    std::function<AutomationLane*()> laneSource;

    juce::Colour accent { 0xff4fd1c5 };
    double lengthBeats  = timeline::defaultLengthBeats;
    double playheadBeat = -1.0;

    int draggedIndex = -1;
    int hoveredIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneComponent)
};

} // namespace atomic

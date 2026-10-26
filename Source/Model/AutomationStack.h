#pragma once

#include <juce_core/juce_core.h>

#include <vector>

#include "AutomationLane.h"

namespace atomic
{

//==============================================================================
/**
    Phase 36: how three automation curves that all point at the same control
    become one number.

    The engine has always multiplied the level curves and added the pan offsets,
    but it did so inline, in two render loops, which meant the rule existed twice
    and was testable in neither place. Worse, nothing could answer the question a
    user actually asks — "so what does this note sound like right now?" —
    without re-implementing the loop's arithmetic somewhere else and hoping the
    copy stayed true.

    So the law lives here, once, as free functions. Three layers:

      * MACRO — the layer's fader and curve times the synth's fader and curve.
        The "how loud is this part of the mix" layer. A note passes through it
        whether or not anybody grouped it.
      * GROUP — the group's trim, curve and mute. A statement about members.
      * NOTE  — the note's own envelope, normalised over the note itself.

    LEVEL MULTIPLIES. Three gates each half open are one eighth as loud, and
    that is the only reading under which turning a layer down twice does what
    the user expects. It also means the order never matters, so a layer that is
    evaluated at a different stage cannot change the result.

    PAN ADDS. A pan is an angle, not a gain: two half-lefts must not cancel, and
    a group panned hard left plus a note nudged right has to land somewhere in
    between. Summed at the end, clamped once.

    A HOSTED PARAMETER MULTIPLIES for the same reason level does — two curves
    both written at 0.5 should combine to 0.25, not to 1.0, because a parameter
    is a normalised amount and "half of half" is the honest arithmetic.

    An ABSENT layer is the identity (1 for level and for a parameter, 0 for pan),
    so "no group" and "a group at unity" are literally the same number and no
    caller needs a special case for a note that was never grouped.
*/
namespace automation
{

//==============================================================================
/** The three level contributions, kept as three numbers rather than one product
    so a readout can say WHICH layer took the level away. Multiply them through
    `total()` to get what the renderer uses. */
struct StackLevels
{
    float macro = 1.0f;
    float group = 1.0f;
    float note  = 1.0f;

    float total() const noexcept  { return macro * group * note; }
    bool  silent() const noexcept { return total() <= 0.0f; }
};

//==============================================================================
/** The three pan contributions, already in -1..+1 offset form. */
struct StackPans
{
    float macro = 0.0f;   // layer pan + synth pan, summed by the caller
    float group = 0.0f;
    float note  = 0.0f;   // what a note's own pan curve contributes

    float total() const noexcept
    {
        return juce::jlimit (-1.0f, 1.0f, macro + group + note);
    }
};

//==============================================================================
/** Level: multiplied. The one line the render loops must not re-derive. */
inline float stackLevel (float macro, float group, float note) noexcept
{
    return macro * group * note;
}

/** Pan: added, then clamped once. */
inline float stackPan (float macro, float group, float note) noexcept
{
    return juce::jlimit (-1.0f, 1.0f, macro + group + note);
}

/** A hosted parameter: multiplied. Either side may be the identity (1.0) when
    that layer has no live curve, so one live curve passes through unchanged. */
inline float stackPluginParam (float group, float note) noexcept
{
    return juce::jlimit (0.0f, 1.0f, group * note);
}

//==============================================================================
/** Samples an absolute-beat lane onto a note's own 0..1 time base.

    The three layers live on two different clocks: a macro or group lane is
    placed on the project's beats, a note envelope is placed inside its note.
    Overlaying them therefore needs one of the two converted, and the note is
    the one that moves — so the lane is sampled here, once per redraw, into
    `steps + 1` values the painter indexes by u.

    Returns false when there is nothing to draw (lane disabled, no points, or a
    zero-length note), and fills `destination` with the identity so a caller
    that ignores the answer cannot produce a zeroed curve by accident. */
inline bool sampleLaneInto (const std::vector<AutomationPoint>& points,
                            bool enabled,
                            double startBeat, double endBeat,
                            std::vector<float>& destination,
                            int steps = 64)
{
    destination.assign ((size_t) juce::jmax (1, steps) + 1, 1.0f);

    if (! enabled || points.empty())
        return false;

    const double span = endBeat - startBeat;

    if (span <= 0.0)
        return false;

    for (int i = 0; i <= steps; ++i)
        destination[(size_t) i] = automationValueAt (points,
                                                      startBeat + span * (double) i / (double) steps);

    return true;
}

/** Looks a sampled curve up at u, linearly. Used by the overlay painter, which
    then applies the same `stackLevel` the audio thread applies — the picture is
    not allowed to be a second, more flattering implementation. */
inline float sampleAt (const std::vector<float>& curve, float u) noexcept
{
    if (curve.empty())
        return 1.0f;

    const float scaled = juce::jlimit (0.0f, 1.0f, u) * (float) (curve.size() - 1);
    const int   i0     = (int) scaled;
    const int   i1     = juce::jmin ((int) curve.size() - 1, i0 + 1);
    const float frac   = scaled - (float) i0;

    return curve[(size_t) i0] * (1.0f - frac) + curve[(size_t) i1] * frac;
}

} // namespace automation
} // namespace atomic

#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace atomic
{

//==============================================================================
/** One breakpoint of a macro automation lane.

    `beat` is ABSOLUTE, not normalised — the opposite of NoteEnvelope. A note
    envelope belongs to one note and must stretch with it; a mix lane belongs to
    the project and must stay put when a note is dragged across it. Two curve
    types, two time bases, on purpose.
*/
struct AutomationPoint
{
    double beat  = 0.0;
    float  value = 1.0f;   // 0..1
};

//==============================================================================
/** What a layer's lane drives. */
enum class LayerAutomationTarget
{
    volume,
    pan
};

/** What a synth slot's lane drives. */
enum class SlotAutomationTarget
{
    filter,
    level
};

/** What a note group's lane drives.

    Phase 16 shipped with one parameter, on the argument that a group is a level
    trim and nothing else. Phase 20 adds pan, because "automate the group" is a
    request about the group's place in the stereo field just as often as about
    its loudness — and a group that could be faded but not swept was a gap the
    user hits as soon as they build a busy stack. Both lanes are absolute beats,
    both are multiplied/added at the voice stage next to the group's own trim.
    Pitch stays per-note: that is what the micro envelope is for. */
enum class GroupAutomationTarget
{
    level,
    pan
};

//==============================================================================
/**
    A macro automation curve: a handful of breakpoints over project time.

    The lane always carries data, even when switched off. "Enabled" is a
    separate flag so turning automation off restores the fader value without
    destroying a curve the user just drew.
*/
class AutomationLane
{
public:
    static constexpr int maxPoints = 32;

    AutomationLane();

    bool isEnabled() const noexcept          { return enabled; }
    void setEnabled (bool e) noexcept        { enabled = e; }

    const std::vector<AutomationPoint>& getPoints() const noexcept { return points; }

    /** Interpolated 0..1 value at an absolute beat. Flat outside the ends. */
    float valueAtBeat (double beat) const noexcept;

    /** Replaces the curve with a flat line across 0..lengthBeats. */
    void resetFlat (float value, double lengthBeats);

    /** Inserts a point, keeping the list sorted. Returns its index, or -1 when
        the lane is full. */
    int  addPoint (double beat, float value, double lengthBeats);

    /** Removes a point. Always keeps at least two, so the curve never vanishes. */
    void removePoint (int index);

    /** Moves a point and re-sorts. Returns the point's new index, which is what
        a drag handler needs to keep following the same handle. */
    int  movePoint (int index, double beat, float value, double lengthBeats);

    /** Replaces the curve wholesale (project load).

        Beats are clamped to >= 0 and the list is sorted and capped, so a
        truncated or reordered file still yields a curve the timeline can
        evaluate. Fewer than two points is repaired into a flat line. The
        enabled flag is separate and untouched. */
    void loadPoints (std::vector<AutomationPoint> restoredPoints);

    /** Shape equality, ignoring the enabled flag. Used by the self-test. */
    bool hasSameShapeAs (const AutomationLane& other) const noexcept;

private:
    void sortPoints();

    std::vector<AutomationPoint> points;
    bool enabled = false;

    JUCE_LEAK_DETECTOR (AutomationLane)
};

//==============================================================================
/** Evaluates a curve snapshot on the audio thread.

    Free function, not a member: the audio thread never sees an AutomationLane,
    only the copied-or-not points inside the published schedule. Same rule the
    note envelopes follow.
*/
inline float automationValueAt (const std::vector<AutomationPoint>& pts, double beat) noexcept
{
    if (pts.empty())
        return 1.0f;

    if (beat <= pts.front().beat)
        return pts.front().value;

    if (beat >= pts.back().beat)
        return pts.back().value;

    for (size_t i = 1; i < pts.size(); ++i)
    {
        const auto& b = pts[i];

        if (beat <= b.beat)
        {
            const auto& a = pts[i - 1];
            const double span = b.beat - a.beat;

            if (span <= 1.0e-9)
                return b.value;

            const double t = (beat - a.beat) / span;
            return (float) (a.value + (b.value - a.value) * t);
        }
    }

    return pts.back().value;
}

//==============================================================================
namespace timeline
{
    /** Project length in beats. One number, shared by the transport, the grid
        and every automation lane, so a lane can never address a moment the
        transport does not have. */
    inline constexpr double defaultLengthBeats = 64.0;
}

} // namespace atomic

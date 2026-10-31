#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

#include "AutomationLane.h"

namespace atomic
{

//==============================================================================
/** Phase 98: the three MIDI controllers a layer can draw.

    Why these three and not a free choice of 0..127: they are the three the
    listening study named, and each one answers a different question about a
    performance. The mod wheel shapes the timbre, expression shapes the level
    without touching the fader, and the sustain pedal is a gate that has to be
    drawn as one (a curve that jumps 0 -> 1, not a ramp).

    The ORDER is the serialised order and it never changes: a lane is addressed
    by this enum, so appending is the only safe growth.
*/
enum MidiCcLane
{
    ccModWheel   = 0,   // CC 1
    ccExpression = 1,   // CC 11
    ccSustain    = 2    // CC 64
};

inline constexpr int numMidiCcLanes = 3;

/** The controller number a lane is sent as. One table, so a lane can never be
    drawn as one number and transmitted as another. */
inline int midiCcNumber (int lane) noexcept
{
    switch (lane)
    {
        case ccExpression: return 11;
        case ccSustain:    return 64;
        case ccModWheel:
        default:           return 1;
    }
}

/** ASCII key used in the project file. Same rule as `noteModTargetKey`: the
    model never needs a font, and the UI never needs to parse a number. */
inline const char* midiCcKey (int lane) noexcept
{
    switch (lane)
    {
        case ccExpression: return "expression";
        case ccSustain:    return "sustain";
        case ccModWheel:
        default:           return "modwheel";
    }
}

inline int midiCcFromKey (const juce::String& key) noexcept
{
    if (key == "expression") return ccExpression;
    if (key == "sustain")    return ccSustain;

    return ccModWheel;
}

/** The inverse of `midiCcNumber`, for the loader.

    A file naming a controller we do not keep a lane for (a hand-edited project,
    or a future version that added CC 7) maps to the mod wheel rather than being
    dropped: the points survive, in the nearest lane that exists, and the user
    can see something is there. Silently discarding a curve is the one outcome
    nobody can diagnose later. */
inline int midiCcLaneFromNumber (int ccNumber) noexcept
{
    for (int lane = 0; lane < numMidiCcLanes; ++lane)
        if (midiCcNumber (lane) == ccNumber)
            return lane;

    return ccModWheel;
}

//==============================================================================
/** A layer's three controllers: each one a curve, each one possibly switched off.

    The CURVE is an `AutomationLane`, not a new type. A CC curve is a handful of
    breakpoints over absolute project time with a 0..1 value between them —
    which is exactly what a layer's volume lane already is. Reusing the class
    means `valueAtBeat`, `addPoint`, `movePoint`, `loadPoints` and the project
    file's `<Point beat value>` shape all come for free, and there is no second
    interpolation to keep in step with the first.

    What is NOT reused is the meaning of the numbers:

      * A lane's value is stored 0..1 like every other curve in this project,
        and quantised to the controller's 128 steps only when it is SENT. The
        model stays continuous so a drag behaves like every other drag; MIDI's
        resolution is a property of the wire, not of the drawing.

      * "Off" means the lane is not sent at all — not "sent as a flat line".
        A plugin keeps whatever value it last received, which is what every
        other host does and the only reading under which the switch is useful.

      * A lane with no points at all must still read as 0, not as the
        `automationValueAt` default of 1.0 — "no mod wheel data" is silence,
        not full modulation. `MidiCcSet`'s constructor resets every lane to a
        flat zero so the two can never be confused.
*/
struct MidiCcSet
{
    MidiCcSet()
    {
        for (auto& lane : lanes)
            lane.resetFlat (0.0f, timeline::defaultLengthBeats);
    }

    AutomationLane&       operator[] (int lane)       noexcept { return lanes[(size_t) juce::jlimit (0, numMidiCcLanes - 1, lane)]; }
    const AutomationLane& operator[] (int lane) const noexcept { return lanes[(size_t) juce::jlimit (0, numMidiCcLanes - 1, lane)]; }

    /** True when at least one lane will actually transmit. The publisher uses
        this to skip the whole set rather than copy three empty vectors. */
    bool anyEnabled() const noexcept
    {
        for (const auto& lane : lanes)
            if (lane.isEnabled())
                return true;

        return false;
    }

    std::array<AutomationLane, numMidiCcLanes> lanes;
};

//==============================================================================
/** The wire's resolution, in one place.

    A controller value is an integer 0..127 and the model's is a float 0..1.
    Rounding happens HERE and only here, so "the number I drew" and "the number
    the plugin got" can only ever disagree by the quantisation itself. */
namespace midicc
{
    inline int toMidi (float value) noexcept
    {
        const float clamped = juce::jlimit (0.0f, 1.0f, value);
        return juce::jlimit (0, 127, (int) std::lround (clamped * 127.0f));
    }

    inline float fromMidi (int raw) noexcept
    {
        return (float) juce::jlimit (0, 127, raw) / 127.0f;
    }

    /** The sentinel for "nothing has been sent on this lane yet".

        Not 0: zero is a real controller value (no modulation), and a lane whose
        first value happens to be zero must still be transmitted once — a plugin
        that kept a value from a previous pass has to be told to go to zero. */
    inline constexpr int nothingSent = -1;

    /** What, if anything, a render chunk should transmit. Returns -1 for none.

        Three rules, and the first two are the whole point of this function
        existing:

        1. **Only a change is sent.** A controller is a STATE, not an event. A
           flat line drawn across a four-second note is one message, not one per
           block — the naive "send every block" version produces several hundred
           identical messages for a gesture the user thinks of as "held".

        2. **One look per chunk, at its start.** The value is sampled where the
           chunk begins, which makes the time resolution of a CC curve equal to
           the render chunk (a few milliseconds). That is the same trade
           `applyHostedParamAutomation` already makes for hosted parameters, and
           the alternative — interpolating per sample — would send a stream of
           intermediate values the plugin cannot use.

        3. **A switched-off lane sends nothing**, and returns -1 so the caller
           leaves its remembered value alone. (Leaving it alone is what makes
           re-enabling the lane send the current value again instead of
           believing it already matches.)

        `lastValue` is `nothingSent` at the start of a pass; the caller is
        responsible for putting it back there on locate/stop/seek, because a
        stale value is a plugin stuck on a controller nobody is drawing
        (Rule 17's "a delay line is memory, not a constant").
    */
    inline int valueToSend (const AutomationLane& lane, double beat, int lastValue) noexcept
    {
        if (! lane.isEnabled())
            return nothingSent;

        const int value = toMidi (lane.valueAtBeat (beat));

        return value != lastValue ? value : nothingSent;
    }

    /** The same rule for a PUBLISHED curve.

        The renderer holds a bare point vector, not an `AutomationLane` — the
        schedule is a frozen copy and the class carries a `JUCE_LEAK_DETECTOR`
        and a `std::vector` nothing on the audio thread needs. Two entry points,
        one rule: this overload is the audio thread's, the one above is the
        model's, and they cannot disagree because both end in the same
        `automationValueAt` and the same `toMidi`. */
    inline int valueToSend (const std::vector<AutomationPoint>& points, bool enabled,
                            double beat, int lastValue) noexcept
    {
        if (! enabled)
            return nothingSent;

        const int value = toMidi (automationValueAt (points, beat));

        return value != lastValue ? value : nothingSent;
    }

} // namespace midicc

} // namespace atomic

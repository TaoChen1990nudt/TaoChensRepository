#pragma once

#include <juce_graphics/juce_graphics.h>

namespace atomic
{

/** Phase 97: velocity as a number the user can point at.

    Three rules, all about the value and none about pixels:

    - **The axis is linear in velocity.** A lane that curves (dB, or MIDI's own
      square-law loudness) puts half of its height in the top two velocity
      steps, where almost nothing changes by ear. Linear is also what makes the
      bar height readable as "how much of the note" at a glance.

    - **The floor is not zero.** 0 is not "very quiet", it is "does not sound",
      and that is what mute and delete are for. A bar dragged to 0 leaves a note
      on the sheet that makes no noise and looks like a bug. The floor is one
      step above the bottom, so the quietest writeable velocity is barely
      audible rather than absent.

    - **A drag is ONE delta for the whole set**, applied per note and clamped
      per note (see `shifted`). Setting every bar to the pointer's height would
      destroy the accent pattern the user selected in order to edit it; a shared
      delta moves the phrase as a phrase. Clamping at the ceiling flattens the
      loud end of it, which is honest — the alternative is refusing to move
      anything at all and calling that "protection".
*/
namespace velocitylane
{
    /** The bottom of the lane, in velocity. Deliberately not 0 — see above. */
    inline constexpr float floor = 0.02f;

    /** What a note gets when nobody said otherwise: `NoteEvent::velocity`'s own
        default, and the value the project reader assumes when a file has none.
        Named here so the lane's "reset" gestures and the model cannot drift. */
    inline constexpr float defaultVelocity = 0.8f;

    inline float clamp (float v) noexcept
    {
        return juce::jlimit (floor, 1.0f, v);
    }

    /** The whole rule of a velocity drag: one delta, clamped per note. */
    inline float shifted (float from, float delta) noexcept
    {
        return clamp (from + delta);
    }

    /** A y inside `box` → velocity. Above the box reads as 1, below as the
        floor, which is what lets a drag leave the lane and keep going. */
    static inline float fromY (float y, juce::Rectangle<float> box) noexcept
    {
        if (box.getHeight() <= 0.0f)
            return 1.0f;

        return clamp (1.0f - (y - box.getY()) / box.getHeight());
    }

    /** Velocity → the y of the bar's top. The bar grows DOWN from here to the
        box's bottom, which is the only orientation that reads as a level. */
    static inline float toY (float v, juce::Rectangle<float> box) noexcept
    {
        return box.getBottom() - clamp (v) * box.getHeight();
    }
} // namespace velocitylane

} // namespace atomic

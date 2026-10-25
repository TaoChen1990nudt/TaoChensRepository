#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace atomic
{
//==============================================================================
/** Phase 13: one dial for the whole shell's density.

    Every size that belongs to the *interface* — font heights, row heights,
    panel widths, button boxes, paddings — is written at the number the layout
    was originally drawn at, then multiplied by `scale`. Bumping the constants
    by hand would make the next "it is still too small" a hundred-line diff;
    this makes it a one-line diff, and it keeps the numbers readable: a row is
    still "74", even though it is now 96 pixels tall.

    Deliberately NOT applied to the musical coordinate space. `beatWidth` stays
    48 pixels per beat and `numPitches` stays 128: those describe what the
    music is measured in, not how big the controls are. Scaling them would
    change how much of the arrangement fits on screen, which is a different
    decision from "the text is too small".
*/
namespace ui
{
    /** 1.0 = the original 1440x820 design. 1.3 is roughly the step where 8.5 pt
        labels stop being smudges on a 100% DPI screen. */
    constexpr float scale = 1.30f;

    /** A design-space length, in real pixels. */
    constexpr int px (double designUnits) noexcept
    {
        return (int) (designUnits * scale + 0.5);
    }

    /** Same, unrounded — font heights are floats and must stay so. */
    constexpr float sp (float designPoints) noexcept
    {
        return designPoints * scale;
    }

    /** A design-space length as a float, for painting geometry. */
    constexpr float pf (double designUnits) noexcept
    {
        return (float) (designUnits * scale);
    }

    /** The left gutter of the timeline, shared by both views.

        The piano keys on the roll and the track headers in the automation
        window are two answers to the same question — "where does beat 0
        start?" — so they must be the same width. When they were not, the
        automation canvas began 86 px to the right of the grid it edits, and
        every curve sat under the wrong beat; the playheads of the two views
        did not even line up vertically. Defined once here so they cannot
        drift apart again, and asserted in the self-test. */
    constexpr int gutterWidth = px (74);

    inline juce::Font font (float designPoints,
                            const juce::String& style = juce::String())
    {
        juce::FontOptions options { sp (designPoints) };

        if (style.isNotEmpty())
            options = options.withStyle (style);

        return juce::Font (options);
    }

    inline juce::Font bold (float designPoints)
    {
        return font (designPoints, "Bold");
    }
} // namespace ui
} // namespace atomic

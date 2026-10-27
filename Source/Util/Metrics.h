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

    //-- Phase 50: the typographic layer (DESIGN.md §5) ----------------------
    /** A font with extra letter spacing, given in DESIGN-SPACE PIXELS.

        JUCE's kerning factor is in EM, not pixels: the shaped-text code adds
        `height * factor` after every glyph. Passing DESIGN.md's "+0.5" straight
        through therefore meant half the font height — at 14 pt that is 9 px of
        air per character, which the first P50 frame showed as "0 2 . 3 . 3".
        The conversion lives here so every caller can keep thinking in pixels. */
    inline juce::Font tracked (float designPoints, float kerningPixels,
                               const juce::String& style = juce::String())
    {
        juce::FontOptions options { sp (designPoints) };
        options = options.withKerningFactor (juce::jmax (1.0f, designPoints) > 0.0f
                                               ? kerningPixels / juce::jmax (1.0f, designPoints)
                                               : 0.0f);

        if (style.isNotEmpty())
            options = options.withStyle (style);

        return juce::Font (options);
    }

    /** Panel titles: bold, slightly open. */
    inline juce::Font title (float designPoints = 11.0f)
    {
        return tracked (designPoints, 0.5f, "Bold");
    }

    /** Panel subtitles / section headers ("LAYER MIX · 3"): small, wide,
        upper-case. The caller passes text that is already capitalised —
        `toUpperCase()` on a Chinese label is a no-op and on a code string is a
        lie, so it is not done here. */
    inline juce::Font section (float designPoints = 8.5f)
    {
        return tracked (designPoints, 0.8f, "Bold");
    }

    //-- Tabular figures ------------------------------------------------------
    /** Width of the widest digit in this font.

        JUCE's Font has no "tabular figures" switch — the OpenType feature is not
        exposed — so the only portable way to stop a readout from twitching is to
        reserve one cell per digit and centre each glyph in it. Measured once per
        call site (these are 1-6 character strings, not paragraphs). */
    inline float digitCellWidth (const juce::Font& f)
    {
        float widest = 0.0f;

        for (int d = 0; d < 10; ++d)
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (
                                             f, juce::String (d)));

        return widest;
    }

    /** The width `text` would occupy if every digit were the same width. */
    inline float tabularWidth (const juce::Font& f, const juce::String& text)
    {
        const float cell = digitCellWidth (f);
        float width = 0.0f;

        for (int i = 0; i < text.length(); ++i)
        {
            const juce::juce_wchar c = text[i];

            width += (c >= '0' && c <= '9')
                       ? cell
                       : juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (c));
        }

        return width;
    }

    /** Draws `text` with digits on a fixed pitch, so 120 → 87 does not move the
        8 by two pixels. Everything else (colon, dot, dB, %) keeps its own width;
        only the digits are forced into cells. */
    inline void drawTabular (juce::Graphics& g, const juce::String& text,
                             juce::Rectangle<float> area, const juce::Font& f,
                             juce::Justification justification = juce::Justification::centredLeft)
    {
        const float total = tabularWidth (f, text);

        float x = area.getX();

        if (justification.testFlags (juce::Justification::right))
            x = area.getRight() - total;
        else if (justification.testFlags (juce::Justification::horizontallyCentred))
            x = area.getCentreX() - total * 0.5f;

        const float cell = digitCellWidth (f);

        g.setFont (f);

        for (int i = 0; i < text.length(); ++i)
        {
            const juce::juce_wchar c = text[i];
            const bool isDigit = (c >= '0' && c <= '9');
            const float width  = isDigit
                                   ? cell
                                   : juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (c));

            if (isDigit)
                g.drawText (juce::String::charToString (c),
                            juce::Rectangle<float> (x, area.getY(), cell, area.getHeight()),
                            juce::Justification::centred, false);
            else
                g.drawText (juce::String::charToString (c),
                            juce::Rectangle<float> (x, area.getY(), width, area.getHeight()),
                            juce::Justification::centred, false);

            x += width;
        }
    }
} // namespace ui
} // namespace atomic

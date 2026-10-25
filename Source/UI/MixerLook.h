#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

/**
    Shared look for the mixer controls.

    Both side panels are hand-painted, and both grew the same fader / pan / M /
    S cluster in Phase 5. Drawing it from one place is the only way the two
    panels cannot drift apart — the same reason routing lives in one header.
*/
namespace atomic::mixerlook
{

/** The fader law. 54 dB of travel, with 0 dB marked: -48 at the bottom, +6 at
    the top. Linear in dB, which is what a fader is supposed to be. */
inline constexpr float faderTopDb   = 6.0f;
inline constexpr float faderBottomDb = -48.0f;
inline constexpr float faderRangeDb = faderTopDb - faderBottomDb;

inline float normFromDb (float db) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - faderBottomDb) / faderRangeDb);
}

inline float dbFromNorm (float norm) noexcept
{
    return faderBottomDb + juce::jlimit (0.0f, 1.0f, norm) * faderRangeDb;
}

inline juce::String dbText (float db)
{
    if (db <= faderBottomDb + 0.05f)
        return juce::String::fromUTF8 ("-\xe2\x88\x9e");   // -inf

    return juce::String (db, 1);
}

/** "C", "L23", "R41" — a pan readout that fits in 40 px. */
inline juce::String panText (float pan)
{
    const int amount = juce::roundToInt (std::abs (pan) * 100.0f);

    if (amount < 2)
        return "C";

    return (pan < 0.0f ? "L" : "R") + juce::String (amount);
}

//==============================================================================
inline void drawFader (juce::Graphics& g, juce::Rectangle<float> r, float norm,
                       juce::Colour accent, bool hovered)
{
    norm = juce::jlimit (0.0f, 1.0f, norm);

    g.setColour (juce::Colours::black.withAlpha (0.42f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    const float filled = juce::jmax (2.0f, r.getWidth() * norm);

    g.setColour (accent.withAlpha (hovered ? 0.95f : 0.72f));
    g.fillRoundedRectangle (r.withWidth (filled), r.getHeight() * 0.5f);

    // 0 dB mark
    const float zeroX = r.getX() + r.getWidth() * normFromDb (0.0f);
    g.setColour (juce::Colours::white.withAlpha (0.26f));
    g.drawVerticalLine ((int) zeroX, r.getY() - 2.0f, r.getBottom() + 2.0f);

    const auto handle = juce::Rectangle<float> (ui::pf (6), r.getHeight() + ui::pf (6))
                          .withCentre ({ r.getX() + filled, r.getCentreY() });

    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.96f : 0.62f));
    g.fillRoundedRectangle (handle, 1.5f);
}

inline void drawPan (juce::Graphics& g, juce::Rectangle<float> r, float pan,
                     juce::Colour accent, bool hovered)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);

    g.setColour (juce::Colours::black.withAlpha (0.42f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    const float centre = r.getCentreX();
    const float x = centre + pan * r.getWidth() * 0.5f;

    g.setColour (accent.withAlpha (hovered ? 0.80f : 0.55f));
    g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (centre, x), r.getY(),
                                                    std::abs (x - centre), r.getHeight()), 2.0f);

    g.setColour (juce::Colours::white.withAlpha (0.30f));
    g.drawVerticalLine ((int) centre, r.getY() - 1.0f, r.getBottom() + 1.0f);

    const auto handle = juce::Rectangle<float> (ui::pf (5), r.getHeight() + ui::pf (6))
                          .withCentre ({ x, r.getCentreY() });

    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.92f : 0.55f));
    g.fillRoundedRectangle (handle, 1.5f);
}

//==============================================================================
inline void drawBadge (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text,
                       bool on, juce::Colour onColour, bool hovered)
{
    g.setColour (on ? onColour.withAlpha (0.20f)
                    : juce::Colours::white.withAlpha (hovered ? 0.10f : 0.045f));
    g.fillRoundedRectangle (r, ui::pf (4));

    g.setColour (on ? onColour.withAlpha (0.85f)
                    : juce::Colours::white.withAlpha (hovered ? 0.28f : 0.14f));
    g.drawRoundedRectangle (r, ui::pf (4), 1.0f);

    g.setColour (on ? onColour : juce::Colours::white.withAlpha (hovered ? 0.70f : 0.48f));
    g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (9.0f)).withStyle ("Bold")));
    g.drawText (text, r, juce::Justification::centred, false);
}

//==============================================================================
/** Meters are logarithmic: a linear bar spends its whole life in the last pixel
    because music peaks around 0.1 of full scale. -48 dB at the left, 0 dB at
    the right. */
inline float meterNorm (float peak) noexcept
{
    if (peak <= 1.0e-5f)
        return 0.0f;

    const float db = 20.0f * std::log10 (peak);
    return juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
}

inline void drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float peak, juce::Colour accent)
{
    g.setColour (juce::Colours::black.withAlpha (0.38f));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    const float level = meterNorm (peak);

    if (level <= 0.002f)
        return;

    g.setColour (accent.withAlpha (0.80f));
    g.fillRoundedRectangle (r.withWidth (juce::jmax (2.0f, r.getWidth() * level)),
                            r.getHeight() * 0.5f);
}

/** A small caret used by the expandable rows.

    `open` draws the ▼ of an expanded row; `closed` draws the ▶ of a collapsed
    one. The distinction matters: a row that folds to a single title bar has to
    look like it can be opened again, and an up-pointing triangle reads as "this
    is already as big as it gets". */
inline void drawCaret (juce::Graphics& g, juce::Rectangle<float> r, bool open, juce::Colour colour)
{
    juce::Path p;
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();

    if (open)
        p.addTriangle (cx - 3.5f, cy - 1.6f, cx + 3.5f, cy - 1.6f, cx, cy + 2.6f);
    else
        p.addTriangle (cx - 1.6f, cy - 3.5f, cx - 1.6f, cy + 3.5f, cx + 2.6f, cy);

    g.setColour (colour);
    g.fillPath (p);
}

} // namespace atomic::mixerlook

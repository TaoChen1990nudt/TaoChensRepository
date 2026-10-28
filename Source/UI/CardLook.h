#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"
#include "Util/Theme.h"

/**
    Shared look for the frames that mean "this is the thing you have selected".

    Three places draw that frame now — a sampler card in the rack, a synth card
    in the rack, and a row in the group list — and the user asked for them to
    look the same (Phase 60). "The same" has to be one function, not three
    copies of the same numbers: the rack already learned that lesson in Phase 55,
    when the spotlight and the selection drew two different rings on one card and
    neither read as load-bearing.
*/
namespace atomic::cardlook
{

/** The selection ring: a soft halo outside the frame and a crisp edge on it,
    both in the theme accent.

    `strength` only touches alpha. It exists so a ring can give way when a
    hand-lit spotlight is on the same card — not so callers can invent a second
    shape at lower alpha. */
inline void drawSelectionRing (juce::Graphics& g, juce::Rectangle<float> card,
                               float strength = 1.0f)
{
    g.setColour (theme::accent.withAlpha (0.22f * strength));
    g.drawRoundedRectangle (card.expanded (ui::pf (3)), ui::pf (6), ui::pf (3));

    g.setColour (theme::accent.withAlpha (0.95f * strength));
    g.drawRoundedRectangle (card.reduced (ui::pf (0.5)), ui::pf (7.5), ui::pf (1.8));
}

} // namespace atomic::cardlook

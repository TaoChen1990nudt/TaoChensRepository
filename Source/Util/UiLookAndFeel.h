#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

namespace atomic::ui
{
//==============================================================================
/**
    Phase 13: the fonts JUCE draws for us.

    Everything the shell paints itself goes through `boldFont` / `ui::font`, so
    it grew with `scale` for free. Menus, alert boxes, tab bars and combo boxes
    are painted by the default LookAndFeel, which knows nothing about our
    scale — at 1.3x the controls are bigger but their labels stay 12 pt, which
    reads as "the text got smaller". This subclass is the one place that tells
    them what size to use.

    It is installed as the *default* look and feel rather than per component:
    popup menus and alerts are owned by the desktop, not by the component that
    opened them, so a per-component one would be ignored exactly where it
    matters most.
*/
class UiLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    UiLookAndFeel() = default;

    juce::Font getPopupMenuFont() override                 { return bold (11.5f); }
    juce::Font getAlertWindowTitleFont() override          { return bold (13.5f); }
    juce::Font getAlertWindowMessageFont() override        { return font (11.0f); }
    juce::Font getAlertWindowFont() override               { return font (11.0f); }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return bold (10.5f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override          { return font (11.0f); }
    juce::Font getLabelFont (juce::Label&) override                { return font (11.0f); }
    juce::Font getTabButtonFont (juce::TabBarButton&, float) override { return bold (10.5f); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiLookAndFeel)
};

/** One instance for the whole app; lives until the process exits. */
inline UiLookAndFeel& lookAndFeel()
{
    static UiLookAndFeel instance;
    return instance;
}

} // namespace atomic::ui

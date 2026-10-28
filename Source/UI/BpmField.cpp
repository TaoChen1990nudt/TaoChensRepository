#include "BpmField.h"

#include "Util/Metrics.h"

#include "Util/Text.h"
#include "Util/Theme.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour kFieldBg    = theme::surface3;
    const juce::Colour kFieldHover = theme::surface4;
    const juce::Colour kFieldEdge  = theme::hairlineStrong;
    const juce::Colour& kLabel      = theme::inkFaint;
    const juce::Colour& kValue      = theme::ink;
    const juce::Colour& kAccent     = theme::accent;

    constexpr int kArrowColumnWidth = atomic::ui::px (18);
}

//==============================================================================
BpmField::BpmField()
{
    setWantsKeyboardFocus (true);

    editor.setVisible (false);
    editor.setJustification (juce::Justification::centredLeft);
    editor.setInputRestrictions (5, "0123456789.");
    editor.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (13.0f)).withStyle ("Bold")));
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::textColourId, kValue);
    editor.setColour (juce::TextEditor::highlightColourId, kAccent.withAlpha (0.30f));
    editor.setColour (juce::CaretComponent::caretColourId, kAccent);
    editor.setBorder (juce::BorderSize<int> (0));

    editor.onReturnKey  = [this] { closeEditor (true); };
    editor.onEscapeKey  = [this] { closeEditor (false); };
    editor.onFocusLost  = [this] { closeEditor (true); };

    addChildComponent (editor);
}

BpmField::~BpmField() = default;

//==============================================================================
void BpmField::setBpm (double newBpm, bool notify)
{
    applyBpm (newBpm, notify);
}

void BpmField::applyBpm (double candidate, bool notify)
{
    const double clamped = juce::jlimit (minBpm, maxBpm, std::round (candidate * 10.0) / 10.0);

    if (juce::approximatelyEqual (clamped, bpm))
        return;

    bpm = clamped;
    repaint();

    if (notify && onBpmChanged != nullptr)
        onBpmChanged (bpm);
}

void BpmField::nudge (double delta)
{
    applyBpm (bpm + delta, true);
}

juce::String BpmField::formatBpm() const
{
    const bool whole = std::abs (bpm - std::round (bpm)) < 0.05;
    return juce::String (bpm, whole ? 0 : 1);
}

//==============================================================================
juce::Rectangle<int> BpmField::getUpBounds() const
{
    return { getWidth() - kArrowColumnWidth, 0, kArrowColumnWidth, getHeight() / 2 };
}

juce::Rectangle<int> BpmField::getDownBounds() const
{
    return { getWidth() - kArrowColumnWidth, getHeight() / 2,
             kArrowColumnWidth, getHeight() - getHeight() / 2 };
}

juce::Rectangle<int> BpmField::getDragBounds() const
{
    return getLocalBounds().withTrimmedRight (kArrowColumnWidth);
}

//==============================================================================
void BpmField::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (hover || dragging ? kFieldHover : kFieldBg);
    g.fillRoundedRectangle (bounds, atomic::ui::pf (6));

    g.setColour (dragging ? kAccent.withAlpha (0.55f) : kFieldEdge);
    g.drawRoundedRectangle (bounds, atomic::ui::pf (6), 1.0f);

    if (editing)
        return;

    g.setColour (kLabel);
    // Phase 50: the caption is the section-label role — small, wide, bold — the
    // hierarchy step DESIGN.md §5 asks for. It used to be a plain 8 pt label
    // indistinguishable from body text.
    g.setFont (atomic::ui::section (8.0f));
    g.drawText ("BPM", atomic::ui::px (8), atomic::ui::px (2),
                atomic::ui::px (34), atomic::ui::px (12), juce::Justification::centredLeft, false);

    g.setColour (kValue);
    // And the value is tabular: 120 → 87 used to shift the last digit by a
    // pixel and, worse, let the field's arrow column drift with it.
    atomic::ui::drawTabular (g, formatBpm(),
                             { (float) atomic::ui::px (8), (float) atomic::ui::px (12),
                               (float) (getWidth() - kArrowColumnWidth - atomic::ui::px (10)),
                               (float) atomic::ui::px (18) },
                             atomic::ui::title (13.0f),
                             juce::Justification::centredLeft);

    // Up / down chevrons.
    auto drawChevron = [&g] (juce::Rectangle<int> area, bool up, bool bright)
    {
        const float cx = (float) area.getCentreX();
        const float cy = (float) area.getCentreY();

        juce::Path p;

        if (up)
            p.addTriangle (cx - atomic::ui::pf (4), cy + atomic::ui::pf (2.5),
                           cx + atomic::ui::pf (4), cy + atomic::ui::pf (2.5),
                           cx, cy - atomic::ui::pf (3));
        else
            p.addTriangle (cx - atomic::ui::pf (4), cy - atomic::ui::pf (2.5),
                           cx + atomic::ui::pf (4), cy - atomic::ui::pf (2.5),
                           cx, cy + atomic::ui::pf (3));

        g.setColour (juce::Colours::white.withAlpha (bright ? 0.85f : 0.32f));
        g.fillPath (p);
    };

    drawChevron (getUpBounds(), true, hoverUp);
    drawChevron (getDownBounds(), false, hoverDown);
}

void BpmField::resized()
{
    editor.setBounds (getDragBounds().reduced (atomic::ui::px (6), atomic::ui::px (3)));
}

//==============================================================================
void BpmField::mouseDown (const juce::MouseEvent& e)
{
    if (editing)
        return;

    const auto pos = e.getPosition();

    // Phase 20: the tempo is a numeric control like any other, so a right click
    // offers the same single verb. "Reset" here means 120 BPM — the tempo a new
    // session starts at, and the one every "how fast is this?" question is
    // answered against.
    if (e.mods.isPopupMenu())
    {
        ui::showValueMenu (this, e.getScreenPosition(), utf8 ("速度"),
                           juce::String (bpm, 1) + " BPM",
                           [this]
                           {
                               applyBpm (defaultBpm, true);
                               repaint();
                           },
                           // Phase 80: the tempo curve has no card, no row and no
                           // obvious owner in the UI, so the field the user is
                           // already pointing at is where the offer belongs.
                           [this] (juce::PopupMenu& menu)
                           {
                               menu.addItem (tempoAutomationMenuId,
                                             utf8 ("显示速度自动化行"), true,
                                             tempoAutomationShown);
                           },
                           [this] (int result)
                           {
                               if (result == tempoAutomationMenuId && onShowTempoAutomation != nullptr)
                                   onShowTempoAutomation();
                           });
        return;
    }

    if (getUpBounds().contains (pos))
    {
        nudge (e.mods.isShiftDown() ? 10.0 : 1.0);
        return;
    }

    if (getDownBounds().contains (pos))
    {
        nudge (e.mods.isShiftDown() ? -10.0 : -1.0);
        return;
    }

    // Body: a drag changes the tempo, a click opens the editor. Which one it
    // was is only known on mouse-up, so record the start and decide then.
    dragging      = true;
    dragMoved     = false;
    dragStartBpm  = bpm;
    dragStartY    = pos.y;
    repaint();
}

void BpmField::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || editing)
        return;

    const int deltaY = dragStartY - e.getPosition().y;

    if (std::abs (deltaY) > 2)
        dragMoved = true;

    if (! dragMoved)
        return;

    // 0.5 BPM per pixel, 0.1 with Shift held for fine control.
    const double perPixel = e.mods.isShiftDown() ? 0.1 : 0.5;
    applyBpm (dragStartBpm + (double) deltaY * perPixel, true);
}

void BpmField::mouseUp (const juce::MouseEvent&)
{
    const bool wasDragging = dragging;
    dragging = false;
    repaint();

    if (wasDragging && ! dragMoved)
        openEditor();
}

void BpmField::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    const bool newHover    = contains (pos);
    const bool newHoverUp  = getUpBounds().contains (pos);
    const bool newHoverDown = getDownBounds().contains (pos);

    setMouseCursor (newHoverUp || newHoverDown ? juce::MouseCursor::PointingHandCursor
                                              : juce::MouseCursor::UpDownResizeCursor);

    if (newHover != hover || newHoverUp != hoverUp || newHoverDown != hoverDown)
    {
        hover     = newHover;
        hoverUp   = newHoverUp;
        hoverDown = newHoverDown;
        repaint();
    }
}

void BpmField::mouseExit (const juce::MouseEvent&)
{
    if (hover || hoverUp || hoverDown)
    {
        hover = hoverUp = hoverDown = false;
        repaint();
    }
}

//==============================================================================
void BpmField::openEditor()
{
    if (editing)
        return;

    editing = true;
    repaint();

    editor.setText (formatBpm(), juce::dontSendNotification);
    editor.setVisible (true);
    editor.toFront (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void BpmField::closeEditor (bool commit)
{
    if (! editing)
        return;

    // onFocusLost can fire while we are hiding the editor; without this guard
    // the two paths re-enter each other.
    editing = false;

    if (commit)
    {
        const double typed = editor.getText().trim().getDoubleValue();

        if (typed > 0.0)
            applyBpm (typed, true);
    }

    editor.setVisible (false);
    repaint();
    grabKeyboardFocus();
}

bool BpmField::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
    {
        const double step = key.getModifiers().isShiftDown() ? 10.0 : 1.0;
        nudge (key == juce::KeyPress::upKey ? step : -step);
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        openEditor();
        return true;
    }

    juce::ignoreUnused (key);
    return false;
}

} // namespace atomic

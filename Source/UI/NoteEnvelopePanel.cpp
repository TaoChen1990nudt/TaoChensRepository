#include "NoteEnvelopePanel.h"

#include "Util/Metrics.h"

#include "Model/Routing.h"
#include "Util/Text.h"

namespace atomic
{

namespace
{
    const juce::Colour kPanelBody   { 0xff1b1d24 };
    const juce::Colour kPanelBorder { 0xff343743 };
    const juce::Colour kEditorBg    { 0xff101116 };
    const juce::Colour kMuted       { 0xff8b91a0 };

    constexpr int kEditorX  = atomic::ui::px (16);
    constexpr int kOutputY  = atomic::ui::px (58);
    constexpr int kOutputH  = atomic::ui::px (28);
    constexpr int kEditorY  = atomic::ui::px (98);
    constexpr int kEditorH  = atomic::ui::px (148);
    constexpr int kPresetY  = atomic::ui::px (256);
    constexpr int kPresetH  = atomic::ui::px (26);
    constexpr int kPresetW  = atomic::ui::px (86);
    constexpr int kPresetGap = atomic::ui::px (8);

    const char* const kPresetLabels[] { "ADSR", "渐弱", "直线" };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }
}

//==============================================================================
//  EnvelopeEditor
//==============================================================================
EnvelopeEditor::EnvelopeEditor()
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void EnvelopeEditor::setEnvelopeSource (std::function<NoteEnvelope*()> source)
{
    envelopeSource = std::move (source);
    draggedIndex = -1;
    hoveredIndex = -1;
    repaint();
}

void EnvelopeEditor::setAccentColour (juce::Colour colour)
{
    accent = colour;
    repaint();
}

void EnvelopeEditor::setPlayheadProgress (double progress)
{
    if (juce::approximatelyEqual (progress, playheadProgress))
        return;

    playheadProgress = progress;
    repaint();
}

NoteEnvelope* EnvelopeEditor::resolve() const
{
    return envelopeSource != nullptr ? envelopeSource() : nullptr;
}

juce::Rectangle<float> EnvelopeEditor::getCurveArea() const
{
    return getLocalBounds().toFloat().reduced (atomic::ui::pf (14), atomic::ui::pf (12));
}

juce::Point<float> EnvelopeEditor::pointToScreen (const EnvelopePoint& point) const
{
    const auto area = getCurveArea();

    return { area.getX() + point.time * area.getWidth(),
             area.getBottom() - point.value * area.getHeight() };
}

EnvelopePoint EnvelopeEditor::screenToPoint (juce::Point<float> position) const
{
    const auto area = getCurveArea();

    if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
        return {};

    return { juce::jlimit (0.0f, 1.0f, (position.x - area.getX()) / area.getWidth()),
             juce::jlimit (0.0f, 1.0f, (area.getBottom() - position.y) / area.getHeight()) };
}

int EnvelopeEditor::findPointAt (juce::Point<float> position) const
{
    auto* envelope = resolve();

    if (envelope == nullptr)
        return -1;

    const auto& points = envelope->getPoints();

    for (int i = 0; i < (int) points.size(); ++i)
        if (pointToScreen (points[(size_t) i]).getDistanceFrom (position) <= 9.0f)
            return i;

    return -1;
}

//==============================================================================
void EnvelopeEditor::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    if (bounds.isEmpty())
        return;

    g.setColour (kEditorBg);
    g.fillRoundedRectangle (bounds, 7.0f);

    const auto curve = getCurveArea();

    //-- Grid -----------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.055f));

    for (int i = 1; i < 4; ++i)
    {
        const float x = curve.getX() + curve.getWidth() * (float) i / 4.0f;
        const float y = curve.getY() + curve.getHeight() * (float) i / 4.0f;

        g.drawVerticalLine ((int) x, curve.getY(), curve.getBottom());
        g.drawHorizontalLine ((int) y, curve.getX(), curve.getRight());
    }

    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawHorizontalLine ((int) curve.getBottom(), curve.getX(), curve.getRight());

    auto* envelope = resolve();

    if (envelope == nullptr)
    {
        g.setColour (kMuted.withAlpha (0.7f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f))));
        g.drawText (utf8 ("没有可编辑的音符"), getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const auto& points = envelope->getPoints();

    if (points.size() < 2)
        return;

    //-- Curve ----------------------------------------------------------------
    juce::Path line, fill;
    const auto first = pointToScreen (points.front());

    line.startNewSubPath (first);
    fill.startNewSubPath (curve.getX(), curve.getBottom());
    fill.lineTo (first);

    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto p = pointToScreen (points[i]);
        line.lineTo (p);
        fill.lineTo (p);
    }

    fill.lineTo (curve.getRight(), curve.getBottom());
    fill.closeSubPath();

    g.setColour (accent.withAlpha (0.16f));
    g.fillPath (fill);

    g.setColour (accent.withAlpha (envelope->isEnabled() ? 0.95f : 0.35f));
    g.strokePath (line, juce::PathStrokeType (1.8f));

    //-- Breakpoints ----------------------------------------------------------
    for (int i = 0; i < (int) points.size(); ++i)
    {
        const bool active = (i == draggedIndex || i == hoveredIndex);
        const float radius = active ? 5.0f : 3.6f;
        const auto centre = pointToScreen (points[(size_t) i]);

        auto dot = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

        g.setColour (kEditorBg);
        g.fillEllipse (dot);
        g.setColour (active ? juce::Colours::white : accent);
        g.drawEllipse (dot, 1.6f);
    }

    //-- Playhead marker ------------------------------------------------------
    // The same curve the piano roll is currently driving, at the same position.
    if (playheadProgress >= 0.0 && playheadProgress <= 1.0)
    {
        const float x = curve.getX() + (float) playheadProgress * curve.getWidth();
        const float v = envelope->valueAt ((float) playheadProgress);
        const float y = curve.getBottom() - juce::jlimit (0.0f, 1.0f, v) * curve.getHeight();

        g.setColour (juce::Colour (0xffff5f6d).withAlpha (0.55f));
        g.drawVerticalLine ((int) x, curve.getY(), curve.getBottom());

        g.setColour (juce::Colour (0xffff5f6d));
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, y }));
    }
}

//==============================================================================
void EnvelopeEditor::mouseDown (const juce::MouseEvent& e)
{
    auto* envelope = resolve();

    if (envelope == nullptr || ! getCurveArea().contains (e.position))
        return;

    const int hit = findPointAt (e.position);

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            envelope->removePoint (hit);
            draggedIndex = -1;
            hoveredIndex = -1;
            repaint();

            if (onChange != nullptr)
                onChange();
        }

        return;
    }

    if (hit >= 0)
    {
        draggedIndex = hit;
    }
    else
    {
        const auto p = screenToPoint (e.position);
        const int index = envelope->addPoint (p.time, p.value);

        if (index < 0)      // envelope full
            return;

        draggedIndex = index;
        hoveredIndex = index;

        if (onChange != nullptr)
            onChange();
    }

    repaint();
}

void EnvelopeEditor::mouseDrag (const juce::MouseEvent& e)
{
    auto* envelope = resolve();

    if (envelope == nullptr || draggedIndex < 0)
        return;

    const auto p = screenToPoint (e.position);
    envelope->movePoint (draggedIndex, p.time, p.value);
    repaint();

    if (onChange != nullptr)
        onChange();
}

void EnvelopeEditor::mouseUp (const juce::MouseEvent&)
{
    if (draggedIndex >= 0)
    {
        draggedIndex = -1;
        repaint();
    }
}

void EnvelopeEditor::mouseMove (const juce::MouseEvent& e)
{
    const int newHover = findPointAt (e.position);

    if (newHover != hoveredIndex)
    {
        hoveredIndex = newHover;
        setMouseCursor (newHover >= 0 ? juce::MouseCursor::DraggingHandCursor
                                      : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void EnvelopeEditor::mouseExit (const juce::MouseEvent&)
{
    if (hoveredIndex != -1)
    {
        hoveredIndex = -1;
        repaint();
    }
}

//==============================================================================
//  NoteEnvelopePanel
//==============================================================================
NoteEnvelopePanel::NoteEnvelopePanel (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    setVisible (false);

    addAndMakeVisible (editor);

    editor.setEnvelopeSource ([this]() -> NoteEnvelope*
    {
        if (auto* note = getNote())
            return &note->envelope;

        return nullptr;
    });

    // Any edit inside the curve must reach the piano roll's mini preview.
    editor.onChange = [this]()
    {
        stack.notifyNotesChanged();
    };

    stack.addListener (this);
    rack.addListener (this);
}

NoteEnvelopePanel::~NoteEnvelopePanel()
{
    stack.removeListener (this);
    rack.removeListener (this);
}

//==============================================================================
NoteEvent* NoteEnvelopePanel::getNote() const
{
    if (layerIndex < 0 || noteId < 0)
        return nullptr;

    if (auto* layer = stack.getLayer (layerIndex))
        return layer->findNote (noteId);

    return nullptr;
}

void NoteEnvelopePanel::openFor (int newLayerIndex, int newNoteId)
{
    auto* layer = stack.getLayer (newLayerIndex);

    if (layer == nullptr || layer->findNote (newNoteId) == nullptr)
        return;

    layerIndex = newLayerIndex;
    noteId = newNoteId;

    editor.setAccentColour (layer->getColour());
    editor.repaint();

    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    repaint();
}

void NoteEnvelopePanel::setPlayheadBeat (double beats)
{
    if (juce::approximatelyEqual (beats, playheadBeat))
        return;

    playheadBeat = beats;

    auto* note = getNote();
    const double progress = (isOpen() && note != nullptr) ? note->progressAt (beats) : -1.0;

    editor.setPlayheadProgress (progress);
}

void NoteEnvelopePanel::close()
{
    if (! isOpen() && ! isVisible())
        return;

    layerIndex = -1;
    noteId = -1;
    setVisible (false);

    if (onClosed != nullptr)
        onClosed();
}

//==============================================================================
void NoteEnvelopePanel::layersChanged()
{
    if (! isOpen())
        return;

    // Only the active layer is editable; switching sheets must not leave this
    // panel editing a note the user can no longer see.
    if (stack.getActiveLayerIndex() != layerIndex || getNote() == nullptr)
    {
        close();
        return;
    }

    repaint();
}

void NoteEnvelopePanel::rackChanged()
{
    if (isOpen())
        repaint();
}

void NoteEnvelopePanel::notesChanged()
{
    if (! isOpen())
        return;

    if (getNote() == nullptr)   // the note was deleted
    {
        close();
        return;
    }

    editor.repaint();
    repaint();
}

//==============================================================================
void NoteEnvelopePanel::resized()
{
    editor.setBounds (kEditorX, kEditorY, getWidth() - kEditorX * 2, kEditorH);
}

juce::Rectangle<int> NoteEnvelopePanel::getCloseBounds() const
{
    return { getWidth() - 38, 10, 26, 26 };
}

juce::Rectangle<int> NoteEnvelopePanel::getPresetBounds (int index) const
{
    return { kEditorX + index * (kPresetW + kPresetGap), kPresetY, kPresetW, kPresetH };
}

juce::Rectangle<int> NoteEnvelopePanel::getOutputBounds() const
{
    return { atomic::ui::px (64), kOutputY,
             juce::jmax (atomic::ui::px (100), getWidth() - atomic::ui::px (248)), kOutputH };
}

//==============================================================================
void NoteEnvelopePanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Drop shadow so the panel reads as floating above the roll.
    g.setColour (juce::Colours::black.withAlpha (0.50f));
    g.fillRoundedRectangle (bounds.reduced (3.0f).translated (0.0f, 5.0f), 12.0f);

    g.setColour (kPanelBody);
    g.fillRoundedRectangle (bounds.reduced (1.0f), 12.0f);
    g.setColour (kPanelBorder);
    g.drawRoundedRectangle (bounds.reduced (1.0f), 12.0f, 1.0f);

    auto* note  = getNote();
    auto* layer = stack.getLayer (layerIndex);
    const auto accent = layer != nullptr ? layer->getColour() : juce::Colour (0xff4fd1c5);

    //-- Header ---------------------------------------------------------------
    g.setColour (accent);
    g.fillEllipse (18.0f, 15.0f, 8.0f, 8.0f);

    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.setFont (boldFont (13.5f));
    g.drawText (utf8 ("音符包络"), atomic::ui::px (34), atomic::ui::px (9),
                atomic::ui::px (200), atomic::ui::px (20),
                juce::Justification::centredLeft, false);

    //-- Identity readout ------------------------------------------------------
    juce::String meta;

    if (note != nullptr)
    {
        meta << "id " << note->id
             << separatorDot() << (layer != nullptr ? layer->getName() : juce::String ("-"))
             << separatorDot() << noteName (note->pitch)
             << separatorDot() << "beat " << juce::String (note->startBeat, 2)
             << separatorDot() << "len " << juce::String (note->lengthBeats, 2)
             << separatorDot() << juce::String ((int) note->envelope.getPoints().size()) << " pts";
    }
    else
    {
        meta = utf8 ("未选中音符");
    }

    g.setColour (kMuted.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (meta, atomic::ui::px (20), atomic::ui::px (34),
                getWidth() - atomic::ui::px (70), atomic::ui::px (16),
                juce::Justification::centredLeft, false);

    //-- Output routing --------------------------------------------------------
    {
        routing::Resolution route;
        juce::String slotName = rack.getSlotName (-1);
        juce::Colour slotColour = juce::Colours::grey;

        if (note != nullptr && layer != nullptr)
        {
            route = routing::resolve (*layer, *note, rack);
            slotName = rack.getSlotName (route.slotId);
            slotColour = rack.getSlotColour (route.slotId);
        }

        g.setColour (kMuted.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("输出"), atomic::ui::px (20), kOutputY + atomic::ui::px (7),
                    atomic::ui::px (40), atomic::ui::px (14),
                    juce::Justification::centredLeft, false);

        auto pill = getOutputBounds().toFloat();

        g.setColour (juce::Colours::white.withAlpha (hoveringOutput ? 0.11f : 0.055f));
        g.fillRoundedRectangle (pill, 6.0f);
        g.setColour (slotColour.withAlpha (hoveringOutput ? 0.80f : 0.45f));
        g.drawRoundedRectangle (pill, 6.0f, 1.0f);

        g.setColour (slotColour);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ pill.getX() + 14.0f, pill.getCentreY() }));

        g.setColour (juce::Colours::white.withAlpha (0.90f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
        g.drawText (slotName, pill.reduced (atomic::ui::pf (26), 0.0f)
                                  .withTrimmedRight (atomic::ui::pf (14)),
                    juce::Justification::centredLeft, false);

        {
            juce::Path caret;
            const float cx = pill.getRight() - 13.0f;
            const float cy = pill.getCentreY() - 1.0f;
            caret.addTriangle (cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
            g.setColour (juce::Colours::white.withAlpha (hoveringOutput ? 0.80f : 0.40f));
            g.fillPath (caret);
        }

        // Badge: why this note goes there.
        const juce::String badge = route.overridden   ? utf8 ("越级指派")
                                 : route.danglingOverride ? utf8 ("越级失效")
                                                          : utf8 ("跟随图层");

        const juce::Colour badgeColour = route.overridden   ? slotColour
                                       : route.danglingOverride ? juce::Colour (0xffff6b6b)
                                                                : kMuted;

        g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (9.5f)).withStyle ("Bold")));
        g.setColour (badgeColour.withAlpha (route.overridden ? 1.0f : 0.65f));
        g.drawText (badge,
                    juce::Rectangle<int> (atomic::ui::px (276), kOutputY + atomic::ui::px (4),
                                          getWidth() - atomic::ui::px (292), atomic::ui::px (20)),
                    juce::Justification::centredRight, false);
    }

    //-- Close button ---------------------------------------------------------
    {
        auto closeBounds = getCloseBounds().toFloat();

        if (hoveringClose)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (closeBounds, 6.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (hoveringClose ? 0.95f : 0.5f));
        g.drawLine (closeBounds.getX() + 9.0f, closeBounds.getY() + 9.0f,
                    closeBounds.getRight() - 9.0f, closeBounds.getBottom() - 9.0f, 1.4f);
        g.drawLine (closeBounds.getRight() - 9.0f, closeBounds.getY() + 9.0f,
                    closeBounds.getX() + 9.0f, closeBounds.getBottom() - 9.0f, 1.4f);
    }

    //-- Presets --------------------------------------------------------------
    for (int i = 0; i < 3; ++i)
    {
        auto button = getPresetBounds (i).toFloat();
        const bool hovered = (i == hoveredPreset);

        g.setColour (juce::Colours::white.withAlpha (hovered ? 0.10f : 0.05f));
        g.fillRoundedRectangle (button, 5.0f);

        g.setColour (accent.withAlpha (hovered ? 0.55f : 0.28f));
        g.drawRoundedRectangle (button, 5.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (hovered ? 0.95f : 0.65f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
        g.drawText (utf8 (kPresetLabels[i]), button, juce::Justification::centred, false);
    }

    //-- Hint -----------------------------------------------------------------
    g.setColour (kMuted.withAlpha (0.55f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
    g.drawText (utf8 ("点击空白加点  ·  拖动调整  ·  右键删点  ·  Esc 关闭  ·  Ctrl+D 复制音符"),
                kEditorX, atomic::ui::px (292), getWidth() - kEditorX * 2, atomic::ui::px (14),
                juce::Justification::centredLeft, false);
}

//==============================================================================
void NoteEnvelopePanel::applyPreset (int index)
{
    auto* note = getNote();

    if (note == nullptr)
        return;

    switch (index)
    {
        case 1:  note->envelope = NoteEnvelope::makeFadeOut(); break;
        case 2:  note->envelope = NoteEnvelope::makeFlat();    break;
        default: note->envelope = NoteEnvelope::makeADSR();    break;
    }

    editor.repaint();
    stack.notifyNotesChanged();
}

//==============================================================================
void NoteEnvelopePanel::showOutputMenu()
{
    auto* note  = getNote();
    auto* layer = stack.getLayer (layerIndex);

    if (note == nullptr || layer == nullptr || rack.getNumSlots() == 0)
        return;

    const int layerDefault = routing::layerSlotId (*layer, rack);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("音符输出"));
    menu.addItem (1, utf8 ("跟随图层  →  ") + rack.getSlotName (layerDefault),
                  true, note->outputSlotOverride < 0);
    menu.addSeparator();

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i))
            menu.addItem (100 + slot->id, slot->name, true, note->outputSlotOverride == slot->id);

    menu.addSeparator();
    menu.addItem (2, utf8 ("清除越级指派"), note->outputSlotOverride >= 0);

    juce::Component::SafePointer<NoteEnvelopePanel> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                          .withTargetComponent (this)
                          .withTargetScreenArea (localAreaToGlobal (getOutputBounds())),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            auto* n = safeThis->getNote();

                            if (n == nullptr)
                                return;

                            if (result == 1)
                                n->outputSlotOverride = -1;
                            else if (result >= 100)
                                n->outputSlotOverride = result - 100;
                            else
                                return;                        // "clear" on a note that has none

                            // Picking the layer's own default is not a bypass —
                            // normalise it back to "follow", so the badge and the
                            // data cannot disagree.
                            if (auto* l = safeThis->stack.getLayer (safeThis->layerIndex))
                                if (n->outputSlotOverride == routing::layerSlotId (*l, safeThis->rack))
                                    n->outputSlotOverride = -1;

                            safeThis->stack.notifyNotesChanged();
                        });
}

//==============================================================================
void NoteEnvelopePanel::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (getCloseBounds().contains (pos))
    {
        close();
        return;
    }

    if (getOutputBounds().contains (pos))
    {
        showOutputMenu();
        return;
    }

    for (int i = 0; i < 3; ++i)
        if (getPresetBounds (i).contains (pos))
        {
            applyPreset (i);
            return;
        }
}

void NoteEnvelopePanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    int newPreset = -1;

    for (int i = 0; i < 3; ++i)
        if (getPresetBounds (i).contains (pos))
            newPreset = i;

    const bool newClose = getCloseBounds().contains (pos);
    const bool newOutput = getOutputBounds().contains (pos);

    setMouseCursor ((newPreset >= 0 || newClose || newOutput) ? juce::MouseCursor::PointingHandCursor
                                                              : juce::MouseCursor::NormalCursor);

    if (newPreset != hoveredPreset || newClose != hoveringClose || newOutput != hoveringOutput)
    {
        hoveredPreset = newPreset;
        hoveringClose = newClose;
        hoveringOutput = newOutput;
        repaint();
    }
}

void NoteEnvelopePanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPreset != -1 || hoveringClose || hoveringOutput)
    {
        hoveredPreset = -1;
        hoveringClose = false;
        hoveringOutput = false;
        repaint();
    }
}

bool NoteEnvelopePanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }

    return false;
}

} // namespace atomic

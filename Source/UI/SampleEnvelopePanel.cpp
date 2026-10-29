#include "SampleEnvelopePanel.h"

#include "Util/Metrics.h"

#include "Model/Routing.h"
#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{

namespace
{
    const juce::Colour& kPanelBody   = theme::surface3;
    const juce::Colour& kPanelBorder = theme::hairlineStrong;
    const juce::Colour& kMuted       = theme::inkSubtle;

    // The note panel's layout, to the pixel: the two windows sit in the same
    // corner of the screen and a row that lands 2 px off between them reads as
    // a different kind of window.
    constexpr int kEditorX   = atomic::ui::px (16);
    constexpr int kOutputY   = atomic::ui::px (58);
    constexpr int kOutputH   = atomic::ui::px (28);
    constexpr int kEditorY   = atomic::ui::px (98);
    constexpr int kEditorH   = atomic::ui::px (148);
    constexpr int kPresetY   = atomic::ui::px (256);
    constexpr int kPresetH   = atomic::ui::px (26);
    constexpr int kPresetW   = atomic::ui::px (86);
    constexpr int kPresetGap = atomic::ui::px (8);

    constexpr int kTargetW   = atomic::ui::px (46);
    constexpr int kTargetH   = atomic::ui::px (24);
    constexpr int kTargetGap = atomic::ui::px (4);

    const char* const kPresetLabels[] { "ADSR", "渐弱", "直线" };
    const char* const kTargetLabels[] { "音量", "声相" };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }
}

//==============================================================================
SampleEnvelopePanel::SampleEnvelopePanel (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    setVisible (false);

    addAndMakeVisible (editor);

    // Resolved on every access, through the SAME `activeCurve()` the renderer
    // and the floating box use. Caching a NoteEnvelope* would dangle the moment
    // a clip is added or removed — clips live in a vector.
    editor.setEnvelopeSource ([this]() -> NoteEnvelope*
    {
        if (auto* sample = getSample())
            return &sample->activeCurve();

        return nullptr;
    });

    editor.setEmptyText (utf8 ("没有可编辑的采样块"));

    editor.onChange = [this]()
    {
        stack.notifyNotesChanged();
    };

    stack.addListener (this);
    rack.addListener (this);
}

SampleEnvelopePanel::~SampleEnvelopePanel()
{
    stack.removeListener (this);
    rack.removeListener (this);
}

//==============================================================================
SampleEvent* SampleEnvelopePanel::getSample() const
{
    if (layerIndex < 0 || sampleId < 0)
        return nullptr;

    if (auto* layer = stack.getLayer (layerIndex))
        return layer->findSample (sampleId);

    return nullptr;
}

//==============================================================================
int SampleEnvelopePanel::getTargetOrdinal() const noexcept
{
    if (auto* sample = getSample())
        return sample->curveTarget() == (int) SampleAutomationTarget::pan ? 1 : 0;

    return 0;
}

void SampleEnvelopePanel::setTargetOrdinal (int ordinal)
{
    auto* sample = getSample();

    if (sample == nullptr)
        return;

    const auto target = ordinal == 1 ? SampleAutomationTarget::pan
                                     : SampleAutomationTarget::volume;

    // Phase 82: the box shows a curve, so "switch to pan" means "show the curve
    // that drives pan". Doing that is a no-op when the block has no pan curve —
    // which is the honest answer: there is nothing to switch to.
    const int before = sample->curves.getActiveId();
    sample->showCurveDriving (target);

    if (sample->curves.getActiveId() == before)
    {
        repaint();
        return;
    }

    // Phase 65's rule, applied to the window: asking to edit a curve is asking
    // to use it. A curve left disabled by an older project would otherwise be
    // shown here as a shape that does nothing, with the switch that used to
    // bring it back gone from the UI.
    if (! sample->activeCurve().isEnabled())
        sample->activeCurve().setEnabled (true);

    updateCaption();
    editor.repaint();
    stack.notifyNotesChanged();
    repaint();
}

//==============================================================================
void SampleEnvelopePanel::applyPreset (int index)
{
    auto* sample = getSample();

    if (sample == nullptr)
        return;

    auto& curve = sample->activeCurve();

    switch (index)
    {
        case 1:
            curve = NoteEnvelope::makeFadeOut();
            break;

        case 2:
            // "Flat" has two meanings and the target decides which one: unity
            // for level, centre for pan. Flattening a pan curve to 1.0 would
            // put the clip hard right under a button labelled "直线".
            curve = sample->curveTarget() == (int) SampleAutomationTarget::pan
                      ? NoteEnvelope::makeIdle (0.5f)
                      : NoteEnvelope::makeFlat();
            break;

        default:
            curve = NoteEnvelope::makeADSR();
            break;
    }

    // A preset is a shape you want to HEAR, so it also switches the curve on.
    curve.setEnabled (true);

    editor.repaint();
    stack.notifyNotesChanged();
    repaint();
}

//==============================================================================
void SampleEnvelopePanel::openFor (int newLayerIndex, int newSampleId)
{
    auto* layer = stack.getLayer (newLayerIndex);

    if (layer == nullptr)
        return;

    auto* sample = layer->findSample (newSampleId);

    if (sample == nullptr)
        return;

    layerIndex = newLayerIndex;
    sampleId   = newSampleId;

    // Opening the window is asking to EDIT this curve, and a disabled curve is
    // drawn as a dim shape that does nothing. The floating box already brings a
    // bypassed curve back (P49/P65); doing it only there would mean the window
    // shows a curve the block is not playing.
    if (! sample->activeCurve().isEnabled())
        sample->activeCurve().setEnabled (true);

    editor.setAccentColour (layer->getColour());
    updateCaption();
    editor.repaint();

    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    repaint();
}

void SampleEnvelopePanel::close()
{
    if (! isOpen() && ! isVisible())
        return;

    layerIndex = -1;
    sampleId   = -1;
    setVisible (false);

    if (onClosed != nullptr)
        onClosed();
}

//==============================================================================
void SampleEnvelopePanel::layersChanged()
{
    if (! isOpen())
        return;

    // Only the active layer is editable; switching sheets must not leave a
    // window editing a block the user can no longer see (same rule as the note
    // panel, and the same reason: the block is addressed by layer).
    if (stack.getActiveLayerIndex() != layerIndex || getSample() == nullptr)
    {
        close();
        return;
    }

    repaint();
}

void SampleEnvelopePanel::notesChanged()
{
    if (! isOpen())
        return;

    if (getSample() == nullptr)     // the block was deleted
    {
        close();
        return;
    }

    editor.repaint();
    repaint();
}

void SampleEnvelopePanel::rackChanged()
{
    if (! isOpen())
        return;

    // The output row prints a slot name and a badge that depend on the rack.
    repaint();
}

//==============================================================================
void SampleEnvelopePanel::resized()
{
    editor.setBounds (kEditorX, kEditorY, getWidth() - kEditorX * 2, kEditorH);
}

juce::Rectangle<int> SampleEnvelopePanel::getCloseBounds() const
{
    return { getWidth() - atomic::ui::px (38), atomic::ui::px (10),
             atomic::ui::px (26), atomic::ui::px (26) };
}

juce::Rectangle<int> SampleEnvelopePanel::getTargetBounds (int ordinal) const
{
    const int right = getWidth() - atomic::ui::px (46);      // clear of the close button
    const int x     = right - (2 - ordinal) * (kTargetW + kTargetGap);

    return { x, atomic::ui::px (9), kTargetW, kTargetH };
}

juce::Rectangle<int> SampleEnvelopePanel::getPresetBounds (int index) const
{
    return { kEditorX + index * (kPresetW + kPresetGap), kPresetY, kPresetW, kPresetH };
}

juce::Rectangle<int> SampleEnvelopePanel::getOutputBounds() const
{
    return { atomic::ui::px (64), kOutputY, getWidth() - atomic::ui::px (172), kOutputH };
}

//==============================================================================
void SampleEnvelopePanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colours::black.withAlpha (0.50f));
    g.fillRoundedRectangle (bounds.reduced (3.0f).translated (0.0f, 5.0f), 12.0f);

    g.setColour (kPanelBody);
    g.fillRoundedRectangle (bounds.reduced (1.0f), 12.0f);
    g.setColour (kPanelBorder);
    g.drawRoundedRectangle (bounds.reduced (1.0f), 12.0f, 1.0f);

    auto* sample = getSample();
    auto* layer  = stack.getLayer (layerIndex);
    const auto accent = layer != nullptr ? layer->getColour() : theme::accent;

    //-- Header ---------------------------------------------------------------
    g.setColour (accent);
    g.fillEllipse (18.0f, 15.0f, 8.0f, 8.0f);

    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.setFont (boldFont (13.5f));
    g.drawText (utf8 ("采样包络"), atomic::ui::px (34), atomic::ui::px (9),
                atomic::ui::px (200), atomic::ui::px (20),
                juce::Justification::centredLeft, false);

    //-- Identity readout ------------------------------------------------------
    juce::String meta;

    if (sample != nullptr)
    {
        meta << "id " << sample->id
             << separatorDot() << (layer != nullptr ? layer->getName() : juce::String ("-"))
             << separatorDot() << sample->name
             << separatorDot() << "beat " << juce::String (sample->startBeat, 2)
             << separatorDot() << "len " << juce::String (sample->lengthBeats, 2)
             << separatorDot() << juce::String ((int) sample->activeCurve().getPoints().size())
             << " pts";
    }
    else
    {
        meta = utf8 ("未选中采样块");
    }

    g.setColour (kMuted.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (meta, atomic::ui::px (20), atomic::ui::px (34),
                getWidth() - atomic::ui::px (180), atomic::ui::px (16),
                juce::Justification::centredLeft, false);

    //-- Which curve -----------------------------------------------------------
    // Two pills in the header rather than a menu: a clip only has two curves,
    // and "which one am I looking at" has to be answerable without opening
    // anything.
    for (int i = 0; i < 2; ++i)
    {
        auto pill = getTargetBounds (i).toFloat();
        const bool active  = (getTargetOrdinal() == i);
        const bool hovered = (hoveredTarget == i);

        g.setColour (active ? accent.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.10f : 0.045f));
        g.fillRoundedRectangle (pill, 6.0f);

        g.setColour (active ? accent.withAlpha (0.85f)
                            : juce::Colours::white.withAlpha (hovered ? 0.30f : 0.14f));
        g.drawRoundedRectangle (pill, 6.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (active ? 0.98f : 0.55f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 (kTargetLabels[i]), pill, juce::Justification::centred, false);
    }

    //-- Output routing --------------------------------------------------------
    {
        routing::SampleResolution route;
        juce::String slotName = rack.getSlotName (-1);
        juce::Colour slotColour = juce::Colours::grey;

        if (sample != nullptr && layer != nullptr)
        {
            route = routing::resolveSample (*layer, *sample, rack);
            slotName   = rack.getSlotName (route.slotId);
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
        g.drawText (slotName, pill.reduced (atomic::ui::pf (26), 0.0f).withTrimmedRight (atomic::ui::pf (14)),
                    juce::Justification::centredLeft, false);

        {
            juce::Path caret;
            const float cx = pill.getRight() - 13.0f;
            const float cy = pill.getCentreY() - 1.0f;
            caret.addTriangle (cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
            g.setColour (juce::Colours::white.withAlpha (hoveringOutput ? 0.80f : 0.40f));
            g.fillPath (caret);
        }

        // Badge: why this block goes there. `resolveSample` decides, not this
        // painter — a clip mounted on a sampler says so, and one whose sampler
        // was deleted says that instead of silently showing the layer's bus.
        const juce::String badge = route.danglingMount ? utf8 ("采样器失效")
                                  : route.viaSampler  ? utf8 ("采样器")
                                                      : utf8 ("跟随图层");

        const juce::Colour badgeColour = route.danglingMount ? theme::record
                                      : route.viaSampler   ? slotColour
                                                           : kMuted;

        g.setFont (juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (9.5f)).withStyle ("Bold")));
        g.setColour (badgeColour.withAlpha (route.viaSampler ? 1.0f : 0.65f));
        g.drawText (badge,
                    juce::Rectangle<int> (getWidth() - atomic::ui::px (104), kOutputY + atomic::ui::px (4),
                                          atomic::ui::px (84), atomic::ui::px (20)),
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
    g.drawText (utf8 ("点击空白加点  ·  拖动调整  ·  右键删点  ·  Esc 关闭"),
                kEditorX, atomic::ui::px (292), getWidth() - kEditorX * 2, atomic::ui::px (14),
                juce::Justification::centredLeft, false);
}

//==============================================================================
void SampleEnvelopePanel::updateCaption()
{
    editor.setCaptionName (getTargetOrdinal() == 1 ? utf8 ("声相") : utf8 ("音量"));
}

void SampleEnvelopePanel::showOutputMenu()
{
    auto* sample = getSample();
    auto* layer  = stack.getLayer (layerIndex);

    if (sample == nullptr || layer == nullptr || rack.getNumSlots() == 0)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("采样块输出"));
    menu.addItem (1, utf8 ("跟随图层  →  ") + rack.getSlotName (routing::layerSlotId (*layer, rack)),
                  true, sample->outputSlotId < 0);
    menu.addSeparator();

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i))
            menu.addItem (100 + slot->id, slot->name, true, sample->outputSlotId == slot->id);

    juce::Component::SafePointer<SampleEnvelopePanel> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                          .withTargetComponent (this)
                          .withTargetScreenArea (localAreaToGlobal (getOutputBounds())),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            auto* s = safeThis->getSample();

                            if (s == nullptr)
                                return;

                            if (result == 1)
                                s->outputSlotId = -1;
                            else if (result >= 100)
                                s->outputSlotId = result - 100;
                            else
                                return;

                            // Picking the layer's own default is not a mount —
                            // normalise it back to "follow", so the badge and
                            // the data cannot disagree.
                            if (auto* l = safeThis->stack.getLayer (safeThis->layerIndex))
                                if (s->outputSlotId == routing::layerSlotId (*l, safeThis->rack))
                                    s->outputSlotId = -1;

                            safeThis->stack.notifyNotesChanged();
                        });
}

//==============================================================================
void SampleEnvelopePanel::mouseDown (const juce::MouseEvent& e)
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

    for (int i = 0; i < 2; ++i)
        if (getTargetBounds (i).contains (pos))
        {
            setTargetOrdinal (i);
            return;
        }

    for (int i = 0; i < 3; ++i)
        if (getPresetBounds (i).contains (pos))
        {
            applyPreset (i);
            return;
        }
}

void SampleEnvelopePanel::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    int newPreset = -1;
    int newTarget = -1;

    for (int i = 0; i < 3; ++i)
        if (getPresetBounds (i).contains (pos))
            newPreset = i;

    for (int i = 0; i < 2; ++i)
        if (getTargetBounds (i).contains (pos))
            newTarget = i;

    const bool newClose  = getCloseBounds().contains (pos);
    const bool newOutput = getOutputBounds().contains (pos);

    setMouseCursor ((newPreset >= 0 || newTarget >= 0 || newClose || newOutput)
                      ? juce::MouseCursor::PointingHandCursor
                      : juce::MouseCursor::NormalCursor);

    if (newPreset != hoveredPreset || newTarget != hoveredTarget
         || newClose != hoveringClose || newOutput != hoveringOutput)
    {
        hoveredPreset  = newPreset;
        hoveredTarget  = newTarget;
        hoveringClose  = newClose;
        hoveringOutput = newOutput;
        repaint();
    }
}

void SampleEnvelopePanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredPreset != -1 || hoveredTarget != -1 || hoveringClose || hoveringOutput)
    {
        hoveredPreset  = -1;
        hoveredTarget  = -1;
        hoveringClose  = false;
        hoveringOutput = false;
        repaint();
    }
}

bool SampleEnvelopePanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }

    return false;
}

} // namespace atomic

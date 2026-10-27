#include "EffectWindows.h"

#include "Util/Metrics.h"

#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{

namespace
{
    const juce::Colour& kBackground = theme::surface1;
    const juce::Colour& kPanel     = theme::surface3;
    const juce::Colour& kText      = theme::inkMuted;
    const juce::Colour& kAccent    = theme::accent;
}

juce::String InsertEditorContent::effectName (fx::Type type)
{
    switch (type)
    {
        case fx::Type::lowPass: return utf8 ("低通滤波");
        case fx::Type::drive:   return utf8 ("驱动");
        case fx::Type::delay:   return utf8 ("延迟");
        case fx::Type::air:     return utf8 ("空气感");
        case fx::Type::eq:      return utf8 ("均衡");
        case fx::Type::comp:    return utf8 ("压缩");
        case fx::Type::reverb:  return utf8 ("混响");
        case fx::Type::vst:     return "VST3";
        case fx::Type::none:
        default:                return utf8 ("空");
    }
}

int InsertEditorContent::preferredHeightFor (int numParams)
{
    // Header band ends at 54; each knob row takes 46; the footer needs ~30.
    return atomic::ui::px (62) + numParams * atomic::ui::px (46) + atomic::ui::px (34);
}

//==============================================================================
PluginEditorWindow::PluginEditorWindow (juce::AudioPluginInstance& processor,
                                        const juce::String& pluginName,
                                        int instanceIdToShow)
    : juce::DocumentWindow (utf8 ("AtomicDAW · ") + pluginName, kBackground,
                            juce::DocumentWindow::closeButton
                              | juce::DocumentWindow::minimiseButton),
      instanceId (instanceIdToShow)
{
    setUsingNativeTitleBar (true);

    editor = processor.createEditorIfNeeded();

    if (editor != nullptr)
    {
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);

        const int w = juce::jmax (320, editor->getWidth());
        const int h = juce::jmax (180, editor->getHeight());

        setResizeLimits (juce::jmax (240, w / 2), juce::jmax (140, h / 2), 2600, 1600);
        centreWithSize (w, h);
    }
    else
    {
        // A plugin is allowed to have no UI. Saying so is better than a window
        // that opens at 0x0 and looks like a crash.
        auto* label = new juce::Label ({}, utf8 ("这个插件没有提供界面，参数请在插件管理器里调整"));
        label->setJustificationType (juce::Justification::centred);
        label->setColour (juce::Label::textColourId, kText);
        label->setColour (juce::Label::backgroundColourId, kBackground);
        setContentOwned (label, true);
        centreWithSize (atomic::ui::px (420), atomic::ui::px (120));
    }

    setVisible (true);
}

PluginEditorWindow::~PluginEditorWindow()
{
    // The editor is owned by the content slot. Clear it explicitly so the
    // processor is told the editor is gone before this window stops existing.
    clearContentComponent();
}

void PluginEditorWindow::setKeyForwarder (juce::KeyListener* listener)
{
    keyForwarder = listener;

    if (listener == nullptr)
        return;

    addKeyListener (listener);

    if (auto* content = getContentComponent())
        content->addKeyListener (listener);
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onCloseRequest != nullptr)
        onCloseRequest();
}

//==============================================================================
InsertEditorContent::InsertEditorContent (MixerModel& mixerModel, int channelToUse, int slotToUse)
    : mixer (mixerModel), channel (channelToUse), slot (slotToUse)
{
    setOpaque (true);
    startTimerHz (15);
}

int InsertEditorContent::getNumParams() const
{
    return fx::numParams (mixer.getInsert (channel, slot).type);
}

float InsertEditorContent::getParam (int index) const
{
    return mixer.getInsert (channel, slot).params[(size_t) juce::jlimit (0, fx::maxParams - 1, index)];
}

void InsertEditorContent::setParam (int index, float value)
{
    mixer.setInsertParam (channel, slot, index, value);
}

juce::String InsertEditorContent::getParamName (int index) const
{
    return fx::paramName (mixer.getInsert (channel, slot).type, index);
}

juce::Rectangle<int> InsertEditorContent::getParamRowBounds (int index) const
{
    return { atomic::ui::px (20), atomic::ui::px (62) + index * atomic::ui::px (46),
             getWidth() - atomic::ui::px (40), atomic::ui::px (36) };
}

juce::Rectangle<int> InsertEditorContent::getBarBounds (int index) const
{
    auto row = getParamRowBounds (index);

    return row.withTrimmedLeft (atomic::ui::px (72)).withTrimmedRight (atomic::ui::px (104))
              .withHeight (atomic::ui::px (8)).withY (row.getCentreY() - atomic::ui::px (4));
}

juce::Rectangle<int> InsertEditorContent::getPowerBounds() const
{
    return { getWidth() - atomic::ui::px (78), atomic::ui::px (14),
             atomic::ui::px (58), atomic::ui::px (20) };
}

void InsertEditorContent::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    const auto& insert = mixer.getInsert (channel, slot);
    const bool on = insert.enabled && insert.type != fx::Type::none;

    //-- Header ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (15.0f)).withStyle ("Bold")));
    g.drawText (effectName (insert.type),
                juce::Rectangle<int> (atomic::ui::px (20), atomic::ui::px (12),
                                      atomic::ui::px (240), atomic::ui::px (24)),
                juce::Justification::centredLeft, false);

    g.setColour (kText.withAlpha (0.7f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
    g.drawText (utf8 ("通道 ") + juce::String (channel + 1)
                  + utf8 (" · 插槽 ") + juce::String (slot + 1)
                  + utf8 (" · 内置效果器"),
                juce::Rectangle<int> (atomic::ui::px (21), atomic::ui::px (34),
                                      atomic::ui::px (320), atomic::ui::px (14)),
                juce::Justification::centredLeft, false);

    //-- Bypass pill ----------------------------------------------------------
    {
        const auto pill = getPowerBounds().toFloat();

        g.setColour (on ? kAccent.withAlpha (0.20f) : juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (pill, atomic::ui::pf (4));
        g.setColour (on ? kAccent.withAlpha (0.7f) : juce::Colours::white.withAlpha (0.16f));
        g.drawRoundedRectangle (pill, atomic::ui::pf (4), 1.0f);

        g.setColour (on ? kAccent : juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (on ? utf8 ("启用") : utf8 ("旁通"),
                    getPowerBounds(), juce::Justification::centred, false);
    }

    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine (atomic::ui::px (54), atomic::ui::pf (16),
                          (float) getWidth() - atomic::ui::pf (16));

    //-- Parameters -----------------------------------------------------------
    const int numParams = getNumParams();

    for (int i = 0; i < numParams; ++i)
    {
        auto row = getParamRowBounds (i);
        const float value = getParam (i);

        g.setColour (kText.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
        g.drawText (getParamName (i),
                    row.withWidth (atomic::ui::px (68)), juce::Justification::centredLeft, false);

        auto bar = getBarBounds (i).toFloat();

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (bar, atomic::ui::pf (4));

        g.setColour (kAccent.withAlpha (on ? 0.85f : 0.30f));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (atomic::ui::pf (6), bar.getWidth() * value)),
                                atomic::ui::pf (4));

        // Handle at the end of the fill: shows where the value is even when the
        // bar is nearly empty.
        const float x = bar.getX() + bar.getWidth() * value;
        g.setColour (juce::Colours::white.withAlpha (on ? 0.85f : 0.4f));
        g.fillEllipse (x - atomic::ui::pf (6), bar.getCentreY() - atomic::ui::pf (6),
                       atomic::ui::pf (12), atomic::ui::pf (12));

        g.setColour (juce::Colours::white.withAlpha (0.82f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.0f))));
        g.drawText (fx::paramText (insert.type, i, value),
                    row.withTrimmedLeft (row.getWidth() - atomic::ui::px (100))
                       .withWidth (atomic::ui::px (100)),
                    juce::Justification::centredRight, false);
    }

    //-- Footer ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.28f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
    g.drawText (utf8 ("拖动参数条改变数值 · 关闭窗口不会卸载效果器"),
                juce::Rectangle<int> (20, getHeight() - 24, getWidth() - 40, 16),
                juce::Justification::centredLeft, false);
}

void InsertEditorContent::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    if (getPowerBounds().contains (pos))
    {
        const auto& insert = mixer.getInsert (channel, slot);

        if (insert.type != fx::Type::none)
            mixer.setInsertEnabled (channel, slot, ! insert.enabled);

        repaint();
        return;
    }

    for (int i = 0; i < getNumParams(); ++i)
    {
        if (getParamRowBounds (i).expanded (0, 4).contains (pos))
        {
            dragParam = i;

            const auto bar = getBarBounds (i).toFloat();
            setParam (i, (float) (pos.x - bar.getX()) / juce::jmax (1.0f, bar.getWidth()));
            repaint();
            return;
        }
    }
}

void InsertEditorContent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragParam < 0)
        return;

    const auto bar = getBarBounds (dragParam).toFloat();
    setParam (dragParam, (float) (e.getPosition().x - bar.getX()) / juce::jmax (1.0f, bar.getWidth()));
    repaint();
}

void InsertEditorContent::mouseUp (const juce::MouseEvent&)
{
    dragParam = -1;
}

void InsertEditorContent::timerCallback()
{
    // The strip on the console edits the same values; keep the readout honest
    // without repainting at 60 fps for nothing.
    repaint();
}

//==============================================================================
InsertEditorWindow::InsertEditorWindow (MixerModel& mixerModel, int channelToUse, int slotToUse)
    : juce::DocumentWindow (utf8 ("AtomicDAW · ")
                              + InsertEditorContent::effectName (mixerModel.getInsert (channelToUse, slotToUse).type),
                            kBackground,
                            juce::DocumentWindow::closeButton),
      channel (channelToUse), slot (slotToUse)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new InsertEditorContent (mixerModel, channel, slot), false);
    setResizable (true, false);
    // EQ carries nine knobs; the window has to grow with the effect, not clip it.
    setResizeLimits (360, 150, 900, InsertEditorContent::preferredHeightFor (fx::maxParams) + 200);
    centreWithSize (InsertEditorContent::preferredWidth,
                    InsertEditorContent::preferredHeightFor (
                        fx::numParams (mixerModel.getInsert (channel, slot).type)));
    setVisible (true);
}

void InsertEditorWindow::setKeyForwarder (juce::KeyListener* listener)
{
    keyForwarder = listener;

    if (listener == nullptr)
        return;

    addKeyListener (listener);

    if (auto* content = getContentComponent())
        content->addKeyListener (listener);
}

void InsertEditorWindow::closeButtonPressed()
{
    if (onCloseRequest != nullptr)
        onCloseRequest();
}

} // namespace atomic

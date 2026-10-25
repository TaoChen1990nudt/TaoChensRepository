#include "EffectWindows.h"

#include "Util/Metrics.h"

#include "Util/Text.h"

namespace atomic
{

namespace
{
    const juce::Colour kBackground { 0xff14151a };
    const juce::Colour kPanel      { 0xff1b1d24 };
    const juce::Colour kText       { 0xff9aa0ad };
    const juce::Colour kAccent     { 0xff4fd1c5 };

    juce::String builtInName (fx::Type type)
    {
        switch (type)
        {
            case fx::Type::lowPass: return utf8 ("低通滤波");
            case fx::Type::drive:   return utf8 ("驱动");
            case fx::Type::delay:   return utf8 ("延迟");
            case fx::Type::air:     return utf8 ("空气感");
            case fx::Type::vst:     return "VST3";
            case fx::Type::none:
            default:                return utf8 ("空");
        }
    }

    juce::String paramLabel (fx::Type type, int index)
    {
        if (index == 1)
            return utf8 ("反馈");

        switch (type)
        {
            case fx::Type::lowPass: return utf8 ("截止");
            case fx::Type::drive:   return utf8 ("驱动");
            case fx::Type::delay:   return utf8 ("时间");
            case fx::Type::air:     return utf8 ("强度");
            default:                return "P" + juce::String (index + 1);
        }
    }

    /** The number the DSP actually uses, not a 0..1 bar. A control that says
        "0.73" tells the user nothing about how bright the filter is. */
    juce::String paramValueText (fx::Type type, int index, float v)
    {
        const float clamped = juce::jlimit (0.0f, 1.0f, v);

        if (index == 1)
            return juce::String (juce::roundToInt (clamped * 100.0f)) + "%";

        switch (type)
        {
            case fx::Type::lowPass:
            {
                const float hz = 120.0f * std::pow (2.0f, clamped * 7.2f);

                return hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + " kHz"
                                     : juce::String (juce::roundToInt (hz)) + " Hz";
            }

            case fx::Type::drive:
                return juce::String (1.0f + clamped * 11.0f, 1) + "x";

            case fx::Type::delay:
                return juce::String (juce::roundToInt (35.0f + clamped * 180.0f)) + " ms";

            case fx::Type::air:
                return juce::String (0.6f + clamped * 2.4f, 1) + "x";

            default:
                return juce::String (juce::roundToInt (clamped * 100.0f)) + "%";
        }
    }
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
    return mixer.getInsert (channel, slot).type == fx::Type::delay ? 2 : 1;
}

float InsertEditorContent::getParam (int index) const
{
    const auto& insert = mixer.getInsert (channel, slot);

    return index == 1 ? insert.p2 : insert.p1;
}

void InsertEditorContent::setParam (int index, float value)
{
    const auto& insert = mixer.getInsert (channel, slot);
    const float v = juce::jlimit (0.0f, 1.0f, value);

    if (index == 1)
        mixer.setInsertParam (channel, slot, insert.p1, v);
    else
        mixer.setInsertParam (channel, slot, v, insert.p2);
}

juce::String InsertEditorContent::getParamName (int index) const
{
    return paramLabel (mixer.getInsert (channel, slot).type, index);
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
    g.drawText (builtInName (insert.type),
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
        g.fillRoundedRectangle (pill, atomic::ui::pf (5));
        g.setColour (on ? kAccent.withAlpha (0.7f) : juce::Colours::white.withAlpha (0.16f));
        g.drawRoundedRectangle (pill, atomic::ui::pf (5), 1.0f);

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
        g.drawText (paramValueText (insert.type, i, value),
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
    : juce::DocumentWindow (utf8 ("AtomicDAW · 效果器"), kBackground,
                            juce::DocumentWindow::closeButton),
      channel (channelToUse), slot (slotToUse)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new InsertEditorContent (mixerModel, channel, slot), false);
    setResizable (true, false);
    setResizeLimits (360, 150, 900, 360);
    centreWithSize (InsertEditorContent::preferredWidth, InsertEditorContent::preferredHeight);
    setVisible (true);
}

void InsertEditorWindow::closeButtonPressed()
{
    if (onCloseRequest != nullptr)
        onCloseRequest();
}

} // namespace atomic

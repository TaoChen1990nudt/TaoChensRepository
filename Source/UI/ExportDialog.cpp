#include "ExportDialog.h"

#include "Audio/Mp3Encoder.h"
#include "Util/Metrics.h"
#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{

namespace
{
    //-- Design pixels ---------------------------------------------------------
    constexpr float kDialogWidth  = 480.0f;
    constexpr float kDialogHeight = 320.0f;

    constexpr float kLabelX  = 24.0f;
    constexpr float kRowSegX = 96.0f;

    constexpr float kSegWidth  = 62.0f;
    constexpr float kSegGap    = 6.0f;
    constexpr float kSegHeight = 24.0f;

    constexpr float kFirstRowY = 64.0f;
    constexpr float kRowStep   = 40.0f;

    // Each row's own index: 0 format, 1 bit depth, 2 bitrate, 3 channels.
    juce::Rectangle<int> segmentBounds (int row, int index)
    {
        return { ui::px (kRowSegX + (kSegWidth + kSegGap) * (float) index),
                 ui::px (kFirstRowY + kRowStep * (float) row),
                 ui::px (kSegWidth), ui::px (kSegHeight) };
    }
}

//==============================================================================
ExportDialog::ExportDialog()
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setSize (ui::px (kDialogWidth), ui::px (kDialogHeight));
}

ExportDialog::~ExportDialog()
{
    if (onClosedByUser != nullptr)
        onClosedByUser();
}

void ExportDialog::setSettings (const ExportSettings& s)
{
    settings = s;

    if (settings.format == ExportFormat::mp3 && ! mp3Available)
        settings.format = ExportFormat::wav;

    repaint();
}

//==============================================================================
void ExportDialog::chooseFormat (ExportFormat format)
{
    if (format == ExportFormat::mp3 && ! mp3Available)
        return;

    settings.format = format;
    repaint();
}

void ExportDialog::chooseBitDepth (int depth)
{
    // Only the three the writer implements. A 20-bit file would be a promise
    // the WAV header cannot keep.
    if (depth != 16 && depth != 24 && depth != 32)
        return;

    settings.bitDepth = depth;
    repaint();
}

void ExportDialog::chooseBitrate (int kbps)
{
    settings.mp3Bitrate = mp3::clampBitrate (kbps);
    repaint();
}

void ExportDialog::chooseMono (bool mono)
{
    settings.mono = mono;
    repaint();
}

//==============================================================================
juce::Rectangle<int> ExportDialog::getFormatBounds (int index) const
{
    return segmentBounds (0, index);
}

juce::Rectangle<int> ExportDialog::getBitDepthBounds (int index) const
{
    return segmentBounds (1, index);
}

juce::Rectangle<int> ExportDialog::getBitrateBounds (int index) const
{
    return segmentBounds (2, index);
}

juce::Rectangle<int> ExportDialog::getChannelBounds (int index) const
{
    return segmentBounds (3, index);
}

juce::Rectangle<int> ExportDialog::getCancelButtonBounds() const
{
    return { getWidth() - ui::px (24) - ui::px (96) - ui::px (8) - ui::px (76),
             getHeight() - ui::px (52), ui::px (76), ui::px (28) };
}

juce::Rectangle<int> ExportDialog::getExportButtonBounds() const
{
    return { getWidth() - ui::px (24) - ui::px (96),
             getHeight() - ui::px (52), ui::px (96), ui::px (28) };
}

int ExportDialog::segmentAt (juce::Rectangle<int> row, int count, juce::Point<int> position)
{
    if (row.getWidth() <= 0 || count <= 0)
        return -1;

    const int step = ui::px (kSegWidth + kSegGap);

    for (int i = 0; i < count; ++i)
    {
        const juce::Rectangle<int> segment (row.getX() + i * step, row.getY(),
                                            row.getWidth(), row.getHeight());

        if (segment.contains (position))
            return i;
    }

    return -1;
}

//==============================================================================
void ExportDialog::paint (juce::Graphics& g)
{
    g.fillAll (theme::surface1);

    const bool wav = settings.format == ExportFormat::wav;

    //-- Title -----------------------------------------------------------------
    g.setColour (theme::ink);
    g.setFont (ui::title (14.0f));
    g.drawText (utf8 ("导出音频"),
                juce::Rectangle<int> (ui::px (kLabelX), ui::px (18),
                                      ui::px (200), ui::px (22)),
                juce::Justification::centredLeft, false);

    if (contentSeconds > 0.0)
    {
        g.setColour (theme::inkSubtle);
        g.setFont (ui::section (9.5f));
        g.drawText (juce::String (contentSeconds, 2) + utf8 (" 秒"),
                    juce::Rectangle<int> (getWidth() - ui::px (24) - ui::px (160), ui::px (20),
                                          ui::px (160), ui::px (18)),
                    juce::Justification::centredRight, false);
    }

    g.setColour (theme::hairline);
    g.fillRect (juce::Rectangle<int> (ui::px (kLabelX), ui::px (48),
                                      getWidth() - ui::px (kLabelX * 2.0f), 1));

    //-- Rows ------------------------------------------------------------------
    const juce::StringArray formatItems { "WAV", "MP3" };

    juce::StringArray depthItems;
    depthItems.add ("16");
    depthItems.add ("24");
    depthItems.add ("32 " + utf8 ("浮点"));

    const juce::StringArray bitrateItems { "128", "192", "256", "320" };

    juce::StringArray channelItems;
    channelItems.add (utf8 ("立体声"));
    channelItems.add (utf8 ("单声道"));

    drawRow (g, 0, utf8 ("格式"), formatItems, wav ? 0 : 1, true, hoverRow == 0);

    drawRow (g, 1, utf8 ("位深"), depthItems,
             settings.bitDepth == 16 ? 0 : (settings.bitDepth == 24 ? 1 : 2),
             wav, hoverRow == 1);

    const int bitrates[4] { 128, 192, 256, 320 };
    int bitrateIndex = 1;

    for (int i = 0; i < 4; ++i)
        if (bitrates[i] == settings.mp3Bitrate)
            bitrateIndex = i;

    drawRow (g, 2, utf8 ("比特率"), bitrateItems, bitrateIndex, ! wav, hoverRow == 2);

    drawRow (g, 3, utf8 ("声道"), channelItems, settings.mono ? 1 : 0, true, hoverRow == 3);

    // Units sit past the END of their own row — past the last chip, not past a
    // fixed x, or a four-chip row's unit ends up inside its last chip.
    g.setColour (theme::inkFaint);
    g.setFont (ui::section (8.5f));

    const int bitUnitX = ui::px (kRowSegX + 3.0f * (kSegWidth + kSegGap) + 8.0f);
    const int rateUnitX = ui::px (kRowSegX + 4.0f * (kSegWidth + kSegGap) + 8.0f);

    g.drawText ("bit", juce::Rectangle<int> (bitUnitX, ui::px (kFirstRowY + kRowStep),
                                             ui::px (60), ui::px (kSegHeight)),
                juce::Justification::centredLeft, false);

    g.drawText ("kbps", juce::Rectangle<int> (rateUnitX, ui::px (kFirstRowY + kRowStep * 2.0f),
                                              ui::px (60), ui::px (kSegHeight)),
                juce::Justification::centredLeft, false);

    //-- Summary ---------------------------------------------------------------
    // The sentence the completion dialog will repeat: what this file IS. Built
    // from the same ExportSettings::describe() the render result uses, so the
    // promise on screen and the report afterwards cannot differ.
    g.setColour (theme::inkSubtle);
    g.setFont (ui::section (9.5f));
    g.drawText (settings.describe(),
                juce::Rectangle<int> (ui::px (kLabelX), getHeight() - ui::px (96),
                                      getWidth() - ui::px (kLabelX * 2.0f), ui::px (18)),
                juce::Justification::centredLeft, false);

    if (! mp3Available && hoverRow == 0)
    {
        g.setColour (theme::warn.withAlpha (0.9f));
        g.setFont (ui::section (8.5f));
        g.drawText (utf8 ("本机没有 MP3 编码器"),
                    juce::Rectangle<int> (ui::px (kRowSegX), ui::px (kFirstRowY + kSegHeight + 2.0f),
                                          ui::px (280), ui::px (14)),
                    juce::Justification::centredLeft, false);
    }

    //-- Buttons ---------------------------------------------------------------
    {
        const auto cancel = getCancelButtonBounds().toFloat();
        const bool hot = hoverRow == 5;

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.12f : 0.06f));
        g.fillRoundedRectangle (cancel, ui::pf (4));
        g.setColour (theme::hairline);
        g.drawRoundedRectangle (cancel.reduced (0.5f), ui::pf (4), 1.0f);

        g.setColour (theme::inkMuted);
        g.setFont (ui::section (10.0f));
        g.drawText (utf8 ("取消"), getCancelButtonBounds(), juce::Justification::centred, false);

        const auto accept = getExportButtonBounds().toFloat();
        const bool hotAccept = hoverRow == 4;

        g.setColour (theme::accent.withAlpha (hotAccept ? 0.95f : 0.80f));
        g.fillRoundedRectangle (accept, ui::pf (4));

        g.setColour (theme::bgApp);
        g.setFont (ui::section (10.0f));
        g.drawText (utf8 ("导出…"), getExportButtonBounds(), juce::Justification::centred, false);
    }
}

void ExportDialog::drawRow (juce::Graphics& g, int row, const juce::String& label,
                            const juce::StringArray& items, int selected,
                            bool enabled, bool hovered) const
{
    g.setColour (enabled ? theme::inkMuted : theme::inkFaint.withAlpha (0.6f));
    g.setFont (ui::section (9.5f));
    g.drawText (label,
                juce::Rectangle<int> (ui::px (kLabelX),
                                      ui::px (kFirstRowY + kRowStep * (float) row),
                                      ui::px (66), ui::px (kSegHeight)),
                juce::Justification::centredLeft, false);

    for (int i = 0; i < items.size(); ++i)
    {
        const auto bounds = segmentBounds (row, i).toFloat();
        const bool isOn   = (i == selected);

        const bool hot = enabled && hovered && hoverSegment == i;

        // A selection on a GREYED row keeps its place but loses its voice: the
        // accent dims, so the row reads as "remembered, not active" — the same
        // reason a disabled juce::Button still shows which item was ticked.
        const auto onEdge   = enabled ? theme::accent.withAlpha (0.90f)
                                      : theme::accent.withAlpha (0.35f);
        const auto onFill   = enabled ? theme::accent.withAlpha (0.22f)
                                      : theme::accent.withAlpha (0.10f);
        const auto onText   = enabled ? theme::ink : theme::inkSubtle;

        g.setColour (isOn ? onFill
                          : juce::Colours::white.withAlpha (hot ? 0.10f : 0.045f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (isOn ? onEdge
                          : juce::Colours::white.withAlpha (enabled ? 0.12f : 0.05f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        g.setColour (isOn ? onText
                          : enabled ? theme::inkMuted : theme::inkFaint.withAlpha (0.55f));
        g.setFont (ui::section (9.5f));
        g.drawText (items[i], bounds.toNearestInt(), juce::Justification::centred, false);
    }
}

//==============================================================================
void ExportDialog::mouseDown (const juce::MouseEvent& e)
{
    clickAt (e.getPosition());
}

void ExportDialog::clickAt (juce::Point<int> position)
{
    const auto p = position;

    if (getExportButtonBounds().contains (p))
    {
        if (onExport != nullptr)
            onExport (settings);

        return;
    }

    if (getCancelButtonBounds().contains (p))
    {
        if (onCancel != nullptr)
            onCancel();

        return;
    }

    if (const int i = segmentAt (getFormatBounds (0), 2, p); i >= 0)
    {
        chooseFormat (i == 0 ? ExportFormat::wav : ExportFormat::mp3);
        return;
    }

    if (const int i = segmentAt (getBitDepthBounds (0), 3, p); i >= 0)
    {
        if (settings.format == ExportFormat::wav)
            chooseBitDepth (i == 0 ? 16 : (i == 1 ? 24 : 32));

        return;
    }

    if (const int i = segmentAt (getBitrateBounds (0), 4, p); i >= 0)
    {
        if (settings.format == ExportFormat::mp3)
        {
            const int bitrates[4] { 128, 192, 256, 320 };
            chooseBitrate (bitrates[i]);
        }

        return;
    }

    if (const int i = segmentAt (getChannelBounds (0), 2, p); i >= 0)
    {
        chooseMono (i == 1);
        return;
    }
}

void ExportDialog::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    int row = -1;
    int segment = -1;

    if (getExportButtonBounds().contains (p))       row = 4;
    else if (getCancelButtonBounds().contains (p))  row = 5;
    else
    {
        // One local instead of five `if (const int i = ...)` declarations that
        // would each shadow the last.
        const int hit = segmentAt (getFormatBounds (0), 2, p);

        if (hit >= 0)                        { row = 0; segment = hit; }
        else if ((segment = segmentAt (getBitDepthBounds (0), 3, p)) >= 0)  row = 1;
        else if ((segment = segmentAt (getBitrateBounds (0), 4, p)) >= 0)   row = 2;
        else if ((segment = segmentAt (getChannelBounds (0), 2, p)) >= 0)   row = 3;
    }

    if (row != hoverRow || segment != hoverSegment)
    {
        hoverRow = row;
        hoverSegment = segment;
        repaint();
    }
}

void ExportDialog::mouseExit (const juce::MouseEvent&)
{
    if (hoverRow != -1 || hoverSegment != -1)
    {
        hoverRow = -1;
        hoverSegment = -1;
        repaint();
    }
}

void ExportDialog::resized() {}

bool ExportDialog::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        if (onExport != nullptr)
            onExport (settings);

        return true;
    }

    if (key == juce::KeyPress::escapeKey)
    {
        if (onCancel != nullptr)
            onCancel();

        return true;
    }

    return false;
}

} // namespace atomic

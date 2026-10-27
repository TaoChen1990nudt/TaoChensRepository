#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Audio/AudioExport.h"

namespace atomic
{

//==============================================================================
/**
    Phase 52: the export dialog — format, depth, bitrate, channels.

    Drawn by hand rather than assembled from juce::Buttons, for the reason the
    rest of this interface is hand-drawn: a segmented choice of four numbers is
    a row of rectangles with a fill, and the built-in widgets would each bring
    their own idea of padding, focus ring and font. It also makes the geometry
    readable by the self-test, which is how "the chips do not overlap and the
    highlighted one is inside its row" becomes an assertion instead of a
    screenshot someone has to remember to look at.

    Both the WAV row and the MP3 row are always visible: hiding the irrelevant
    one would make the dialog change height every time the format changes, and
    a greyed row is a better explanation ("MP3 has no bit depth, it has a
    bitrate") than a missing one.
*/
class ExportDialog final : public juce::Component
{
public:
    ExportDialog();
    ~ExportDialog() override;

    /** What the user has chosen so far. */
    ExportSettings getSettings() const noexcept { return settings; }
    void setSettings (const ExportSettings& s);

    void setContentSeconds (double seconds) { contentSeconds = seconds; }

    /** False when the platform has no encoder: the MP3 chip greys out instead
        of accepting a click that would fail after the render. */
    void setMp3Available (bool available) { mp3Available = available; repaint(); }
    bool isMp3Available() const noexcept { return mp3Available; }

    /** The choices, as functions — the click handler and the self-test both go
        through these, so a test cannot pass while the button does nothing. */
    void chooseFormat (ExportFormat format);
    void chooseBitDepth (int depth);
    void chooseBitrate (int kbps);
    void chooseMono (bool mono);

    /** One click, as a decision. Public so the self-test can press the greyed
        row and the live row through the same code the mouse uses — otherwise
        "the inactive row does nothing" would be a property of a lambda nobody
        could call. */
    void clickAt (juce::Point<int> position);

    std::function<void (const ExportSettings&)> onExport;   // 导出…
    std::function<void()> onCancel;                          // 取消 / Esc

    /** Called from the destructor, whatever closed the window. The host uses it
        to drop its "one dialog at a time" flag — that flag must not depend on
        the user pressing one of the two buttons. */
    std::function<void()> onClosedByUser;

    //-- Geometry, public for the self-test --------------------------------
    juce::Rectangle<int> getFormatBounds (int index) const;      // 0 wav, 1 mp3
    juce::Rectangle<int> getBitDepthBounds (int index) const;    // 0..2
    juce::Rectangle<int> getBitrateBounds (int index) const;     // 0..3
    juce::Rectangle<int> getChannelBounds (int index) const;     // 0 stereo, 1 mono
    juce::Rectangle<int> getExportButtonBounds() const;
    juce::Rectangle<int> getCancelButtonBounds() const;

    /** Which segment of `count` a point lands in, or -1. One rule for every
        row, so the chip that highlights and the choice that runs are the same
        rectangle. */
    static int segmentAt (juce::Rectangle<int> row, int count, juce::Point<int> position);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    juce::Rectangle<int> getRowBounds (int row) const;

    void drawRow (juce::Graphics&, int row, const juce::String& label,
                  const juce::StringArray& items, int selected, bool enabled,
                  bool hovered) const;

    ExportSettings settings;
    double contentSeconds = 0.0;
    bool   mp3Available   = true;

    /** Which row the pointer is over: 0 format, 1 depth, 2 bitrate, 3 channel,
        4 export, 5 cancel, -1 nothing. Hover is drawn from this and nothing
        else, so a chip that highlights is always a chip that would react. */
    int hoverRow = -1;
    int hoverSegment = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportDialog)
};

} // namespace atomic

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <functional>
#include <vector>

namespace atomic
{

//==============================================================================
/**
    Phase 17: the sample browser under the sheet list.

    A folder list, not a full file manager. The user adds the folders they keep
    samples in; the panel remembers them; every WAV inside is a draggable row.

    Two decisions worth stating:

    * **Folders are remembered outside the project.** Which folders you have
      browsed is a property of you, not of the song, and putting it in the
      .atomic file would make "add a folder" an undoable edit — so it lives in a
      two-line text file next to the plugin library.

    * **Only files this build can decode are listed.** `isSampleFile()` is the
      single list, and it is WAV only. A browser that shows a row which turns
      into "cannot decode" when you drop it is worse than a short list with the
      format printed in the header.

    Dragging is done through the DragAndDropContainer that MainComponent is: the
    panel only starts the drag, it does not know the roll exists.
*/
class FileBrowserPanel final : public juce::Component,
                               private juce::Timer
{
public:
    static constexpr int headerHeight    = ui::px (46);
    static constexpr int folderHeight    = ui::px (30);
    static constexpr int fileHeight      = ui::px (23);

    FileBrowserPanel();
    ~FileBrowserPanel() override;

    /** Fired when a row is added or removed, so the host can refresh anything
        that lists samples. */
    std::function<void()> onFoldersChanged;

    /** Phase 20: a single click on a file row asks to hear it.

        Returns how many seconds were actually started (<= 0 means "could not
        play / refused"), so the row's progress line is the length of the sound
        rather than a guess. The host owns the decoder and the engine; the panel
        owns nothing but the request. Starting a second preview stops the first
        — that is enforced in the audio engine, not here, because "no overlap"
        has to hold no matter which panel asked. */
    std::function<double (const juce::File&)> onPreviewRequested;

    /** 0..1 progress of the previewing row, or -1 when nothing is previewing.
        Public so the self-test can assert the indicator expires. */
    double getPreviewProgress() const;
    juce::String getPreviewingPath() const { return previewing ? previewPath : juce::String(); }

    /** Adds a folder and scans it. Duplicates are ignored. */
    void addFolder (const juce::File& folder);
    void removeFolder (int index);
    void rescanFolders();

    int        getNumFolders() const noexcept { return (int) folders.size(); }
    juce::File getFolder (int index) const;

    /** Every row the panel would draw, in display order. Folders come first,
        then the files of each expanded folder, indented. Empty when there is
        nothing to show. */
    struct Row
    {
        bool        isFolder    = false;
        int         folderIndex = -1;
        juce::File  file;            // only for file rows
        int         indent      = 0;
    };

    std::vector<Row> buildRows() const;

    /** Total height the rows need, and how much of it the panel can show.
        Public because the self-test asserts that a long list really scrolls. */
    int getContentHeight() const;
    int getViewHeight() const;
    int getScrollY() const noexcept { return (int) scrollY; }
    void setScrollY (int pixels);

    bool isFolderExpanded (int index) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** The folder list file, created on demand. */
    static juce::File getStoreFile();

private:
    struct FolderEntry
    {
        juce::File folder;
        bool       expanded = true;
        std::vector<juce::File> files;
    };

    void timerCallback() override;

    /** Starts the indicator for `file` (already accepted by the host). */
    void beginPreviewFeedback (const juce::File& file, double seconds);
    void endPreviewFeedback();

    void scanFolder (FolderEntry&);
    void loadFromDisk();
    void saveToDisk() const;

    juce::Rectangle<int> getAddButtonBounds() const;
    juce::Rectangle<int> getRemoveButtonBounds (int rowIndex) const;
    juce::Rectangle<int> getViewArea() const;
    juce::Rectangle<int> getRowBounds (int rowIndex) const;
    int  rowAt (juce::Point<int>) const;
    void clampScroll();

    /** A small picture of the row, handed to JUCE so the drag has something to
        carry under the cursor. */
    juce::Image makeDragImage (const juce::String& name) const;

    std::vector<FolderEntry> folders;

    float  scrollY = 0.0f;
    int    hoveredRow = -1;
    int    pressedRow = -1;

    /** Set once a drag has actually been started, so mouseUp does not treat the
        gesture as a click. */
    bool   dragging = false;

    juce::Point<int> dragStart;

    //-- Phase 20: the audition indicator -------------------------------------
    /** Stored as a PATH, not a row index: folding a folder renumbers every row
        below it, and a preview that jumps to a different file when you collapse
        a folder is worse than no indicator at all. */
    juce::String previewPath;
    double previewStartMs    = 0.0;
    double previewDurationMs = 0.0;
    bool   previewing        = false;

    /** Kept alive while the async chooser runs; a second one would orphan the
        first, exactly like the project save dialog. */
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileBrowserPanel)
};

} // namespace atomic

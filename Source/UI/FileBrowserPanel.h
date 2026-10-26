#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Util/Metrics.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <set>
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

    Phase 28 changed two things about the list itself:

    * **It is a tree, not a flat dump.** A folder of 800 samples that were all
      drawn at one indent was a list you could not navigate: two files from
      different subdirectories sat next to each other with nothing to say which
      was which. Every subdirectory is now its own row with its own disclosure
      triangle, opened/folded state keyed by ABSOLUTE PATH so a rescan does not
      forget which parts of the tree you had opened.

      Phase 33: the tree arrives FOLDED. A scan used to leave every branch open,
      which turned a real sample library into a thousand-row wall the instant it
      finished; the store now remembers the branches the user OPENED and treats
      everything else as closed.

    * **Scanning is asynchronous and reports progress.** `findChildFiles()` on
      a real sample library freezes the message thread for seconds; the panel
      used to do exactly that, on the click that added the folder. The scan now
      runs on its own thread, walks the tree level by level (so "how much is
      left" is a real ratio rather than a guess), and paints a progress line in
      the header while it works. Results are handed back through AsyncUpdater,
      so the model is still touched only by the message thread.

    Dragging is done through the DragAndDropContainer that MainComponent is: the
    panel only starts the drag, it does not know the roll exists.
*/
class FileBrowserPanel final : public juce::Component,
                               private juce::Timer,
                               private juce::AsyncUpdater
{
public:
    static constexpr int headerHeight    = ui::px (46);
    static constexpr int folderHeight    = ui::px (30);   // a root folder row
    static constexpr int subFolderHeight = ui::px (24);   // a nested folder row
    static constexpr int fileHeight      = ui::px (23);

    /** How far one tree level indents, in real pixels. */
    static constexpr int indentWidth     = ui::px (13);

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

    /** Adds a folder and queues a scan. Duplicates are ignored. The row appears
        immediately (empty, with a spinner) — the files arrive when the scan
        finishes.

        `persist` = false adds the folder for this session only: the snapshot
        fixture and the store-file reader both need a folder that is not written
        back out, one because it is a fixture, the other because rewriting the
        file WHILE reading it truncates every path after the one being added. */
    void addFolder (const juce::File& folder, bool persist = true);
    void removeFolder (int index);
    void rescanFolders();

    int        getNumFolders() const noexcept { return (int) folders.size(); }
    juce::File getFolder (int index) const;

    //-- Phase 28: the scan --------------------------------------------------
    /** True while a scan is in flight (including the window between "the thread
        finished" and "the rows were rebuilt"). */
    bool  isScanning() const noexcept { return scanRunning; }
    /** 0..1. Level-by-level, so it means "how much of the tree is done". */
    float getScanProgress() const noexcept { return scanProgress.load(); }
    /** Files found so far by the running scan. The header prints it live. */
    int   getScanFileCount() const noexcept { return scanFileCount.load(); }
    /** How many folders are waiting their turn, including the one running. */
    int   getPendingScanCount() const noexcept
    {
        return (int) scanQueue.size() + (scanRunning ? 1 : 0);
    }
    juce::String getScanningFolderName() const { return scanningRoot.getFileName(); }

    /** Test hook: blocks until every queued scan has been applied. Returns
        false on timeout. The self-test runs before the message loop, so it
        cannot wait for AsyncUpdater — it has to pump the delivery itself. */
    bool waitForPendingScans (int timeoutMs = 10000);

    /** Snapshot hook: drops every listed folder WITHOUT touching the store
        file, so a screenshot can show a known fixture tree instead of whatever
        this machine happens to have. Never call it from the UI. */
    void clearFoldersForSnapshot();

    /** Every row the panel would draw, in display order: each root folder,
        then its subdirectories and files depth-first, indented.

        Phase 28: a row is one of three kinds, and `indent` is its depth in the
        tree (0 = the root folder itself). */
    struct Row
    {
        enum class Kind { rootFolder, subFolder, file };

        Kind        kind        = Kind::file;
        int         folderIndex = -1;
        /** The file for a file row, the DIRECTORY for a folder row. */
        juce::File  file;
        int         indent      = 0;
        /** Folder rows: how many WAVs live underneath, recursively. */
        int         fileCount   = 0;
        /** Folder rows: the name to draw. Empty means "derive from file". */
        juce::String label;

        bool isFolder() const noexcept { return kind != Kind::file; }
        int  height() const noexcept
        {
            switch (kind)
            {
                case Kind::rootFolder: return folderHeight;
                case Kind::subFolder:  return subFolderHeight;
                case Kind::file:
                default:               return fileHeight;
            }
        }
    };

    std::vector<Row> buildRows() const;

    /** Total height the rows need, and how much of it the panel can show.
        Public because the self-test asserts that a long list really scrolls. */
    int getContentHeight() const;
    int getViewHeight() const;
    int getScrollY() const noexcept { return (int) scrollY; }
    void setScrollY (int pixels);

    bool isFolderExpanded (int index) const;

    /** Phase 28: folds or unfolds one nested directory. Keyed by path, so it
        survives a rescan and is what the tree's triangles toggle.

        Phase 33: `persist = false` leaves the store file alone, which is what
        the screenshot harness needs (a temp folder's path must not end up in
        the user's real folder list). */
    void setSubFolderExpanded (const juce::File& directory, bool expanded,
                               bool persist = true);
    bool isSubFolderExpanded (const juce::File& directory) const;

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
    /** One directory in the scanned tree. Owns its files and its children. */
    struct DirNode
    {
        juce::String name;
        juce::File   dir;
        int          scanDepth = 0;                           // 0 = the root
        std::vector<juce::File> files;                        // direct children
        std::vector<std::unique_ptr<DirNode>> children;       // subdirectories
        int fileCount = 0;                                    // recursive

        int recount();
    };

    struct FolderEntry
    {
        juce::File folder;
        bool       expanded = true;
        bool       scanned  = false;      // false = the scan is still running
        std::unique_ptr<DirNode> tree;
    };

    struct ScanResult
    {
        juce::File root;
        std::unique_ptr<DirNode> tree;
        int  files     = 0;
        bool cancelled = false;
    };

    struct ScanThread;                    // defined in the .cpp

    void timerCallback() override;
    void handleAsyncUpdate() override;

    /** Starts the next queued folder, or clears `scanRunning` when there is
        nothing left. */
    void startNextQueuedScan();
    void applyScanResult (ScanResult&);
    void updateTimerState();

    void appendNodeRows (std::vector<Row>&, int folderIndex,
                         const DirNode&, int indent) const;

    /** The row list, rebuilt on demand. Cached because the tree is deep enough
        that rebuilding it on every mouseMove would be felt on a large library;
        every mutation calls invalidateRows(). */
    void invalidateRows();
    const std::vector<Row>& ensureRows() const;

    /** Starts the indicator for `file` (already accepted by the host). */
    void beginPreviewFeedback (const juce::File& file, double seconds);
    void endPreviewFeedback();

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

    mutable std::vector<Row> cachedRows;
    mutable bool             rowsDirty = true;

    /** Absolute paths of nested directories the user has OPENED. Paths, not
        indices: a rescan rebuilds the tree, and a fold that jumps to the wrong
        branch is worse than no fold at all.

        Phase 33 inverted this set. It used to hold the folded branches, which
        made "open" the default and put the whole library on screen; the set now
        holds the opened ones and an unlisted directory is folded. */
    std::set<juce::String> expandedPaths;

    //-- Phase 28: the scan job ----------------------------------------------
    std::unique_ptr<ScanThread> scanner;
    std::deque<juce::File>      scanQueue;     // waiting their turn
    juce::File                  scanningRoot;  // the one in flight
    std::atomic<bool>  scanRunning   { false };
    std::atomic<float> scanProgress  { 0.0f };
    std::atomic<int>   scanFileCount { 0 };

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

#include "FileBrowserPanel.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"
#include "Util/Theme.h"

#include <algorithm>

namespace atomic
{

namespace
{
    const juce::Colour& kPanelBackground = theme::surface1;
    const juce::Colour& kDivider        = theme::hairline;
    const juce::Colour kRowHover        = juce::Colours::white.withAlpha (0.051f);
    const juce::Colour kFileText        = theme::ink;
    const juce::Colour kFolderText      = theme::ink;
    const juce::Colour& kAccent         = theme::accent;
    /** Phase 20: the audition line. Green, because it is "playing", and green
        is the one colour in this palette nothing else uses. */
    const juce::Colour kPreviewAccent   = theme::live;
    /** Phase 28: the scan line. Amber — it is "working", not "playing". */
    const juce::Colour kScanAccent      = theme::warn;

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    /** A folder is scanned at most this deep, so adding "C:\\" by accident
        cannot freeze the scanner for a minute. */
    constexpr int maxScanDepth = 6;
    constexpr int maxFilesPerFolder = 2000;

    /** The prefix that marks a directory the user has opened in the store file.

        Phase 33 inverted this list. It used to record the branches that were
        FOLDED, which made "everything is open" the default — and a real sample
        library then arrived as a thousand-row wall the moment the scan finished.
        Folding is the state worth remembering, so the store now names the
        branches that are OPEN and anything unlisted starts folded. An older
        store's `!collapse` lines are simply ignored: every branch it left open
        opens folded once, which is the behaviour this change is for. */
    const juce::String expandPrefix ("!expand ");

    bool isHiddenDirectory (const juce::File& dir)
    {
        const auto name = dir.getFileName();
        return name.startsWithChar ('.') || name.startsWithIgnoreCase ("__");
    }
}

//==============================================================================
FileBrowserPanel::FileBrowserPanel()
{
    setOpaque (true);
    loadFromDisk();
}

juce::File FileBrowserPanel::getStoreFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("sample-folders.txt");
}

//==============================================================================
//  Phase 28: the background scan
//==============================================================================
struct FileBrowserPanel::ScanThread final : public juce::Thread
{
    ScanThread (FileBrowserPanel& o, juce::File r)
        : juce::Thread ("AtomicDAW sample scan"),
          owner (o), root (std::move (r))
    {
    }

    void run() override;

    std::unique_ptr<ScanResult> takeResult() { return std::move (result); }

    FileBrowserPanel& owner;
    juce::File        root;
    std::unique_ptr<ScanResult> result;
};

void FileBrowserPanel::ScanThread::run()
{
    auto out = std::make_unique<ScanResult>();
    out->root = root;

    auto rootNode = std::make_unique<DirNode>();
    rootNode->dir        = root;
    rootNode->name       = root.getFileName();
    rootNode->scanDepth  = 0;

    // Level by level, so "how far along is this" is a real ratio: every
    // iteration finishes one directory and leaves a known number in the queue.
    // A single recursive findChildFiles() would be one indivisible call, and a
    // progress bar over one indivisible call can only lie.
    std::deque<DirNode*> queue { rootNode.get() };
    int processed = 0;
    int found     = 0;

    while (! queue.empty() && ! threadShouldExit())
    {
        auto* node = queue.front();
        queue.pop_front();

        if (node->dir.isDirectory())
        {
            juce::Array<juce::File> dirs, files;

            node->dir.findChildFiles (dirs,  juce::File::findDirectories, false, "*");
            node->dir.findChildFiles (files, juce::File::findFiles,      false, "*.wav;*.wave");

            for (const auto& dir : dirs)
            {
                if (threadShouldExit())
                    break;

                if (node->scanDepth + 1 > maxScanDepth || isHiddenDirectory (dir))
                    continue;

                auto child = std::make_unique<DirNode>();
                child->dir       = dir;
                child->name      = dir.getFileName();
                child->scanDepth = node->scanDepth + 1;
                queue.push_back (child.get());
                node->children.push_back (std::move (child));
            }

            for (const auto& file : files)
            {
                if (found >= maxFilesPerFolder)
                    break;

                if (! isSampleFile (file))
                    continue;

                node->files.push_back (file);
                ++found;
            }

            // Alphabetical inside one directory; the tree provides the order
            // BETWEEN directories, so sorting by full path here would just
            // scramble the branch you are looking at.
            std::sort (node->files.begin(), node->files.end(),
                       [] (const juce::File& a, const juce::File& b)
                       { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });

            std::sort (node->children.begin(), node->children.end(),
                       [] (const std::unique_ptr<DirNode>& a, const std::unique_ptr<DirNode>& b)
                       { return a->name.compareIgnoreCase (b->name) < 0; });
        }

        ++processed;

        const int remaining = (int) queue.size();
        const float progress = (float) processed / (float) juce::jmax (1, processed + remaining);

        owner.scanProgress.store (juce::jlimit (0.0f, 1.0f, progress));
        owner.scanFileCount.store (found);
    }

    rootNode->recount();

    out->tree      = std::move (rootNode);
    out->files     = found;
    out->cancelled = threadShouldExit();

    result = std::move (out);

    owner.triggerAsyncUpdate();
}

//==============================================================================
//  Defined here, after ScanThread, because the destructor has to be able to
//  stop the thread it holds by unique_ptr — a destructor written above the
//  definition of an incomplete type cannot call members on it.
FileBrowserPanel::~FileBrowserPanel()
{
    stopTimer();
    cancelPendingUpdate();

    if (scanner != nullptr)
    {
        scanner->signalThreadShouldExit();

        // A Thread object must never be destroyed while its thread is still
        // running — JUCE asserts on it and, in a release build, the thread
        // would touch a half-destroyed panel. findChildFiles() cannot be
        // interrupted, so this waits for the current directory to finish and
        // then quits at the next level.
        if (! scanner->waitForThreadToExit (5000))
            scanner->stopThread (-1);
    }
}

int FileBrowserPanel::DirNode::recount(){
    fileCount = (int) files.size();

    for (const auto& child : children)
        fileCount += child->recount();

    return fileCount;
}

//==============================================================================
void FileBrowserPanel::startNextQueuedScan()
{
    if (scanQueue.empty())
    {
        scanRunning.store (false);
        // 100 %, not 0 %: the last scan did finish, and a finished job that
        // reports zero reads as "it failed" in anything that inspects it after
        // the fact (including the self-test).
        scanProgress.store (1.0f);
        return;
    }

    scanningRoot = scanQueue.front();
    scanQueue.pop_front();

    scanRunning.store (true);
    scanProgress.store (0.0f);
    scanFileCount.store (0);

    scanner = std::make_unique<ScanThread> (*this, scanningRoot);
    scanner->startThread (juce::Thread::Priority::normal);
}

void FileBrowserPanel::applyScanResult (ScanResult& result)
{
    for (auto& entry : folders)
    {
        if (entry.folder.getFullPathName() != result.root.getFullPathName())
            continue;

        entry.tree    = std::move (result.tree);
        entry.scanned = true;
        break;
    }

    invalidateRows();
    clampScroll();

    if (onFoldersChanged)
        onFoldersChanged();

    repaint();
}

void FileBrowserPanel::handleAsyncUpdate()
{
    if (scanner == nullptr)
        return;

    // The thread posts this on its last line, so it is normally already gone;
    // the wait only covers the scheduler finishing the unwind. Without it the
    // result could be read while run() is still placing it.
    if (scanner->isThreadRunning())
        scanner->waitForThreadToExit (3000);

    auto result = scanner->takeResult();
    scanner.reset();
    scanRunning.store (false);

    if (result != nullptr && ! result->cancelled)
        applyScanResult (*result);

    startNextQueuedScan();
    updateTimerState();
    repaint();
}

bool FileBrowserPanel::waitForPendingScans (int timeoutMs){
    const auto deadline = juce::Time::getMillisecondCounter()
                            + (juce::uint32) juce::jmax (1, timeoutMs);

    for (;;)
    {
        if (scanner != nullptr)
        {
            scanner->waitForThreadToExit (250);

            if (! scanner->isThreadRunning())
            {
                cancelPendingUpdate();
                handleAsyncUpdate();      // idempotent: it resets the thread
            }
        }

        if (! scanRunning.load() && scanQueue.empty())
            return true;

        if (juce::Time::getMillisecondCounter() > deadline)
            return false;
    }
}

void FileBrowserPanel::clearFoldersForSnapshot()
{
    // No saveToDisk() on purpose: this is a view-only reset for the screenshot
    // harness, and writing here would erase the user's real folder list. The
    // folders added afterwards use persist = false for the same reason.
    folders.clear();
    expandedPaths.clear();
    scanQueue.clear();
    invalidateRows();
    clampScroll();
    repaint();
}

//==============================================================================
void FileBrowserPanel::loadFromDisk()
{
    const auto store = getStoreFile();

    if (! store.existsAsFile())
        return;

    const auto lines = juce::StringArray::fromLines (store.loadFileAsString());

    // Two passes: the folded branches have to be known BEFORE the folders are
    // added, because adding a folder writes the store file back out — and a
    // write in the middle of reading would drop every line after it.
    std::vector<juce::File> paths;

    for (const auto& line : lines)
    {
        const auto text = line.trim();

        if (text.isEmpty() || text.startsWith ("#"))
            continue;

        if (text.startsWith (expandPrefix))
        {
            const auto path = text.fromFirstOccurrenceOf (expandPrefix, false, false).trim();

            if (path.isNotEmpty())
                expandedPaths.insert (juce::File (path).getFullPathName());

            continue;
        }

        // Any other "!" line is a marker this build does not know: `!collapse`
        // from Phase 28's store is the one that actually exists in the wild.
        // Skipping it (rather than treating it as a folder path) is what makes
        // an old store readable instead of a list of folders called "!collapse".
        if (text.startsWithChar ('!'))
            continue;

        paths.push_back (juce::File (text));
    }

    for (const auto& path : paths)
        addFolder (path, false);

    // Written once, after every path is in: addFolder() used to write the store
    // on each call, which on a load means rewriting the file that is still being
    // read — every path after the one being added fell off the end.
    saveToDisk();
}

void FileBrowserPanel::saveToDisk() const
{
    juce::StringArray lines;
    lines.add ("# AtomicDAW sample folders — one path per line, safe to edit");
    lines.add (utf8 ("# 采样文件夹列表，一行一个路径；") + expandPrefix
               + utf8 ("<路径> 表示该子目录已展开，未列出的默认折叠"));

    for (const auto& entry : folders)
        lines.add (entry.folder.getFullPathName());

    for (const auto& path : expandedPaths)
        lines.add (expandPrefix + path);

    const auto store = getStoreFile();
    store.getParentDirectory().createDirectory();
    store.replaceWithText (lines.joinIntoString ("\n"));
}

//==============================================================================
void FileBrowserPanel::addFolder (const juce::File& folder, bool persist)
{
    if (! folder.isDirectory())
        return;

    const auto canonical = folder.getFullPathName();

    for (const auto& entry : folders)
        if (entry.folder.getFullPathName() == canonical)
            return;                                  // already listed

    FolderEntry entry;
    entry.folder = folder;

    // The row appears NOW, empty, with the header showing a spinner. Filling it
    // in immediately is what used to freeze the click that added the folder.
    folders.push_back (std::move (entry));

    if (persist)
        saveToDisk();

    invalidateRows();
    clampScroll();

    scanQueue.push_back (folder);

    if (! scanRunning.load())
        startNextQueuedScan();

    updateTimerState();

    if (onFoldersChanged)
        onFoldersChanged();

    repaint();
}

void FileBrowserPanel::removeFolder (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) folders.size()))
        return;

    folders.erase (folders.begin() + index);

    saveToDisk();
    invalidateRows();
    clampScroll();

    if (onFoldersChanged)
        onFoldersChanged();

    repaint();
}

void FileBrowserPanel::rescanFolders()
{
    for (const auto& entry : folders)
        scanQueue.push_back (entry.folder);

    if (! scanRunning.load())
        startNextQueuedScan();

    updateTimerState();
    repaint();
}

juce::File FileBrowserPanel::getFolder (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) folders.size()))
        return {};

    return folders[(size_t) index].folder;
}

bool FileBrowserPanel::isFolderExpanded (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) folders.size()))
        return false;

    return folders[(size_t) index].expanded;
}

bool FileBrowserPanel::isSubFolderExpanded (const juce::File& directory) const
{
    // Phase 33: unlisted means FOLDED. A folder the user has never touched
    // starts closed, which is the whole point of the change.
    return expandedPaths.find (directory.getFullPathName()) != expandedPaths.end();
}

void FileBrowserPanel::setSubFolderExpanded (const juce::File& directory, bool expanded,
                                             bool persist)
{
    const auto key = directory.getFullPathName();

    if (expanded)
        expandedPaths.insert (key);
    else
        expandedPaths.erase (key);

    // Phase 33: the screenshot harness unfolds a branch with persist = false —
    // it must not leave a temp folder's path in the user's real store file.
    if (persist)
        saveToDisk();
    invalidateRows();
    clampScroll();
    repaint();
}

//==============================================================================
void FileBrowserPanel::invalidateRows()
{
    rowsDirty = true;
}

const std::vector<FileBrowserPanel::Row>& FileBrowserPanel::ensureRows() const
{
    if (rowsDirty)
    {
        cachedRows = buildRows();
        rowsDirty  = false;
    }

    return cachedRows;
}

std::vector<FileBrowserPanel::Row> FileBrowserPanel::buildRows() const
{
    std::vector<Row> rows;

    for (int i = 0; i < (int) folders.size(); ++i)
    {
        const auto& entry = folders[(size_t) i];

        Row header;
        header.kind        = Row::Kind::rootFolder;
        header.folderIndex = i;
        header.file        = entry.folder;
        header.label       = entry.folder.getFileName().isEmpty()
                               ? entry.folder.getFullPathName()
                               : entry.folder.getFileName();
        header.fileCount   = entry.tree != nullptr ? entry.tree->fileCount : 0;
        rows.push_back (std::move (header));

        if (! entry.expanded || entry.tree == nullptr)
            continue;

        appendNodeRows (rows, i, *entry.tree, 1);
    }

    return rows;
}

void FileBrowserPanel::appendNodeRows (std::vector<Row>& rows, int folderIndex,
                                       const DirNode& node, int indent) const
{
    // Directories first, then files — the order every file manager uses, and
    // the order that answers "where am I" without reading a path.
    for (const auto& child : node.children)
    {
        if (child->fileCount == 0)
            continue;                      // nothing to show underneath it

        Row dirRow;
        dirRow.kind        = Row::Kind::subFolder;
        dirRow.folderIndex = folderIndex;
        dirRow.file        = child->dir;
        dirRow.indent      = indent;
        dirRow.label       = child->name;
        dirRow.fileCount   = child->fileCount;
        rows.push_back (std::move (dirRow));

        if (isSubFolderExpanded (child->dir))
            appendNodeRows (rows, folderIndex, *child, indent + 1);
    }

    for (const auto& file : node.files)
    {
        Row row;
        row.kind        = Row::Kind::file;
        row.folderIndex = folderIndex;
        row.file        = file;
        row.indent      = indent;
        rows.push_back (std::move (row));
    }
}

int FileBrowserPanel::getContentHeight() const
{
    int total = 0;

    for (const auto& row : ensureRows())
        total += row.height();

    return total;
}

int FileBrowserPanel::getViewHeight() const
{
    return juce::jmax (0, getHeight() - headerHeight);
}

//==============================================================================
juce::Rectangle<int> FileBrowserPanel::getAddButtonBounds() const
{
    return { getWidth() - ui::px (92), ui::px (11), ui::px (78), ui::px (22) };
}

juce::Rectangle<int> FileBrowserPanel::getRemoveButtonBounds (int rowIndex) const
{
    return { getWidth() - ui::px (28), getRowBounds (rowIndex).getY() + ui::px (6),
             ui::px (16), ui::px (16) };
}

juce::Rectangle<int> FileBrowserPanel::getViewArea() const
{
    return { 0, headerHeight, getWidth(), getViewHeight() };
}

juce::Rectangle<int> FileBrowserPanel::getRowBounds (int rowIndex) const
{
    const auto& rows = ensureRows();

    int y = headerHeight - (int) scrollY;

    for (int i = 0; i < rowIndex && i < (int) rows.size(); ++i)
        y += rows[(size_t) i].height();

    const int height = juce::isPositiveAndBelow (rowIndex, (int) rows.size())
                         ? rows[(size_t) rowIndex].height()
                         : 0;

    return { 0, y, getWidth(), height };
}

int FileBrowserPanel::rowAt (juce::Point<int> position) const
{
    if (! getViewArea().contains (position))
        return -1;

    const auto& rows = ensureRows();

    for (int i = 0; i < (int) rows.size(); ++i)
        if (getRowBounds (i).contains (position))
            return i;

    return -1;
}

//==============================================================================
void FileBrowserPanel::setScrollY (int pixels)
{
    scrollY = (float) pixels;
    clampScroll();
    repaint();
}

void FileBrowserPanel::clampScroll()
{
    const int excess = juce::jmax (0, getContentHeight() - getViewHeight());
    scrollY = (float) juce::jlimit (0, excess, (int) scrollY);
}

//==============================================================================
void FileBrowserPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanelBackground);

    //-- Header ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (boldFont (12.5f));
    g.drawText (utf8 ("文件"), ui::px (14), ui::px (8), ui::px (100), ui::px (18),
                juce::Justification::centredLeft, false);

    const bool scanning = scanRunning.load();

    if (scanning)
    {
        //-- Phase 28: the scan line ------------------------------------------
        // It replaces the "SAMPLES · N · WAV" caption instead of sitting next to
        // it: during a scan the count is a moving target, and printing a number
        // that changes four times a second reads as a glitch.
        const auto  name     = scanningRoot.getFileName();
        const float progress = juce::jlimit (0.0f, 1.0f, scanProgress.load());
        const int   files    = scanFileCount.load();

        g.setColour (kScanAccent.withAlpha (0.92f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (utf8 ("正在加载 ") + (name.isEmpty() ? utf8 ("文件夹") : name)
                      + utf8 (" · ") + juce::String ((int) (progress * 100.0f)) + "%"
                      + utf8 (" · ") + juce::String (files) + utf8 (" 个文件"),
                    ui::px (14), ui::px (25), getWidth() - ui::px (110), ui::px (14),
                    juce::Justification::centredLeft, false);

        // The track is the full width under the header, so the bar is readable
        // from across the room even when the number is not.
        const auto track = juce::Rectangle<float> (0.0f, (float) headerHeight - ui::pf (2.5f),
                                                   (float) getWidth(), ui::pf (2.5f));
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRect (track);
        g.setColour (kScanAccent.withAlpha (0.85f));
        g.fillRect (track.withWidth (track.getWidth() * progress));
    }
    else
    {
        int fileCount = 0;

        for (const auto& entry : folders)
            if (entry.tree != nullptr)
                fileCount += entry.tree->fileCount;

        g.setColour (juce::Colours::white.withAlpha (0.26f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText ("SAMPLES" + separatorDot() + juce::String (fileCount) + utf8 (" · WAV"),
                    ui::px (14), ui::px (25), ui::px (150), ui::px (14),
                    juce::Justification::centredLeft, false);
    }

    {
        const auto button = getAddButtonBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (button, ui::pf (4));
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (button, ui::pf (4), 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.66f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("＋ 文件夹"), button.toNearestInt(), juce::Justification::centred, false);
    }

    g.setColour (kDivider);
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    //-- Rows -----------------------------------------------------------------
    const auto& rows = ensureRows();

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (getViewArea());

        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto bounds = getRowBounds (i);

            if (! bounds.intersects (getViewArea()))
                continue;

            const auto& row = rows[(size_t) i];

            if (i == hoveredRow)
            {
                g.setColour (kRowHover);
                g.fillRect (bounds);
            }

            const int  left = ui::px (10) + row.indent * indentWidth;
            const bool isRoot = (row.kind == Row::Kind::rootFolder);

            if (row.isFolder())
            {
                const bool expanded = isRoot ? folders[(size_t) row.folderIndex].expanded
                                             : isSubFolderExpanded (row.file);

                // Disclosure triangle.
                juce::Path caret;
                const float cx = (float) left + ui::pf (8);
                const float cy = (float) bounds.getCentreY();

                if (expanded)
                    caret.addTriangle (cx - ui::pf (3.5f), cy - ui::pf (2),
                                       cx + ui::pf (3.5f), cy - ui::pf (2),
                                       cx, cy + ui::pf (3));
                else
                    caret.addTriangle (cx - ui::pf (2), cy - ui::pf (3.5f),
                                       cx - ui::pf (2), cy + ui::pf (3.5f),
                                       cx + ui::pf (3), cy);

                g.setColour (juce::Colours::white.withAlpha (isRoot ? 0.42f : 0.30f));
                g.fillPath (caret);

                const auto name = row.label.isNotEmpty() ? row.label
                                                         : row.file.getFileName();

                g.setColour (kFolderText.withAlpha (isRoot ? 0.92f : 0.72f));
                g.setFont (isRoot ? boldFont (11.0f)
                                  : juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));

                const int textLeft = left + ui::px (20);

                g.drawText (name,
                            bounds.withTrimmedLeft (textLeft)
                                  .withTrimmedRight (ui::px (isRoot ? 34 : 26)),
                            juce::Justification::centredLeft, true);

                // File count, right aligned, quiet.
                g.setColour (juce::Colours::white.withAlpha (isRoot ? 0.26f : 0.20f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));

                const auto count = row.fileCount > 0 ? juce::String (row.fileCount)
                                                     : juce::String (utf8 ("…"));
                g.drawText (count, bounds.withTrimmedRight (ui::px (isRoot ? 8 : 4)),
                            juce::Justification::centredRight, false);

                if (isRoot && i == hoveredRow)
                {
                    const auto remove = getRemoveButtonBounds (i).toFloat();
                    g.setColour (juce::Colours::white.withAlpha (0.10f));
                    g.fillEllipse (remove);
                    g.setColour (juce::Colours::white.withAlpha (0.55f));
                    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
                    g.drawText ("x", remove.toNearestInt(), juce::Justification::centred, false);
                }
            }
            else
            {
                const bool exists = row.file.existsAsFile();
                const int  textLeft = left + ui::px (18);

                g.setColour (kFileText.withAlpha (exists ? 0.86f : 0.34f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
                g.drawText (row.file.getFileName(),
                            bounds.withTrimmedLeft (textLeft).withTrimmedRight (ui::px (26)),
                            juce::Justification::centredLeft, true);

                // A five-bar waveform instead of a generic file icon: the row is
                // audio, and the shape is the one thing that distinguishes it
                // from a text file at a glance.
                {
                    const float cx = (float) left + ui::pf (9);
                    const float cy = (float) bounds.getCentreY();
                    const float heights[] { 0.35f, 0.75f, 1.0f, 0.55f, 0.30f };

                    g.setColour (kFileText.withAlpha (exists ? 0.50f : 0.18f));

                    for (int b = 0; b < 5; ++b)
                    {
                        const float h = ui::pf (9) * heights[b];
                        g.fillRoundedRectangle (cx + ui::pf (b * 2.4f) - ui::pf (0.7f),
                                                cy - h * 0.5f, ui::pf (1.4f), h, ui::pf (0.7f));
                    }
                }

                // Drag grip. Without it a row reads as a label; with it, as
                // something you can pick up and drop on the roll.
                if (i == hoveredRow)
                {
                    const float gx = (float) bounds.getRight() - ui::pf (12);
                    const float gy = (float) bounds.getCentreY();

                    g.setColour (juce::Colours::white.withAlpha (0.34f));

                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 2; ++c)
                            g.fillEllipse (gx + (float) c * ui::pf (3.2f),
                                           gy - ui::pf (4) + (float) r * ui::pf (4),
                                           ui::pf (1.6f), ui::pf (1.6f));
                }

                //-- Phase 20: the audition indicator ------------------------
                // A file that is playing says so on its own row, with the
                // remaining three seconds drawn as a shrinking line. Without
                // this the click is invisible: the sound could be coming from
                // anywhere, and "did I hit the wrong row" has no answer.
                if (previewing && row.file.getFullPathName() == previewPath)
                {
                    const double p = juce::jlimit (0.0, 1.0, getPreviewProgress());

                    g.setColour (kRowHover.brighter (0.25f));
                    g.fillRect (bounds.reduced (0, 1));

                    g.setColour (kPreviewAccent.withAlpha (0.9f));
                    g.fillRect (juce::Rectangle<int> (bounds.getX() + textLeft - ui::px (2),
                                                      bounds.getBottom() - ui::px (2),
                                                      (int) ((float) (bounds.getWidth() - textLeft
                                                                       - ui::px (40)) * (float) p),
                                                      ui::px (2)));

                    g.setColour (kPreviewAccent);
                    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
                    g.drawText (utf8 ("试听"),
                                bounds.withTrimmedRight (ui::px (22)),
                                juce::Justification::centredRight, false);
                }
            }
        }
    }

    //-- Empty state ----------------------------------------------------------
    if (folders.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawFittedText (utf8 ("还没有文件夹。\n点「＋ 文件夹」挑一个采样目录，\n然后把文件拖进钢琴窗。"),
                          getViewArea().reduced (ui::px (16), ui::px (10)),
                          juce::Justification::centredTop, 4);
    }

    //-- Scroll indicator -----------------------------------------------------
    const int excess = getContentHeight() - getViewHeight();

    if (excess > 0)
    {
        const auto view = getViewArea();
        const float trackTop = (float) view.getY();
        const float trackHeight = (float) view.getHeight();
        const float thumbHeight = juce::jmax (ui::pf (18),
                                              trackHeight * (float) getViewHeight()
                                                / (float) juce::jmax (1, getContentHeight()));
        const float thumbTop = trackTop + (trackHeight - thumbHeight) * (scrollY / (float) excess);

        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) getWidth() - ui::pf (4), thumbTop,
                                                        ui::pf (3), thumbHeight), ui::pf (1.5f));
    }
}

void FileBrowserPanel::resized()
{
    clampScroll();
}

//==============================================================================
void FileBrowserPanel::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    dragStart = e.getPosition();

    if (getAddButtonBounds().contains (e.getPosition()))
    {
        chooser = std::make_unique<juce::FileChooser> (utf8 ("选择采样文件夹"),
                                                       juce::File::getSpecialLocation (
                                                           juce::File::userMusicDirectory),
                                                       juce::String());

        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto result = fc.getResult();

                                  if (result.isDirectory())
                                      addFolder (result);
                              });
        return;
    }

    const int row = rowAt (e.getPosition());
    pressedRow = row;

    if (row < 0)
        return;

    const auto& info = ensureRows()[(size_t) row];

    if (info.kind == Row::Kind::rootFolder)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, utf8 ("重新扫描"));
            menu.addItem (2, utf8 ("在资源管理器中显示"));
            menu.addItem (3, utf8 ("移除这个文件夹"));

            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea (localAreaToGlobal (getRowBounds (row))),
                                [this, index = info.folderIndex, dir = info.file] (int choice)
                                {
                                    if (choice == 1)
                                        rescanFolders();
                                    else if (choice == 2)
                                        dir.revealToUser();
                                    else if (choice == 3)
                                        removeFolder (index);
                                });
            return;
        }

        if (getRemoveButtonBounds (row).contains (e.getPosition()))
        {
            removeFolder (info.folderIndex);
            return;
        }

        auto& entry = folders[(size_t) info.folderIndex];
        entry.expanded = ! entry.expanded;
        invalidateRows();
        clampScroll();
        repaint();
        return;
    }

    if (info.kind == Row::Kind::subFolder)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, utf8 ("在资源管理器中显示"));

            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea (localAreaToGlobal (getRowBounds (row))),
                                [dir = info.file] (int choice)
                                {
                                    if (choice == 1)
                                        dir.revealToUser();
                                });
            return;
        }

        setSubFolderExpanded (info.file, ! isSubFolderExpanded (info.file));
    }
}

void FileBrowserPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || pressedRow < 0)
        return;

    if (e.getDistanceFromDragStart() < 6)
        return;

    const auto& rows = ensureRows();

    if (! juce::isPositiveAndBelow (pressedRow, (int) rows.size()))
        return;

    const auto& info = rows[(size_t) pressedRow];

    if (info.isFolder() || ! info.file.existsAsFile())
        return;

    auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);

    if (container == nullptr || container->isDragAndDropActive())
        return;

    dragging = true;

    // The payload is the absolute path and nothing else: the roll decides what
    // to do with it, and a string survives the trip between two components that
    // know nothing about each other.
    // The ScaledImage overload, not the plain-image one: the deprecated form
    // cannot say what DPI the picture was drawn at, so the drag ghost comes out
    // at the wrong size on a scaled display.
    container->startDragging (juce::var (info.file.getFullPathName()),
                              this,
                              juce::ScaledImage (makeDragImage (info.file.getFileName()), 1.0f),
                              true);
}

void FileBrowserPanel::mouseUp (const juce::MouseEvent& e)
{
    const int  row        = pressedRow;
    const bool wasDragging = dragging;

    pressedRow = -1;
    dragging   = false;

    // A drag is a drop, not a listen. Releasing over a DIFFERENT row is not a
    // click on that row either — the gesture belongs to where it started.
    if (wasDragging || row < 0 || rowAt (e.getPosition()) != row)
        return;

    const auto& rows = ensureRows();

    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& info = rows[(size_t) row];

    if (info.isFolder() || ! info.file.existsAsFile() || onPreviewRequested == nullptr)
        return;

    // Phase 20: one click, three seconds. The host decodes and starts the
    // engine; the engine stops whatever was already previewing, so two clicks in
    // quick succession cannot overlap.
    const double seconds = onPreviewRequested (info.file);

    if (seconds > 0.0)
        beginPreviewFeedback (info.file, seconds);
}

//==============================================================================
void FileBrowserPanel::beginPreviewFeedback (const juce::File& file, double seconds)
{
    previewPath       = file.getFullPathName();
    previewStartMs    = juce::Time::getMillisecondCounterHiRes();
    previewDurationMs = juce::jmax (1.0, seconds * 1000.0);
    previewing        = true;

    // 30 Hz is enough for a 3-second line and cheap enough to leave running for
    // the whole of it; the timer stops itself the moment the window is over.
    updateTimerState();
    repaint();
}

void FileBrowserPanel::endPreviewFeedback()
{
    if (! previewing)
        return;

    previewing = false;
    updateTimerState();
    repaint();
}

void FileBrowserPanel::updateTimerState()
{
    // One timer, two jobs: the audition line expires, the scan percentage
    // ticks. Starting it at 30 Hz for either is fine — the repaint is the same
    // header strip.
    if (previewing || scanRunning.load())
        startTimerHz (30);
    else
        stopTimer();
}

double FileBrowserPanel::getPreviewProgress() const
{
    if (! previewing || previewDurationMs <= 0.0)
        return -1.0;

    const double elapsed = juce::Time::getMillisecondCounterHiRes() - previewStartMs;

    return juce::jlimit (0.0, 1.0, elapsed / previewDurationMs);
}

void FileBrowserPanel::timerCallback()
{
    if (scanRunning.load())
    {
        // Only the header moves while a scan runs: repainting the whole list at
        // 30 Hz to update a percentage would make adding a folder the slowest
        // thing this panel ever does.
        repaint (0, 0, getWidth(), headerHeight);
    }

    // 120 ms of slack: the engine's own tail fade can outlast the nominal
    // length by a few milliseconds, and a line that snaps back to zero while the
    // sound is still ringing looks like a bug.
    if (previewing
          && juce::Time::getMillisecondCounterHiRes() - previewStartMs > previewDurationMs + 120.0)
        endPreviewFeedback();

    if (! previewing && ! scanRunning.load())
        stopTimer();
}

void FileBrowserPanel::mouseMove (const juce::MouseEvent& e)
{
    const int row = rowAt (e.getPosition());

    if (row != hoveredRow)
    {
        hoveredRow = row;
        repaint();
    }
}

void FileBrowserPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1)
    {
        hoveredRow = -1;
        repaint();
    }
}

void FileBrowserPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int excess = getContentHeight() - getViewHeight();

    if (excess <= 0)
        return;

    const float delta = wheel.deltaY * (float) getViewHeight() * 0.9f;

    if (delta != 0.0f)
    {
        setScrollY ((int) (scrollY - delta));
        return;
    }

    // Horizontal wheels and trackpads that only report deltaX: treat it as
    // vertical instead of ignoring the gesture.
    if (wheel.deltaX != 0.0f)
        setScrollY ((int) (scrollY - wheel.deltaX * (float) getViewHeight()));
}

//==============================================================================
juce::Image FileBrowserPanel::makeDragImage (const juce::String& name) const
{
    const int width  = ui::px (150);
    const int height = ui::px (24);

    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics g (image);

    g.setColour (theme::surface3.withAlpha (0.94f));
    g.fillRoundedRectangle (image.getBounds().toFloat(), ui::pf (4));
    g.setColour (kAccent.withAlpha (0.55f));
    g.drawRoundedRectangle (image.getBounds().toFloat().reduced (0.5f), ui::pf (4), 1.0f);

    g.setColour (juce::Colours::white.withAlpha (0.72f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (utf8 ("♪ ") + name, image.getBounds().reduced (ui::px (8), 0),
                juce::Justification::centredLeft, true);

    return image;
}

} // namespace atomic

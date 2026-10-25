#include "FileBrowserPanel.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"

#include <algorithm>

namespace atomic
{

namespace
{
    const juce::Colour kPanelBackground { 0xff14151a };
    const juce::Colour kDivider         { 0xff262832 };
    const juce::Colour kRowHover        { 0x0dffffff };
    const juce::Colour kFileText        { 0xffb9bfcc };
    const juce::Colour kFolderText      { 0xffd6dae2 };
    const juce::Colour kAccent          { 0xff4fd1c5 };
    /** Phase 20: the audition line. Green, because it is "playing", and green
        is the one colour in this palette nothing else uses. */
    const juce::Colour kPreviewAccent   { 0xff7bd88f };

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    /** A folder is scanned at most this deep, so adding "C:\\" by accident
        cannot freeze the message thread for a minute. */
    constexpr int maxScanDepth = 6;
    constexpr int maxFilesPerFolder = 800;
}

//==============================================================================
FileBrowserPanel::FileBrowserPanel()
{
    setOpaque (true);
    loadFromDisk();
}

FileBrowserPanel::~FileBrowserPanel()
{
    stopTimer();
}

juce::File FileBrowserPanel::getStoreFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("AtomicDAW")
             .getChildFile ("sample-folders.txt");
}

//==============================================================================
void FileBrowserPanel::loadFromDisk()
{
    const auto store = getStoreFile();

    if (! store.existsAsFile())
        return;

    const auto lines = juce::StringArray::fromLines (store.loadFileAsString());

    for (const auto& line : lines)
    {
        const auto path = line.trim();

        if (path.isEmpty() || path.startsWith ("#"))
            continue;

        addFolder (juce::File (path));
    }
}

void FileBrowserPanel::saveToDisk() const
{
    juce::StringArray lines;
    lines.add ("# AtomicDAW sample folders — one path per line, safe to edit");
    lines.add ("# 采样文件夹列表，一行一个路径");

    for (const auto& entry : folders)
        lines.add (entry.folder.getFullPathName());

    const auto store = getStoreFile();
    store.getParentDirectory().createDirectory();
    store.replaceWithText (lines.joinIntoString ("\n"));
}

//==============================================================================
void FileBrowserPanel::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return;

    const auto canonical = folder.getFullPathName();

    for (const auto& entry : folders)
        if (entry.folder.getFullPathName() == canonical)
            return;                                  // already listed

    FolderEntry entry;
    entry.folder = folder;
    scanFolder (entry);
    folders.push_back (std::move (entry));

    saveToDisk();
    clampScroll();

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
    clampScroll();

    if (onFoldersChanged)
        onFoldersChanged();

    repaint();
}

void FileBrowserPanel::rescanFolders()
{
    for (auto& entry : folders)
        scanFolder (entry);

    clampScroll();
    repaint();
}

void FileBrowserPanel::scanFolder (FolderEntry& entry)
{
    entry.files.clear();

    if (! entry.folder.isDirectory())
        return;

    // Recursive but bounded: samples usually live one or two levels down, and a
    // folder with eight hundred hits is already more than the panel can show.
    juce::Array<juce::File> found;
    entry.folder.findChildFiles (found, juce::File::findFiles, true, "*.wav;*.wave");

    for (const auto& file : found)
    {
        if ((int) entry.files.size() >= maxFilesPerFolder)
            break;

        if (! isSampleFile (file))
            continue;

        // Depth guard: count separators below the folder itself.
        const auto relative = file.getRelativePathFrom (entry.folder);
        const int  depth    = juce::StringArray::fromTokens (relative, "/\\", "").size() - 1;

        if (depth > maxScanDepth)
            continue;

        entry.files.push_back (file);
    }

    std::sort (entry.files.begin(), entry.files.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });
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

//==============================================================================
std::vector<FileBrowserPanel::Row> FileBrowserPanel::buildRows() const
{
    std::vector<Row> rows;

    for (int i = 0; i < (int) folders.size(); ++i)
    {
        const auto& entry = folders[(size_t) i];

        Row header;
        header.isFolder    = true;
        header.folderIndex = i;
        header.file        = entry.folder;
        rows.push_back (std::move (header));

        if (! entry.expanded)
            continue;

        for (const auto& file : entry.files)
        {
            Row row;
            row.folderIndex = i;
            row.file        = file;
            row.indent      = 1;
            rows.push_back (std::move (row));
        }
    }

    return rows;
}

int FileBrowserPanel::getContentHeight() const
{
    int total = 0;

    for (const auto& row : buildRows())
        total += row.isFolder ? folderHeight : fileHeight;

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
    const auto rows = buildRows();

    int y = headerHeight - (int) scrollY;

    for (int i = 0; i < rowIndex && i < (int) rows.size(); ++i)
        y += rows[(size_t) i].isFolder ? folderHeight : fileHeight;

    const int height = juce::isPositiveAndBelow (rowIndex, (int) rows.size())
                         ? (rows[(size_t) rowIndex].isFolder ? folderHeight : fileHeight)
                         : 0;

    return { 0, y, getWidth(), height };
}

int FileBrowserPanel::rowAt (juce::Point<int> position) const
{
    if (! getViewArea().contains (position))
        return -1;

    const auto rows = buildRows();

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

    int fileCount = 0;

    for (const auto& entry : folders)
        fileCount += (int) entry.files.size();

    g.setColour (juce::Colours::white.withAlpha (0.26f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
    g.drawText ("SAMPLES" + separatorDot() + juce::String (fileCount) + utf8 (" · WAV"),
                ui::px (14), ui::px (25), ui::px (150), ui::px (14),
                juce::Justification::centredLeft, false);

    {
        const auto button = getAddButtonBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (button, ui::pf (5));
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (button, ui::pf (5), 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.66f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("＋ 文件夹"), button.toNearestInt(), juce::Justification::centred, false);
    }

    g.setColour (kDivider);
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    //-- Rows -----------------------------------------------------------------
    const auto rows = buildRows();

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

            if (row.isFolder)
            {
                const bool expanded = folders[(size_t) row.folderIndex].expanded;

                // Disclosure triangle.
                juce::Path caret;
                const float cx = ui::pf (18);
                const float cy = (float) bounds.getCentreY();

                if (expanded)
                    caret.addTriangle (cx - ui::pf (3.5f), cy - ui::pf (2),
                                       cx + ui::pf (3.5f), cy - ui::pf (2),
                                       cx, cy + ui::pf (3));
                else
                    caret.addTriangle (cx - ui::pf (2), cy - ui::pf (3.5f),
                                       cx - ui::pf (2), cy + ui::pf (3.5f),
                                       cx + ui::pf (3), cy);

                g.setColour (juce::Colours::white.withAlpha (0.42f));
                g.fillPath (caret);

                g.setColour (kFolderText.withAlpha (0.92f));
                g.setFont (boldFont (11.0f));
                g.drawText (row.file.getFileName(),
                            bounds.withTrimmedLeft (ui::px (30)).withTrimmedRight (ui::px (34)),
                            juce::Justification::centredLeft, true);

                // File count, right aligned, quiet.
                g.setColour (juce::Colours::white.withAlpha (0.26f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
                g.drawText (juce::String ((int) folders[(size_t) row.folderIndex].files.size()),
                            bounds.withTrimmedRight (ui::px (8)),
                            juce::Justification::centredRight, false);

                if (i == hoveredRow)
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

                g.setColour (kFileText.withAlpha (exists ? 0.86f : 0.34f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
                g.drawText (row.file.getFileName(),
                            bounds.withTrimmedLeft (ui::px (38)).withTrimmedRight (ui::px (26)),
                            juce::Justification::centredLeft, true);

                // A five-bar waveform instead of a generic file icon: the row is
                // audio, and the shape is the one thing that distinguishes it
                // from a text file at a glance.
                {
                    const float cx = ui::pf (20);
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
                    g.fillRect (juce::Rectangle<int> (bounds.getX() + ui::px (34),
                                                      bounds.getBottom() - ui::px (2),
                                                      (int) ((float) (bounds.getWidth() - ui::px (74))
                                                               * (float) p),
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

    const auto rows = buildRows();
    const auto& info = rows[(size_t) row];

    if (info.isFolder)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, utf8 ("重新扫描"));
            menu.addItem (2, utf8 ("移除这个文件夹"));

            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea (localAreaToGlobal (getRowBounds (row))),
                                [this, index = info.folderIndex] (int choice)
                                {
                                    if (choice == 1)
                                        rescanFolders();
                                    else if (choice == 2)
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
        clampScroll();
        repaint();
    }
}

void FileBrowserPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || pressedRow < 0)
        return;

    if (e.getDistanceFromDragStart() < 6)
        return;

    const auto rows = buildRows();

    if (! juce::isPositiveAndBelow (pressedRow, (int) rows.size()))
        return;

    const auto& info = rows[(size_t) pressedRow];

    if (info.isFolder || ! info.file.existsAsFile())
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

    const auto rows = buildRows();

    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& info = rows[(size_t) row];

    if (info.isFolder || ! info.file.existsAsFile() || onPreviewRequested == nullptr)
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
    startTimerHz (30);
    repaint();
}

void FileBrowserPanel::endPreviewFeedback()
{
    if (! previewing)
        return;

    previewing = false;
    stopTimer();
    repaint();
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
    // 120 ms of slack: the engine's own tail fade can outlast the nominal
    // length by a few milliseconds, and a line that snaps back to zero while the
    // sound is still ringing looks like a bug.
    if (! previewing
          || juce::Time::getMillisecondCounterHiRes() - previewStartMs > previewDurationMs + 120.0)
    {
        endPreviewFeedback();
        return;
    }

    repaint();
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

    g.setColour (juce::Colour (0xf01b1d24));
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

#include "AutomationWindow.h"

#include "Util/Metrics.h"

#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{

namespace
{
    const juce::Colour& kBackground = theme::surface0;
    const juce::Colour kPanel       = theme::surface2;
    const juce::Colour& kRowEven    = theme::surface1;
    const juce::Colour kRowOdd      = theme::surface1;
    const juce::Colour& kOutline    = theme::surface4;
    const juce::Colour& kText       = theme::inkMuted;
    const juce::Colour& kAccent     = theme::accent;
    const juce::Colour& kPlayhead   = theme::record;

    /** Where the two lines of a track header start inside their row.

        The column is exactly `ui::gutterWidth` wide — the piano keyboard's
        width — so there is room for a name, a parameter and two hit targets,
        and nothing else. The content is centred vertically instead of being
        pinned to fixed offsets, because a row can be squeezed to 36 px or
        stretched to 220 px, and both have to look deliberate. */
    int headerContentTop (juce::Rectangle<int> row)
    {
        return row.getY() + juce::jmax (ui::px (3), (row.getHeight() - ui::px (32)) / 2);
    }
}

//==============================================================================
AutomationWindow::AutomationWindow (LayerStack& layerStack, RackModel& rackToUse,
                                    AutomationModel& automationModelToUse,
                                    MixerModel& mixerModelToUse)
    : stack (layerStack), rack (rackToUse), automation (automationModelToUse),
      mixer (mixerModelToUse)
{
    stack.addListener (this);
    rack.addListener (this);
    automation.addListener (this);

    setWantsKeyboardFocus (false);
}

AutomationWindow::~AutomationWindow()
{
    automation.removeListener (this);
    rack.removeListener (this);
    stack.removeListener (this);
}

//==============================================================================
AutomationWindow::Resolved AutomationWindow::resolve (const AutomationTrack& track) const
{
    Resolved resolved;

    if (track.owner == AutomationOwner::master)
    {
        // One bus, so no lookup: the lane lives in the mixer model, next to the
        // master fader it multiplies.
        const int target = juce::jlimit (0, MixerModel::numMasterParameters - 1, track.target);

        resolved.lane      = &const_cast<MixerModel&> (mixer).automationFor (target);
        resolved.name      = utf8 ("主输出");
        resolved.colour    = groupPalette::group1;   // 主输出：金色系身份，随色板降饱和
        resolved.parameter = target == 1 ? utf8 ("主声相")
                           : target == 2 ? utf8 ("主静音")
                                         : utf8 ("主音量");
    }
    else if (track.owner == AutomationOwner::layer)
    {
        auto* layer = const_cast<LayerStack&> (stack).findLayerById (track.ownerId);

        if (layer == nullptr)
            return resolved;

        const auto target = (LayerAutomationTarget) juce::jlimit (0, 1, track.target);

        resolved.lane      = &layer->automationFor (target);
        resolved.name      = layer->getName();
        resolved.colour    = layer->getColour();
        resolved.parameter = target == LayerAutomationTarget::pan ? utf8 ("声相") : utf8 ("音量");
    }
    else if (track.owner == AutomationOwner::group)
    {
        // Group ids are unique across the session, so this scan finds at most
        // one owner. Resolved on every paint instead of caching a pointer: a
        // group can be dissolved while this window is open, and a cached
        // AutomationLane* would dangle the moment it is.
        for (int i = 0; i < stack.getNumLayers(); ++i)
        {
            auto* layer = const_cast<LayerStack&> (stack).getLayer (i);

            if (layer == nullptr)
                continue;

            if (auto* group = layer->findGroup (track.ownerId))
            {
                // Phase 20: two lanes per group now, addressed by target the
                // same way a layer's two are.
                // Phase 24: and a third, which drives a parameter of the synth
                // the group is mounted on. The window cannot name the plugin
                // parameter (it has no plugin host and must not grow one), so
                // the row says what KIND of curve it is and the inline box —
                // which does have the list — says which parameter.
                const auto target = (GroupAutomationTarget) juce::jlimit (0, numGroupAutomationTargets - 1,
                                                                         track.target);

                resolved.lane      = &group->automationFor (target);
                resolved.name      = group->getName();
                resolved.colour    = group->getColour();
                resolved.parameter = target == GroupAutomationTarget::pan    ? utf8 ("组声相")
                                   : target == GroupAutomationTarget::plugin ? utf8 ("音源参数")
                                                                             : utf8 ("组音量");
                break;
            }
        }
    }
    else
    {
        auto* slot = const_cast<RackModel&> (rack).findSlot (track.ownerId);

        if (slot == nullptr)
            return resolved;

        const auto target = (SlotAutomationTarget) juce::jlimit (0, numSlotAutomationTargets - 1,
                                                                 track.target);

        resolved.lane      = &slot->automationFor (target);
        resolved.name      = slot->getDisplayName();
        resolved.colour    = slot->colour;
        // Phase 42: the window still cannot name the plugin's parameter — it has
        // no plugin host and must not grow one — so the third row says what KIND
        // of curve it is and the rack card's submenu says which parameter.
        //
        // Phase 43: the strip grew three rows: 声相 / 静音 / 独奏, named the way
        // the console names them.
        resolved.parameter = target == SlotAutomationTarget::filter ? utf8 ("滤波")
                           : target == SlotAutomationTarget::plugin ? utf8 ("音源参数")
                           : target == SlotAutomationTarget::pan    ? utf8 ("声相")
                           : target == SlotAutomationTarget::mute   ? utf8 ("静音")
                           : target == SlotAutomationTarget::solo   ? utf8 ("独奏")
                                                                    : utf8 ("音量");
    }

    return resolved;
}

AutomationWindow::Resolved AutomationWindow::resolveTrackAt (int index) const
{
    const auto& tracks = automation.getTracks();

    if (! juce::isPositiveAndBelow (index, (int) tracks.size()))
        return {};

    return resolve (tracks[(size_t) index]);
}

//==============================================================================
juce::Rectangle<int> AutomationWindow::getHeaderArea() const
{
    return getLocalBounds().withHeight (headerHeight);
}

juce::Rectangle<int> AutomationWindow::getListArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withWidth (listWidth);
}

juce::Rectangle<int> AutomationWindow::getCanvasArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedLeft (listWidth);
}

juce::Rectangle<int> AutomationWindow::getCanvasRulerArea() const
{
    return getCanvasArea().withHeight (rulerHeight);
}

juce::Rectangle<int> AutomationWindow::getRowsArea() const
{
    return getCanvasArea().withTrimmedTop (rulerHeight);
}

int AutomationWindow::getTrackHeight (int index) const
{
    const auto& tracks = automation.getTracks();

    if (! juce::isPositiveAndBelow (index, (int) tracks.size()))
        return rowHeight;

    const auto it = trackHeights.find (tracks[(size_t) index].id);

    return it != trackHeights.end() ? it->second : rowHeight;
}

void AutomationWindow::setTrackHeight (int index, int height)
{
    const auto& tracks = automation.getTracks();

    if (! juce::isPositiveAndBelow (index, (int) tracks.size()))
        return;

    trackHeights[tracks[(size_t) index].id] = juce::jlimit (minRowHeight, maxRowHeight, height);
    clampScroll();
    repaint();
}

int AutomationWindow::getContentHeight() const
{
    int total = 0;

    for (int i = 0; i < automation.getNumTracks(); ++i)
        total += getTrackHeight (i);

    return total;
}

int AutomationWindow::rowTopAt (int index) const
{
    int top = 0;

    for (int i = 0; i < index; ++i)
        top += getTrackHeight (i);

    return top;
}

juce::Rectangle<int> AutomationWindow::getListRowBounds (int index) const
{
    const auto rows = getRowsArea();
    const int  height = getTrackHeight (index);

    return { 0, rows.getY() - scrollY + rowTopAt (index), listWidth, height };
}

juce::Rectangle<float> AutomationWindow::getCanvasRowBounds (int index) const
{
    const auto rows = getRowsArea();

    return { (float) rows.getX(), (float) (rows.getY() - scrollY + rowTopAt (index)),
             (float) rows.getWidth(), (float) getTrackHeight (index) };
}

juce::Rectangle<int> AutomationWindow::getResizeBounds (int index) const
{
    const auto row = getListRowBounds (index);

    return { 0, row.getBottom() - resizeZone, getWidth(), resizeZone };
}

juce::Rectangle<int> AutomationWindow::getDeleteBounds (int index) const
{
    auto row = getListRowBounds (index);

    // Top-right corner: the name below it is trimmed to leave this space, and
    // the parameter line underneath is untouched, so the cross never lands on
    // text. It used to be vertically centred, which was fine in a 140 px
    // column and impossible in a 74 px one.
    return { row.getRight() - ui::px (15), headerContentTop (row) + ui::px (4),
             ui::px (9), ui::px (9) };
}

juce::Rectangle<int> AutomationWindow::getEnableBounds (int index) const
{
    auto row = getListRowBounds (index);

    // On the parameter line, in front of the word it switches on, centred on
    // that line's ink. Anchored to the row's content rather than to its bottom,
    // so a squeezed row cannot put the dot on top of the name.
    return { row.getX() + ui::px (7), headerContentTop (row) + ui::px (23),
             ui::px (8), ui::px (8) };
}

int AutomationWindow::visibleRowCount() const
{
    return juce::jmax (1, getRowsArea().getHeight() / rowHeight);
}

void AutomationWindow::clampScroll()
{
    const int content = getContentHeight();
    const int maxScroll = juce::jmax (0, content - getRowsArea().getHeight());

    scrollY = juce::jlimit (0, maxScroll, scrollY);
}

//==============================================================================
void AutomationWindow::setTimeline (double lengthBeatsIn, double pixelsPerBeatIn)
{
    lengthBeats   = juce::jmax (1.0, lengthBeatsIn);
    pixelsPerBeat = juce::jmax (4.0, pixelsPerBeatIn);
    repaint();
}

void AutomationWindow::setScrollBeats (double beats)
{
    if (juce::approximatelyEqual (beats, scrollBeats))
        return;

    scrollBeats = juce::jmax (0.0, beats);
    repaint();
}

void AutomationWindow::setPlayheadBeat (double beats)
{
    if (juce::approximatelyEqual (beats, playheadBeat))
        return;

    playheadBeat = beats;
    repaint();
}

void AutomationWindow::revealTrack (int trackId)
{
    const auto& tracks = automation.getTracks();
    int index = -1;

    for (int i = 0; i < (int) tracks.size(); ++i)
        if (tracks[(size_t) i].id == trackId)
            index = i;

    if (index < 0)
        return;

    const int top    = rowTopAt (index);
    const int bottom = top + getTrackHeight (index);
    const int height = getRowsArea().getHeight();

    if (top < scrollY)
        scrollY = top;
    else if (height > 0 && bottom > scrollY + height)
        scrollY = bottom - height;

    clampScroll();
    repaint();
}

//==============================================================================
void AutomationWindow::automationTracksChanged()
{
    clampScroll();
    repaint();
}

//==============================================================================
juce::Point<float> AutomationWindow::beatToPoint (const juce::Rectangle<float>& row,
                                                  double beat, float value) const
{
    return { getRowsArea().getX() + (float) ((beat - scrollBeats) * pixelsPerBeat),
             row.getBottom() - 4.0f - juce::jlimit (0.0f, 1.0f, value) * (row.getHeight() - 8.0f) };
}

double AutomationWindow::beatAtX (float x) const
{
    return scrollBeats + (double) (x - (float) getRowsArea().getX()) / pixelsPerBeat;
}

// Phase 18: the same 1/16 grid the piano roll snaps to. Kept as a literal here
// rather than reaching into PianoRollComponent — the automation window has no
// business depending on the roll, and 0.25 IS the project's grid.
static constexpr double snapGridBeats = 0.25;

double AutomationWindow::snappedBeatAtX (float x) const
{
    const double raw = beatAtX (x);

    if (! snapEnabled)
        return raw;

    return std::round (raw / snapGridBeats) * snapGridBeats;
}

void AutomationWindow::setSnapEnabled (bool shouldSnap)
{
    snapEnabled = shouldSnap;
}

float AutomationWindow::valueAtY (const juce::Rectangle<float>& row, float y) const
{
    return juce::jlimit (0.0f, 1.0f, (row.getBottom() - 4.0f - y) / juce::jmax (1.0f, row.getHeight() - 8.0f));
}

int AutomationWindow::rowIndexAtY (float y) const
{
    const auto rows = getRowsArea();

    if (y < (float) rows.getY() || y >= (float) rows.getBottom())
        return -1;

    const int offset = scrollY + (int) (y - (float) rows.getY());
    int       top    = 0;

    for (int i = 0; i < automation.getNumTracks(); ++i)
    {
        const int height = getTrackHeight (i);

        if (offset < top + height)
            return i;

        top += height;
    }

    return -1;
}

int AutomationWindow::pointIndexAt (int rowIndex, juce::Point<float> position) const
{
    auto resolved = resolveTrackAt (rowIndex);

    if (resolved.lane == nullptr)
        return -1;

    const auto row = getCanvasRowBounds (rowIndex);
    const auto& points = resolved.lane->getPoints();

    for (int i = 0; i < (int) points.size(); ++i)
        if (beatToPoint (row, points[(size_t) i].beat, points[(size_t) i].value)
                .getDistanceFrom (position) < 7.0f)
            return i;

    return -1;
}

//==============================================================================
void AutomationWindow::selectTrack (int index)
{
    const auto& tracks = automation.getTracks();

    if (! juce::isPositiveAndBelow (index, (int) tracks.size()))
        return;

    automation.selectTrack (tracks[(size_t) index].id);
}

void AutomationWindow::deleteTrack (int index)
{
    const auto& tracks = automation.getTracks();

    if (! juce::isPositiveAndBelow (index, (int) tracks.size()))
        return;

    const auto track = tracks[(size_t) index];
    auto resolved = resolve (track);

    // Deleting the row must stop the curve from driving anything: a hidden but
    // still enabled lane would keep moving the fader with no row to show it.
    if (resolved.lane != nullptr)
        resolved.lane->setEnabled (false);

    automation.removeTrack (track.id);

    if (onChanged != nullptr)
        onChanged();
}

void AutomationWindow::toggleTrackEnabled (int index)
{
    auto resolved = resolveTrackAt (index);

    if (resolved.lane == nullptr)
        return;

    resolved.lane->setEnabled (! resolved.lane->isEnabled());
    repaint();

    if (onChanged != nullptr)
        onChanged();
}

//==============================================================================
void AutomationWindow::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    drawHeader (g);
    drawList (g);
    drawCanvas (g);
}

void AutomationWindow::drawHeader (juce::Graphics& g)
{
    //-- Header strip ---------------------------------------------------------
    {
        auto header = getHeaderArea();

        g.setColour (kPanel);
        g.fillRect (header);

        // Two pieces of text have to share the width of the track-header column
        // above the rows — 74 design units — so the title and the count are laid
        // out tight against it instead of the old 60 + 46 split that ended
        // 20 px deep inside the canvas. The hint starts where the canvas does.
        g.setColour (juce::Colours::white.withAlpha (0.82f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (11.5f)).withStyle ("Bold")));
        g.drawText (utf8 ("自动化"),
                    header.withTrimmedLeft (ui::px (12)).withWidth (ui::px (50)),
                    juce::Justification::centredLeft, false);

        g.setColour (kAccent.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (juce::String (automation.getNumTracks()) + utf8 (" 轨"),
                    header.withTrimmedLeft (ui::px (60)).withWidth (ui::px (34)),
                    juce::Justification::centredLeft, false);

        g.setColour (juce::Colours::white.withAlpha (0.42f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
        g.drawText (utf8 ("右键图层 / 音源 / 编组 → 创建自动化      画布：单击加点 / 拖动改点 / 右键删点      拖行底边改高度"),
                    header.withTrimmedLeft (listWidth + ui::px (10)).withTrimmedRight (ui::px (12)),
                    juce::Justification::centredLeft, false);

        g.setColour (kOutline);
        g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());
    }
}

void AutomationWindow::drawList (juce::Graphics& g)
{
    auto list = getListArea();

    g.setColour (kPanel);
    g.fillRect (list);

    const auto& tracks = automation.getTracks();

    if (tracks.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        // Two short lines, and shorter than they used to be: the column is now
        // the width of the piano keyboard, so "右键控件 → 创建自动化" would wrap
        // into three broken lines. The header strip spells the long version out.
        g.drawFittedText (utf8 ("还没有轨道\n右键控件创建"),
                          list.reduced (ui::px (8), ui::px (18)), juce::Justification::topLeft, 3);
        return;
    }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (list);

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        auto row = getListRowBounds (i);

        if (row.getBottom() < list.getY() || row.getY() > list.getBottom())
            continue;

        const auto resolved = resolve (tracks[(size_t) i]);
        const bool selected = automation.getSelectedTrackId() == tracks[(size_t) i].id;

        g.setColour (selected ? resolved.colour.withAlpha (0.16f)
                              : (i % 2 == 0 ? kRowEven : kRowOdd));
        g.fillRect (row);

        // Colour tab: the same colour the layer uses on the roll.
        g.setColour (resolved.colour.withAlpha (resolved.lane != nullptr ? 0.9f : 0.25f));
        g.fillRect (row.getX(), row.getY(), ui::px (3), row.getHeight());

        const int contentTop = headerContentTop (row);

        g.setColour (resolved.lane != nullptr ? juce::Colours::white.withAlpha (0.88f)
                                              : juce::Colours::white.withAlpha (0.30f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.5f))));
        // Two short lines fit a 74-unit column; three do not. Truncation with an
        // ellipsis is better than a name that runs under the delete cross.
        g.drawText (resolved.name.isNotEmpty() ? resolved.name : utf8 ("（已删除）"),
                    juce::Rectangle<int> (row.getX() + ui::px (10), contentTop,
                                          listWidth - ui::px (32), ui::px (16)),
                    juce::Justification::centredLeft, true);

        g.setColour (kText.withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        // A 20-unit strip, not 14: the label sits on the row's second line and
        // the strip has to be taller than the glyphs or they touch the name.
        g.drawText (resolved.parameter,
                    juce::Rectangle<int> (row.getX() + ui::px (22), contentTop + ui::px (16),
                                          listWidth - ui::px (28), ui::px (20)),
                    juce::Justification::centredLeft, false);

        // Enable dot: filled when the curve is driving the parameter.
        const auto dot = getEnableBounds (i).toFloat();

        g.setColour (resolved.lane != nullptr && resolved.lane->isEnabled()
                         ? resolved.colour : juce::Colours::white.withAlpha (0.18f));
        g.fillEllipse (dot);

        // Delete cross.
        const auto del = getDeleteBounds (i).toFloat();
        g.setColour (hoveredRow == i && hoveredPart == 2
                         ? theme::record : kText.withAlpha (0.42f));
        g.drawLine (del.getX(), del.getY(), del.getRight(), del.getBottom(), ui::pf (1.2));
        g.drawLine (del.getRight(), del.getY(), del.getX(), del.getBottom(), ui::pf (1.2));

        // Phase 12: the bottom edge is a grab handle. Drawn as a two-pixel line
        // that lights up under the mouse, because a resize that only exists in
        // the cursor shape is a resize nobody finds.
        const bool hotResize = hoveredRow == i && hoveredPart == 5;

        g.setColour (hotResize ? resolved.colour.withAlpha (0.85f)
                               : kOutline.withAlpha (0.7f));
        g.fillRect (row.getX(), row.getBottom() - (hotResize ? 2 : 1), row.getWidth(),
                    hotResize ? 2 : 1);
    }

    g.setColour (kOutline);
    g.drawVerticalLine (list.getRight() - 1, (float) list.getY(), (float) list.getBottom());
}

void AutomationWindow::drawCanvas (juce::Graphics& g)
{
    auto canvas = getCanvasArea();
    auto ruler  = getCanvasRulerArea();

    g.setColour (kRowOdd);
    g.fillRect (canvas);

    //-- Beat ruler -----------------------------------------------------------
    g.setColour (kPanel);
    g.fillRect (ruler);

    const double visible = (double) getRowsArea().getWidth() / pixelsPerBeat;
    const int firstBar = (int) std::floor (scrollBeats / 4.0);
    const int lastBar  = (int) std::ceil ((scrollBeats + visible) / 4.0);

    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));

    for (int bar = firstBar; bar <= lastBar; ++bar)
    {
        const double beat = bar * 4.0;

        if (beat < 0.0 || beat > lengthBeats)
            continue;

        const float x = (float) getRowsArea().getX() + (float) ((beat - scrollBeats) * pixelsPerBeat);

        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawVerticalLine ((int) x, (float) ruler.getY() + 5.0f, (float) ruler.getBottom());

        g.setColour (kText.withAlpha (0.8f));
        g.drawText (juce::String (bar + 1), (int) x + ui::px (4), ruler.getY() + ui::px (1),
                    ui::px (34), ui::px (16),
                    juce::Justification::centredLeft, false);
    }

    const auto& tracks = automation.getTracks();

    if (! tracks.empty())
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (getRowsArea());

        for (int i = 0; i < (int) tracks.size(); ++i)
        {
            auto row = getCanvasRowBounds (i);

            if (row.getBottom() < (float) getRowsArea().getY() || row.getY() > (float) getRowsArea().getBottom())
                continue;

            drawTrackRow (g, i, tracks[(size_t) i], resolve (tracks[(size_t) i]));
        }
    }

    //-- Playhead across every row -------------------------------------------
    // Phase 8: the same playhead the piano roll draws, in the same colour, plus
    // a marker in the ruler — a bare line in a window full of curves reads as
    // just another automation line.
    if (playheadBeat >= 0.0)
    {
        const float x = (float) getRowsArea().getX() + (float) ((playheadBeat - scrollBeats) * pixelsPerBeat);

        if (x >= (float) getRowsArea().getX() && x <= (float) getRowsArea().getRight())
        {
            {

                juce::Graphics::ScopedSaveState line (g);
                g.reduceClipRegion (getRowsArea());

                g.setColour (kPlayhead.withAlpha (0.10f));
                g.fillRect (juce::Rectangle<float> (x - 2.0f, (float) getRowsArea().getY(),
                                                    4.0f, (float) getRowsArea().getHeight()));

                g.setColour (kPlayhead.withAlpha (0.92f));
                g.drawVerticalLine ((int) x, (float) getRowsArea().getY(), (float) getRowsArea().getBottom());
            }

            {
                juce::Graphics::ScopedSaveState marker (g);
                g.reduceClipRegion (getCanvasRulerArea().expanded (0, 1));

                juce::Path head;
                head.addTriangle (x - 4.5f, (float) ruler.getY() + 1.0f,
                                  x + 4.5f, (float) ruler.getY() + 1.0f,
                                  x,        (float) ruler.getY() + 9.0f);

                g.setColour (kPlayhead);
                g.fillPath (head);
            }
        }
    }
}

void AutomationWindow::drawTrackRow (juce::Graphics& g, int index,
                                     const AutomationTrack& track, const Resolved& resolved)
{
    const auto row = getCanvasRowBounds (index);
    const bool selected = automation.getSelectedTrackId() == track.id;

    g.setColour (selected ? resolved.colour.withAlpha (0.075f)
                          : (index % 2 == 0 ? kRowEven.withAlpha (0.55f) : juce::Colours::transparentBlack));
    if (selected || index % 2 == 0)
        g.fillRect (row);

    // Bar grid.
    const double visible = (double) getRowsArea().getWidth() / pixelsPerBeat;
    const int firstBar = (int) std::floor (scrollBeats / 4.0);
    const int lastBar  = (int) std::ceil ((scrollBeats + visible) / 4.0);

    for (int bar = firstBar; bar <= lastBar; ++bar)
    {
        const double beat = bar * 4.0;

        if (beat < 0.0 || beat > lengthBeats)
            continue;

        const float x = (float) getRowsArea().getX() + (float) ((beat - scrollBeats) * pixelsPerBeat);
        g.setColour (juce::Colours::white.withAlpha (0.055f));
        g.drawVerticalLine ((int) x, row.getY(), row.getBottom());
    }

    // Mid line, so "0.5" has a reference.
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine ((int) row.getCentreY(), row.getX(), row.getRight());

    if (resolved.lane == nullptr || resolved.lane->getPoints().empty())
        return;

    const auto& points = resolved.lane->getPoints();
    const bool enabled = resolved.lane->isEnabled();

    juce::Path path;

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto p = beatToPoint (row, points[(size_t) i].beat, points[(size_t) i].value);

        if (i == 0)
            path.startNewSubPath (p);
        else
            path.lineTo (p);
    }

    // Fill under the curve: a 0..1 lane with no shading reads as a stray line.
    juce::Path filled = path;
    filled.lineTo (beatToPoint (row, points.back().beat, 0.0f));
    filled.lineTo (beatToPoint (row, points.front().beat, 0.0f));
    filled.closeSubPath();

    g.setColour (resolved.colour.withAlpha (enabled ? 0.16f : 0.05f));
    g.fillPath (filled);

    g.setColour (resolved.colour.withAlpha (enabled ? 0.95f : 0.30f));
    g.strokePath (path, juce::PathStrokeType (1.6f));

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto p = beatToPoint (row, points[(size_t) i].beat, points[(size_t) i].value);
        const bool hot = (hoveredRow == index && hoveredPart == 4 && hoveredPoint == i)
                           || (dragPointRow == index && dragPointIndex == i);

        g.setColour (enabled ? resolved.colour : resolved.colour.withAlpha (0.4f));
        g.fillEllipse (p.x - (hot ? 4.5f : 3.0f), p.y - (hot ? 4.5f : 3.0f),
                       hot ? 9.0f : 6.0f, hot ? 9.0f : 6.0f);

        if (hot)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f, 1.2f);
        }
    }

    const bool hotResize = hoveredRow == index && hoveredPart == 5;

    g.setColour (hotResize ? resolved.colour.withAlpha (0.7f) : kOutline.withAlpha (0.55f));
    g.drawHorizontalLine ((int) row.getBottom() - 1, row.getX(), row.getRight());
}

//==============================================================================
void AutomationWindow::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    const bool rightClick = e.mods.isPopupMenu();

    clampScroll();

    //-- Row resize -----------------------------------------------------------
    // Phase 12. Checked before the list and the canvas, because the grab band
    // runs the full width: what the user grabs is the row's edge, not "the
    // list's edge" or "the canvas's edge".
    for (int i = 0; i < automation.getNumTracks(); ++i)
    {
        if (getResizeBounds (i).contains (pos))
        {
            selectTrack (i);
            draggingResize = true;
            resizeRowIndex = i;
            resizeStartY   = pos.y;
            resizeStartH   = getTrackHeight (i);
            return;
        }
    }

    //-- Track list -----------------------------------------------------------
    if (getListArea().contains (pos))
    {
        const int index = rowIndexAtY ((float) pos.y);

        if (index >= 0)
        {
            if (getDeleteBounds (index).expanded (ui::px (2), ui::px (2)).contains (pos))
            {
                deleteTrack (index);
                return;
            }

            if (getEnableBounds (index).expanded (3, 3).contains (pos))
            {
                toggleTrackEnabled (index);
                return;
            }

            selectTrack (index);
            draggingRow = true;
            dragRowIndex = index;
            dragStartY = pos.y;
        }

        return;
    }

    //-- Canvas ---------------------------------------------------------------
    if (getRowsArea().contains (pos))
    {
        const int index = rowIndexAtY ((float) pos.y);

        if (index < 0)
            return;

        selectTrack (index);

        auto resolved = resolveTrackAt (index);

        if (resolved.lane == nullptr)
            return;

        const int pointIndex = pointIndexAt (index, e.position);

        if (rightClick)
        {
            if (pointIndex >= 0)
            {
                resolved.lane->removePoint (pointIndex);
                repaint();

                if (onChanged != nullptr)
                    onChanged();
            }

            return;
        }

        const auto row = getCanvasRowBounds (index);

        if (pointIndex >= 0)
        {
            dragPointRow = index;
            dragPointIndex = pointIndex;
            return;
        }

        const double beat = juce::jlimit (0.0, lengthBeats, snappedBeatAtX (e.position.x));
        const float  value = valueAtY (row, e.position.y);
        const int inserted = resolved.lane->addPoint (beat, value, lengthBeats);

        if (inserted >= 0)
        {
            dragPointRow = index;
            dragPointIndex = inserted;
            resolved.lane->setEnabled (true);
            repaint();

            if (onChanged != nullptr)
                onChanged();
        }
    }
}

void AutomationWindow::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingResize && resizeRowIndex >= 0)
    {
        setTrackHeight (resizeRowIndex, resizeStartH + (e.getPosition().y - resizeStartY));
        return;
    }

    if (draggingRow && dragRowIndex >= 0)
    {
        const int dy = e.getPosition().y - dragStartY;

        // The threshold follows the row's own height: a row stretched to 200 px
        // should not swap places after a 26 px nudge.
        if (std::abs (dy) >= juce::jmax (12, getTrackHeight (dragRowIndex) * 6 / 10))
        {
            const auto& tracks = automation.getTracks();

            if (juce::isPositiveAndBelow (dragRowIndex, (int) tracks.size()))
            {
                const int step = dy > 0 ? 1 : -1;
                const int travel = getTrackHeight (dragRowIndex + step);

                if (automation.moveTrack (tracks[(size_t) dragRowIndex].id, step))
                {
                    dragRowIndex += step;
                    dragStartY += step * travel;
                }
            }
        }

        return;
    }

    if (dragPointRow < 0 || dragPointIndex < 0)
        return;

    const int index = rowIndexAtY (e.position.y);

    // Dragging past the row edge moves the point to the neighbouring track,
    // which is how a curve drawn against the wrong parameter gets fixed.
    if (index >= 0 && index != dragPointRow)
    {
        auto from = resolveTrackAt (dragPointRow);
        auto to   = resolveTrackAt (index);

        if (from.lane != nullptr && to.lane != nullptr)
        {
            const auto& points = from.lane->getPoints();

            if (juce::isPositiveAndBelow (dragPointIndex, (int) points.size()))
            {
                const auto moved = points[(size_t) dragPointIndex];
                from.lane->removePoint (dragPointIndex);
                dragPointIndex = juce::jmax (0, to.lane->addPoint (moved.beat, moved.value, lengthBeats));
                dragPointRow = index;
            }
        }
    }

    auto resolved = resolveTrackAt (dragPointRow);

    if (resolved.lane == nullptr)
        return;

    const auto row = getCanvasRowBounds (dragPointRow);
    const double beat = juce::jlimit (0.0, lengthBeats, snappedBeatAtX (e.position.x));
    const float  value = valueAtY (row, e.position.y);

    dragPointIndex = resolved.lane->movePoint (dragPointIndex, beat, value, lengthBeats);
    repaint();

    if (onChanged != nullptr)
        onChanged();
}

void AutomationWindow::mouseUp (const juce::MouseEvent&)
{
    draggingRow = false;
    dragRowIndex = -1;
    draggingResize = false;
    resizeRowIndex = -1;
    dragPointRow = -1;
    dragPointIndex = -1;
    repaint();
}

void AutomationWindow::mouseMove (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    int newRow = -1;
    int newPart = 0;
    int newPoint = -1;

    // Row edges first — the same order mouseDown uses.
    for (int i = 0; i < automation.getNumTracks(); ++i)
    {
        if (getResizeBounds (i).contains (pos))
        {
            newRow = i;
            newPart = 5;
            break;
        }
    }

    if (newPart == 5)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }
    else if (getListArea().contains (pos))
    {
        newRow = rowIndexAtY ((float) pos.y);

        if (newRow >= 0)
        {
            if (getDeleteBounds (newRow).expanded (ui::px (2), ui::px (2)).contains (pos)) newPart = 2;
            else if (getEnableBounds (newRow).expanded (3, 3).contains (pos)) newPart = 3;
            else                                              newPart = 1;
        }

        setMouseCursor (newPart == 0 ? juce::MouseCursor::NormalCursor
                                     : juce::MouseCursor::PointingHandCursor);
    }
    else if (getRowsArea().contains (pos))
    {
        newRow = rowIndexAtY ((float) pos.y);

        if (newRow >= 0)
        {
            newPoint = pointIndexAt (newRow, e.position);
            newPart = newPoint >= 0 ? 4 : 1;
        }

        setMouseCursor (newPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                      : juce::MouseCursor::NormalCursor);
    }
    else
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    if (newRow != hoveredRow || newPart != hoveredPart || newPoint != hoveredPoint)
    {
        hoveredRow = newRow;
        hoveredPart = newPart;
        hoveredPoint = newPoint;
        repaint();
    }
}

void AutomationWindow::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow >= 0)
    {
        hoveredRow = -1;
        hoveredPart = 0;
        hoveredPoint = -1;
        repaint();
    }
}

void AutomationWindow::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scrollY -= juce::roundToInt (wheel.deltaY * 120.0f);
    clampScroll();
    repaint();
}

void AutomationWindow::resized()
{
    clampScroll();
}

} // namespace atomic

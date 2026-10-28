#include "GroupPanel.h"

#include "Model/Routing.h"
#include "UI/CardLook.h"
#include "UI/MixerLook.h"
#include "Util/SharedRules.h"
#include "Util/Text.h"
#include "Util/Theme.h"
#include "Util/ValueMenu.h"

namespace atomic
{

namespace
{
    const juce::Colour& kPanelBackground = theme::surface1;
    const juce::Colour& kDivider        = theme::hairline;
    const juce::Colour kRowPlate        = juce::Colours::white.withAlpha (0.031f);
    const juce::Colour kRowHover        = juce::Colours::white.withAlpha (0.055f);

    /** Phase 35: the mixer's mute amber, repeated rather than shared. The two
        panels have no common header to put it in, and inventing one for a
        single colour would be a worse dependency than a line of duplication. */
    const juce::Colour& kMuteColour     = theme::warn;      // P48 11.1：静音统一黄色

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    juce::Font plainFont (float height)
    {
        return juce::Font (juce::FontOptions (atomic::ui::sp (height)));
    }
}

//==============================================================================
GroupPanel::GroupPanel (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    setOpaque (true);
    stack.addListener (this);
    rack.addListener (this);
}

GroupPanel::~GroupPanel()
{
    stack.removeListener (this);
    rack.removeListener (this);
}

//==============================================================================
std::vector<GroupPanel::RowRef> GroupPanel::getRows() const
{
    std::vector<RowRef> rows;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        for (const auto& group : layer->getGroups())
            rows.push_back ({ i, group.getId() });
    }

    return rows;
}

int GroupPanel::getNumRows() const
{
    return (int) getRows().size();
}

NoteGroup* GroupPanel::groupFor (const RowRef& ref) const
{
    auto& self = *const_cast<GroupPanel*> (this);

    if (auto* layer = self.stack.getLayer (ref.layerIndex))
        return layer->findGroup (ref.groupId);

    return nullptr;
}

//==============================================================================
bool GroupPanel::isGroupFocused (int layerIndex, int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || focusSlotId < 0)
        return false;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId && routing::resolve (*layer, note, rack).slotId == focusSlotId)
            return true;

    return false;
}

void GroupPanel::setFocusSlotId (int slotId)
{
    if (focusSlotId == slotId)
        return;

    focusSlotId = slotId;
    repaint();
}

void GroupPanel::setSelectedGroups (const std::vector<RowRef>& rows)
{
    // Cheap equality first: onSelectionChanged fires on every note click, and a
    // repaint per click on a panel that did not change is a flicker waiting to
    // be reported as a bug.
    bool same = rows.size() == selectedRows.size();

    for (size_t i = 0; same && i < rows.size(); ++i)
        same = rows[i].layerIndex == selectedRows[i].layerIndex
                 && rows[i].groupId == selectedRows[i].groupId;

    if (same)
        return;

    selectedRows = rows;
    repaint();
}

bool GroupPanel::isRowSelected (int layerIndex, int groupId) const
{
    for (const auto& ref : selectedRows)
        if (ref.layerIndex == layerIndex && ref.groupId == groupId)
            return true;

    return false;
}

//==============================================================================
int GroupPanel::getContentHeight() const
{
    int total = 0;

    for (const auto& ref : getRows())
    {
        const auto* group = groupFor (ref);
        total += (group != nullptr && group->isCollapsed()) ? collapsedHeight : rowHeight;
    }

    return total;
}

int GroupPanel::getViewHeight() const
{
    return juce::jmax (0, getHeight() - headerHeight);
}

juce::Rectangle<int> GroupPanel::getViewArea() const
{
    return { 0, headerHeight, getWidth(), getViewHeight() };
}

juce::Rectangle<int> GroupPanel::getRowBounds (int rowIndex) const
{
    const auto rows = getRows();

    int y = headerHeight - (int) scrollY;

    for (int i = 0; i < rowIndex && i < (int) rows.size(); ++i)
    {
        const auto* group = groupFor (rows[(size_t) i]);
        y += (group != nullptr && group->isCollapsed()) ? collapsedHeight : rowHeight;
    }

    int height = 0;

    if (juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
    {
        const auto* group = groupFor (rows[(size_t) rowIndex]);
        height = (group != nullptr && group->isCollapsed()) ? collapsedHeight : rowHeight;
    }

    return { 0, y, getWidth(), height };
}

juce::Rectangle<int> GroupPanel::getCollapseBounds (int rowIndex) const
{
    const auto row = getRowBounds (rowIndex);
    return { ui::px (8), row.getY() + ui::px (6), ui::px (18), ui::px (18) };
}

juce::Rectangle<int> GroupPanel::getVolumeBounds (int rowIndex) const
{
    const auto row = getRowBounds (rowIndex);

    return { ui::px (46), row.getY() + ui::px (30),
             juce::jmax (ui::px (20), getWidth() - ui::px (46) - ui::px (58)), ui::px (7) };
}

juce::Rectangle<int> GroupPanel::getPanBounds (int rowIndex) const
{
    const auto row = getRowBounds (rowIndex);

    return { ui::px (46), row.getY() + ui::px (50),
             juce::jmax (ui::px (20), getWidth() - ui::px (46) - ui::px (58)), ui::px (7) };
}

juce::Rectangle<int> GroupPanel::getSendBounds (int rowIndex) const
{
    const auto row = getRowBounds (rowIndex);

    return { ui::px (12), row.getY() + ui::px (68), getWidth() - ui::px (24), ui::px (18) };
}

juce::Rectangle<int> GroupPanel::getMuteBounds (int rowIndex) const
{
    // On the title line, at the far right: the one place a row has spare width,
    // and the same corner the mixer's strips put it in. The layer name that used
    // to live there is trimmed to make room rather than moved — see drawRow.
    const auto row = getRowBounds (rowIndex);

    return { getWidth() - ui::px (30), row.getY() + ui::px (4), ui::px (22), ui::px (16) };
}

int GroupPanel::rowAt (juce::Point<int> position, Part* partOut) const
{
    if (partOut != nullptr)
        *partOut = Part::none;

    if (! getViewArea().contains (position))
        return -1;

    const auto rows = getRows();

    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto bounds = getRowBounds (i);

        if (! bounds.contains (position))
            continue;

        if (partOut != nullptr)
        {
            const auto* group = groupFor (rows[(size_t) i]);
            const bool collapsed = group != nullptr && group->isCollapsed();

            if (getCollapseBounds (i).contains (position))
                *partOut = Part::collapse;
            else if (getMuteBounds (i).contains (position))
                *partOut = Part::mute;          // Phase 35, before the body
            else if (collapsed)
                *partOut = Part::body;
            else if (getVolumeBounds (i).expanded (0, ui::px (6)).contains (position))
                *partOut = Part::volume;
            else if (getPanBounds (i).expanded (0, ui::px (6)).contains (position))
                *partOut = Part::pan;
            else if (getSendBounds (i).contains (position))
                *partOut = Part::send;
            else
                *partOut = Part::body;
        }

        return i;
    }

    return -1;
}

//==============================================================================
void GroupPanel::setScrollY (int pixels)
{
    scrollY = (float) pixels;
    clampScroll();
    repaint();
}

void GroupPanel::clampScroll()
{
    const int excess = juce::jmax (0, getContentHeight() - getViewHeight());
    scrollY = (float) juce::jlimit (0, excess, (int) scrollY);
}

//==============================================================================
void GroupPanel::drawRow (juce::Graphics& g, int rowIndex)
{
    const auto rows = getRows();

    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;

    const auto ref = rows[(size_t) rowIndex];
    auto* group = groupFor (ref);

    if (group == nullptr)
        return;

    const auto* layer = stack.getLayer (ref.layerIndex);
    const auto  bounds = getRowBounds (rowIndex);
    const bool  collapsed = group->isCollapsed();
    const bool  hot = rowIndex == hoveredRow;
    const bool  focused = isGroupFocused (ref.layerIndex, ref.groupId);
    const bool  selected = isRowSelected (ref.layerIndex, ref.groupId);

    const auto accent = group->getColour();

    if (hot)
    {
        g.setColour (kRowHover);
        g.fillRect (bounds);
    }
    else
    {
        g.setColour (kRowPlate);
        g.fillRect (bounds.reduced (ui::px (4), ui::px (2)));
    }

    //-- Phase 59/60: a row whose group is selected in the roll ---------------
    // The roll owns the selection; this is its mirror, so "the group I am
    // selecting in the piano roll" is findable in a list of twenty.
    //
    // Phase 60: the mark is the rack's selection ring, not a tint of the group
    // colour. The user asked for the group frame and the sampler / synth frames
    // to look the same, and one function draws all three (cardlook::). A tinted
    // plate read as "this row is highlighted for some reason"; the ring reads as
    // "this is the selected object", which is what it is.
    if (selected)
        cardlook::drawSelectionRing (g, bounds.reduced (ui::px (4), ui::px (2)).toFloat());

    // A focused group gets a left edge in the synth's colour, matching what the
    // layer list does for a sheet that feeds the spotlighted synth.
    if (focused)
    {
        g.setColour (accent.withAlpha (0.85f));
        g.fillRect (bounds.getX(), bounds.getY() + ui::px (4), ui::px (2), bounds.getHeight() - ui::px (8));
    }

    //-- Line 1: caret, colour chip, name, layer ------------------------------
    mixerlook::drawCaret (g, getCollapseBounds (rowIndex).toFloat(), ! collapsed,
                          juce::Colours::white.withAlpha (hot ? 0.75f : 0.42f));

    g.setColour (accent.withAlpha (0.92f));
    g.fillRoundedRectangle (juce::Rectangle<float> (ui::pf (30), (float) bounds.getY() + ui::pf (8),
                                                    ui::pf (10), ui::pf (10)), 3.0f);

    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (boldFont (11.5f));
    g.drawText (group->getName(),
                juce::Rectangle<int> (ui::px (46), bounds.getY() + ui::px (4),
                                      getWidth() - ui::px (46 + 60), ui::px (18)),
                juce::Justification::centredLeft, false);

    g.setColour (juce::Colours::white.withAlpha (0.26f));
    g.setFont (plainFont (9.0f));
    g.drawText (layer != nullptr ? layer->getName() : juce::String(),
                juce::Rectangle<int> (0, bounds.getY() + ui::px (5),
                                      getWidth() - ui::px (34), ui::px (16)),
                juce::Justification::centredRight, true);

    //-- Phase 35: mute -------------------------------------------------------
    // The same chip the mixer's strips have (M, amber when on), because a mute
    // is a mute wherever it appears. It stays on the title line in both the
    // expanded and the folded state — a folded group is exactly the row you
    // cannot otherwise tell is silent.
    {
        const auto chip = getMuteBounds (rowIndex).toFloat();
        const bool on   = group->isMuted();
        const bool chipHot = hot && hoveredPart == Part::mute;

        g.setColour (on ? kMuteColour
                        : juce::Colours::white.withAlpha (chipHot ? 0.18f : 0.07f));
        g.fillRoundedRectangle (chip, ui::pf (2.5f));

        g.setColour (on ? juce::Colours::black.withAlpha (0.8f)
                        : juce::Colours::white.withAlpha (chipHot ? 0.72f : 0.42f));
        g.setFont (boldFont (9.0f));
        g.drawText ("M", chip, juce::Justification::centred, false);
    }

    if (collapsed)
    {
        // Phase 59: a folded row is its title and nothing else. The dB / pan
        // readout used to be drawn here into the SAME rectangle the name uses,
        // so every folded group showed a ghost of "0.0 dB · C" behind its name
        // (the user: "折叠之后会在组名背后显示不相关的文字"). Folding means one
        // line, and that line is the title; the fader and the mount come back
        // with the row.
        return;
    }

    //-- Line 2: volume -------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.34f));
    g.setFont (plainFont (9.0f));
    g.drawText (utf8 ("音量"), juce::Rectangle<int> (ui::px (12), bounds.getY() + ui::px (27),
                                                     ui::px (30), ui::px (12)),
                juce::Justification::centredLeft, false);

    mixerlook::drawFader (g, getVolumeBounds (rowIndex).toFloat(),
                          mixerlook::normFromDb (group->getLevelDb()), accent,
                          hot && hoveredPart == Part::volume);

    g.setColour (juce::Colours::white.withAlpha (0.48f));
    g.setFont (plainFont (9.0f));
    g.drawText (mixerlook::dbText (group->getLevelDb()),
                juce::Rectangle<int> (0, bounds.getY() + ui::px (27), getWidth() - ui::px (12), ui::px (12)),
                juce::Justification::centredRight, false);

    //-- Line 3: pan ----------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.34f));
    g.setFont (plainFont (9.0f));
    g.drawText (utf8 ("声相"), juce::Rectangle<int> (ui::px (12), bounds.getY() + ui::px (47),
                                                     ui::px (30), ui::px (12)),
                juce::Justification::centredLeft, false);

    mixerlook::drawPan (g, getPanBounds (rowIndex).toFloat(), group->getPan(), accent,
                        hot && hoveredPart == Part::pan);

    g.setColour (juce::Colours::white.withAlpha (0.48f));
    g.setFont (plainFont (9.0f));
    g.drawText (mixerlook::panText (group->getPan()),
                juce::Rectangle<int> (0, bounds.getY() + ui::px (47), getWidth() - ui::px (12), ui::px (12)),
                juce::Justification::centredRight, false);

    //-- Line 4: the mount ----------------------------------------------------
    {
        const auto pill = getSendBounds (rowIndex).toFloat();
        const int  slotId = group->getOutputSlotId();
        const bool mounted = rack.findSlot (slotId) != nullptr;
        const auto slotColour = mounted ? rack.getSlotColour (slotId) : juce::Colours::grey;

        g.setColour (mounted ? slotColour.withAlpha (0.16f) : juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (pill, ui::pf (4));

        g.setColour (mounted ? slotColour.withAlpha (0.55f) : juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (pill, ui::pf (4), 1.0f);

        if (mounted)
        {
            g.setColour (slotColour);
            g.fillEllipse (juce::Rectangle<float> (ui::pf (5), ui::pf (5))
                             .withCentre ({ pill.getX() + ui::pf (9), pill.getCentreY() }));
        }

        g.setColour (mounted ? juce::Colours::white.withAlpha (0.86f)
                             : juce::Colours::white.withAlpha (0.42f));
        g.setFont (plainFont (9.5f));
        g.drawText (mounted ? (utf8 ("→ ") + rack.getSlotName (slotId))
                            : utf8 ("→ 跟随图层输出"),
                    pill.withTrimmedLeft (ui::pf (18)).withTrimmedRight (ui::pf (10)),
                    juce::Justification::centredLeft, true);
    }
}

void GroupPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanelBackground);

    //-- Header ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (boldFont (12.5f));
    g.drawText (utf8 ("编组"), ui::px (14), ui::px (8), ui::px (100), ui::px (18),
                juce::Justification::centredLeft, false);

    g.setColour (juce::Colours::white.withAlpha (0.26f));
    g.setFont (plainFont (9.5f));
    g.drawText ("GROUPS" + separatorDot() + juce::String (getNumRows()),
                ui::px (14), ui::px (25), ui::px (160), ui::px (14),
                juce::Justification::centredLeft, false);

    g.setColour (kDivider);
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    //-- Rows -----------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (getViewArea());

        for (int i = 0; i < getNumRows(); ++i)
        {
            if (! getRowBounds (i).intersects (getViewArea()))
                continue;

            drawRow (g, i);
        }
    }

    if (getNumRows() == 0)
    {
        g.setColour (juce::Colours::white.withAlpha (0.26f));
        g.setFont (plainFont (10.0f));
        g.drawFittedText (utf8 ("还没有编组。\n在钢琴窗选中音符或采样按 G，\n编组就会出现在这里。"),
                          getViewArea().reduced (ui::px (16), ui::px (12)),
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

void GroupPanel::resized()
{
    clampScroll();
}

//==============================================================================
void GroupPanel::setLevelFromMouse (NoteGroup& group, juce::Rectangle<int> track, float x)
{
    const float norm = juce::jlimit (0.0f, 1.0f,
                                     (x - (float) track.getX()) / juce::jmax (1.0f, (float) track.getWidth()));
    group.setLevelDb (mixerlook::dbFromNorm (norm));
    stack.touchLayers();
}

void GroupPanel::setPanFromMouse (NoteGroup& group, juce::Rectangle<int> track, float x)
{
    const float norm = juce::jlimit (0.0f, 1.0f,
                                     (x - (float) track.getX()) / juce::jmax (1.0f, (float) track.getWidth()));
    group.setPan (norm * 2.0f - 1.0f);
    stack.touchLayers();
}

void GroupPanel::toggleMute (int rowIndex)
{
    const auto rows = getRows();

    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;

    auto* group = groupFor (rows[(size_t) rowIndex]);

    if (group == nullptr)
        return;

    group->setMuted (! group->isMuted());

    // Same notification the fader uses, for the same reason: mute is frozen into
    // the schedule, so the engine has to republish or the mix keeps playing a
    // group the model already calls silent.
    stack.touchLayers();
    repaint();
}

void GroupPanel::mouseDown (const juce::MouseEvent& e)
{
    Part part = Part::none;
    const int row = rowAt (e.getPosition(), &part);

    dragRow  = row;
    dragPart = part;
    dragMayStart    = false;    // every press re-decides; nothing is inherited
    selectOnRelease = false;

    if (row < 0)
        return;

    const auto rows = getRows();

    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto ref = rows[(size_t) row];
    auto* group = groupFor (ref);

    if (group == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        // Phase 20: the group's two trims answer a right click with 复位, like
        // every other numeric control in the app. Anything else on the row is
        // still the group menu.
        if (part == Part::volume || part == Part::pan)
        {
            const bool onVolume  = (part == Part::volume);
            const auto screenPos = e.getScreenPosition();
            const int  layerIndex = ref.layerIndex;
            const int  groupId2   = ref.groupId;

            ui::showValueMenu (this, screenPos,
                               group->getName() + (onVolume ? utf8 (" · 组音量")
                                                            : utf8 (" · 组声相")),
                               onVolume ? juce::String (group->getLevelDb(), 1) + " dB"
                                        : mixerlook::panText (group->getPan()),
                               // Re-resolved inside the callback, not captured:
                               // a menu can outlive the group it was opened on.
                               [this, layerIndex, groupId2, onVolume]
                               {
                                   RowRef r;
                                   r.layerIndex = layerIndex;
                                   r.groupId    = groupId2;

                                   if (auto* g = groupFor (r))
                                   {
                                       if (onVolume)
                                           g->setLevelDb (0.0f);
                                       else
                                           g->setPan (0.0f);

                                       stack.touchLayers();
                                   }
                               },
                               [] (juce::PopupMenu& m)
                               {
                                   m.addItem (10, utf8 ("编组菜单…"));
                               },
                               [this, layerIndex, groupId2, screenPos] (int result)
                               {
                                   if (result == 10 && onGroupMenuRequested)
                                       onGroupMenuRequested (layerIndex, groupId2, screenPos);
                               });

            dragRow = -1;
            return;
        }

        if (onGroupMenuRequested)
            onGroupMenuRequested (ref.layerIndex, ref.groupId, e.getScreenPosition());

        dragRow = -1;
        return;
    }

    switch (part)
    {
        case Part::collapse:
            // The chevron is a control, not a handle: it folds on the spot and
            // never starts a drag, because a 18 px box is not what you grab to
            // move a group.
            group->setCollapsed (! group->isCollapsed());
            clampScroll();
            stack.touchLayers();
            break;

        case Part::body:
            // Phase 58: arm "drag this group onto the roll". Phase 60: the click
            // that a drag is not — selecting the group — happens on release, for
            // the same reason. Folding is the chevron's job now (Part::collapse):
            // a row that folds when you click it cannot be selected by clicking.
            dragMayStart    = true;
            selectOnRelease = true;
            break;

        case Part::volume:
            setLevelFromMouse (*group, getVolumeBounds (row), (float) e.getPosition().x);
            break;

        case Part::pan:
            setPanFromMouse (*group, getPanBounds (row), (float) e.getPosition().x);
            break;

        case Part::send:
            if (onMountRequested)
                onMountRequested (ref.layerIndex, ref.groupId, e.getScreenPosition());
            break;

        case Part::mute:
            toggleMute (row);
            break;

        case Part::none:
        default:
            break;
    }
}

void GroupPanel::mouseDrag (const juce::MouseEvent& e)
{
    // Phase 58: a body drag LEAVES the panel — the roll answers it with another
    // appearance of the same group. Checked before the fader guard because this
    // gesture never set dragPart to a fader part.
    if (dragMayStart && dragRow >= 0 && e.getDistanceFromDragStart() > ui::px (4))
    {
        dragMayStart    = false;
        selectOnRelease = false;      // it was a drag, not a click

        const auto rows = getRows();

        if (juce::isPositiveAndBelow (dragRow, (int) rows.size()))
        {
            const auto ref = rows[(size_t) dragRow];

            // The payload is text, so a group deleted mid-drag simply fails to
            // resolve on the other end instead of danging a pointer.
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
                if (! container->isDragAndDropActive())
                    container->startDragging (groupDragDescription (ref.layerIndex, ref.groupId),
                                              this, juce::ScaledImage(), false, nullptr, &e.source);
        }

        return;
    }

    if (dragRow < 0 || (dragPart != Part::volume && dragPart != Part::pan))
        return;

    const auto rows = getRows();

    if (! juce::isPositiveAndBelow (dragRow, (int) rows.size()))
        return;

    auto* group = groupFor (rows[(size_t) dragRow]);

    if (group == nullptr)
        return;

    if (dragPart == Part::volume)
        setLevelFromMouse (*group, getVolumeBounds (dragRow), (float) e.getPosition().x);
    else
        setPanFromMouse (*group, getPanBounds (dragRow), (float) e.getPosition().x);
}

void GroupPanel::mouseUp (const juce::MouseEvent&)
{
    // Phase 58/60: a body press that never became a drag is a click, and a click
    // selects the row. Doing it here (not on press) is what lets the same press
    // also be the start of a drag.
    if (dragMayStart && selectOnRelease && dragRow >= 0)
        selectRow (dragRow);

    dragMayStart    = false;
    selectOnRelease = false;
    dragRow  = -1;
    dragPart = Part::none;
}

bool GroupPanel::selectRow (int rowIndex)
{
    const auto rows = getRows();

    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return false;

    const auto ref = rows[(size_t) rowIndex];

    // Optimistic, and deliberately so: a panel with no listener still shows the
    // click. A listening host answers with setSelectedGroups on the next
    // selection change and replaces this with the real selection (which is this
    // row, unless the group has gone).
    const std::vector<RowRef> justThis { ref };

    if (selectedRows.size() != 1
         || selectedRows.front().layerIndex != ref.layerIndex
         || selectedRows.front().groupId != ref.groupId)
    {
        selectedRows = justThis;
        repaint();
    }

    if (onGroupSelected != nullptr)
        onGroupSelected (ref.layerIndex, ref.groupId);

    return true;
}

void GroupPanel::mouseMove (const juce::MouseEvent& e)
{
    Part part = Part::none;
    const int row = rowAt (e.getPosition(), &part);

    if (row != hoveredRow || part != hoveredPart)
    {
        hoveredRow  = row;
        hoveredPart = part;
        setMouseCursor (part == Part::volume ? juce::MouseCursor::LeftRightResizeCursor
                       : part == Part::pan  ? juce::MouseCursor::LeftRightResizeCursor
                                            : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void GroupPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1 || hoveredPart != Part::none)
    {
        hoveredRow  = -1;
        hoveredPart = Part::none;
        repaint();
    }
}

void GroupPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (getContentHeight() <= getViewHeight())
        return;

    const float delta = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX)
                          * (float) getViewHeight() * 0.9f;

    if (delta != 0.0f)
        setScrollY ((int) (scrollY - delta));
}

//==============================================================================
void GroupPanel::layersChanged()
{
    clampScroll();
    repaint();
}

void GroupPanel::notesChanged()
{
    // Rows are derived from the model on every paint (see getRows), so the only
    // thing standing between "a group was just made" and "the row is on screen"
    // is this notification. Clamp first: grouping can also PRUNE an emptied
    // group, which makes the list shorter under a scrolled viewport.
    clampScroll();
    repaint();
}

} // namespace atomic

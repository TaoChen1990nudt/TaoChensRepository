#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Util/Metrics.h"
#include "Util/SharedRules.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"
#include "Util/Theme.h"

#include <limits>
#include <map>
#include <set>

namespace atomic
{
using namespace roll;



//==============================================================================
//  Phase 21: the inline group automation box
//==============================================================================
NoteGroup* PianoRollComponent::getExpandedGroup()
{
    if (expandedGroupId < 0)
        return nullptr;

    if (auto* layer = stack.getLayer (expandedGroupLayerIndex))
        return layer->findGroup (expandedGroupId);

    return nullptr;
}


const NoteGroup* PianoRollComponent::getExpandedGroup() const
{
    if (expandedGroupId < 0)
        return nullptr;

    if (const auto* layer = stack.getLayer (expandedGroupLayerIndex))
        return layer->findGroup (expandedGroupId);

    return nullptr;
}


int PianoRollComponent::getExpandedGroupTarget() const noexcept
{
    // One reading, no cache: the group's active curve IS the answer. A field
    // holding a copy would need syncing after every route change, every curve
    // switch, every undo — and one missed sync would paint a different lane
    // from the one the mouse edits.
    if (const auto* group = getExpandedGroup())
        return group->curveTarget();

    return 0;
}


void PianoRollComponent::openGroupAutomation (int layerIndex, int groupId, int target,
                                              int occurrence)
{
    // One floating editor at a time, same rule as the note box.
    closeNoteAutomation();
    closeSampleAutomation();

    expandedGroupLayerIndex = layerIndex;
    expandedGroupId         = groupId;
    expandedGroupOccurrence = juce::jmax (0, occurrence);
    dragGroupPoint          = -1;

    // Phase 82: `target` now names WHICH CURVE to open, looked up through the
    // routing table — a group may hold several shapes and two of them cannot
    // drive one parameter, so "the lane for level" is unambiguous while "the
    // third lane" is not.
    //
    // A group that was never given a plugin parameter cannot be edited on the
    // plugin lane: opening the box there would show a curve with nothing
    // behind it (the publisher skips a lane with `pluginParam < 0`). Falling
    // back to the level curve is the honest default — the menu is one click
    // away.
    if (auto* group = getExpandedGroup())
    {
        int wanted = juce::jlimit (-1, numGroupAutomationTargets - 1, target);

        if (wanted == (int) GroupAutomationTarget::plugin && ! group->drivesPluginParam())
            wanted = (int) GroupAutomationTarget::level;

        if (wanted >= 0)
        {
            if (const auto* slot = group->getCurveBank().findRoutedTo (wanted))
                group->setActiveCurveId (slot->id);
            else
                group->ensureAutomationFor ((GroupAutomationTarget) wanted);  // creates + activates
        }

        // Opening the box means "I am editing this curve", so the lane goes on
        // with it — a curve you can see and cannot hear is the single most
        // common way a DAW teaches the wrong thing about automation. The
        // notification only fires when the flag really flips: opening a box on
        // a live curve must not push an undo step for doing nothing.
        auto& lane = group->activeAutomation();

        if (! lane.isEnabled())
        {
            lane.setEnabled (true);
            stack.notifyNotesChanged();
        }
    }

    curveSelection = CurveSelection::groupLane;

    repaint();
}


void PianoRollComponent::closeGroupAutomation()
{
    if (expandedGroupId < 0)
        return;

    expandedGroupLayerIndex = -1;
    expandedGroupId         = -1;
    dragGroupPoint          = -1;
    hoveredGroupMenu        = 0;

    if (curveSelection == CurveSelection::groupLane)
        curveSelection = CurveSelection::none;

    repaint();
}


juce::Range<double> PianoRollComponent::getGroupBeatSpan (const Layer& layer,
                                                          const NoteGroup& group,
                                                          int occurrence) const
{
    bool   any = false;
    double lo  = 0.0;
    double hi  = 0.0;

    for (const auto& note : layer.getNotes())
    {
        if (note.groupId != group.getId())
            continue;

        // Phase 58: the curve box belongs to ONE appearance. A box spanning
        // every copy would put the same absolute-time curve over four
        // unrelated stretches of the song.
        if (occurrence >= 0 && note.groupOccurrence != occurrence)
            continue;

        if (! any)
        {
            lo  = note.startBeat;
            hi  = note.endBeat();
            any = true;
        }
        else
        {
            lo = juce::jmin (lo, note.startBeat);
            hi = juce::jmax (hi, note.endBeat());
        }
    }

    if (! any || hi <= lo + 1.0e-6)
        return {};

    return { lo, hi };
}


juce::Rectangle<float> PianoRollComponent::getGroupAutomationBoxBounds (int layerIndex,
                                                                      int groupId,
                                                                      int occurrence) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return {};

    const auto span = getGroupBeatSpan (*layer, *group, occurrence);

    if (span.isEmpty())
        return {};

    const auto content = getGroupContentBounds (*layer, *group, occurrence);

    if (content.isEmpty())
        return {};

    const auto grid = getGridArea();

    // Phase 22: the box IS the group's beat span — both edges, exactly. It
    // used to carry a 22 px floor and to be pushed back inside the grid, which
    // meant the frame you were editing started a few pixels left of the first
    // note and ended a few pixels right of the last one: the curve never lined
    // up with the thing it shapes. When the group is scrolled half out of view
    // the box now goes with it and the paint clip trims it, which is what
    // "aligned" has to mean if it is going to mean anything.
    const float left   = screenXForBeat (span.getStart());
    const float width  = juce::jmax (2.0f, screenXForBeat (span.getEnd()) - left);
    const float height = ui::pf (92);

    const float x = left;

    // Under the group: the label pill already took the space above it, and a
    // box that covered the pill would hide the handle that closes it.
    float y = content.getBottom() + ui::pf (3);

    if (y + height > grid.getBottom() - ui::pf (2))
        y = content.getY() - ui::pf (3) - height;      // no room below: flip above

    y = juce::jlimit (grid.getY(), juce::jmax (grid.getY(), grid.getBottom() - height), y);

    return { x, y, width, height };
}


juce::Rectangle<float> PianoRollComponent::getGroupCurveBounds() const
{
    const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId,
                                                  expandedGroupOccurrence);

    if (box.isEmpty())
        return {};

    // Phase 22: NO horizontal padding. The curve's first pixel is the frame's
    // first pixel, so the automation visibly starts where the group starts —
    // previously an inset that shrank with the box (up to 7 px a side) left a
    // blank margin at both ends that read as silence the curve never asked for.
    // The bottom is still trimmed: that row is the parameter cell.
    auto inner = box.reduced (0.0f, ui::pf (6));
    inner.removeFromBottom (ui::pf (30));
    return inner;
}


juce::Rectangle<float> PianoRollComponent::getGroupMenuArea() const
{
    const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId,
                                                  expandedGroupOccurrence);

    if (box.isEmpty())
        return {};

    // The menu row keeps its inset: it is a control, not a graph, and a button
    // that touches the frame looks like part of the frame.
    const float padX = juce::jlimit (ui::pf (2), ui::pf (7), box.getWidth() * 0.10f);

    return box.reduced (padX, ui::pf (6)).removeFromBottom (ui::pf (26));
}


juce::Rectangle<float> PianoRollComponent::getGroupCurveMenuArea() const
{
    const auto row = getGroupMenuArea();

    return row.isEmpty() ? row : splitCurveRow (row).first;
}


juce::Rectangle<float> PianoRollComponent::getGroupRouteMenuArea() const
{
    const auto row = getGroupMenuArea();

    return row.isEmpty() ? row : splitCurveRow (row).second;
}


juce::Rectangle<float> PianoRollComponent::getGroupCloseArea() const
{
    const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);

    if (box.isEmpty())
        return {};

    const float size = juce::jlimit (ui::pf (6), ui::pf (17), box.getWidth() - ui::pf (7));

    return { box.getRight() - size - ui::pf (3), box.getY() + ui::pf (3), size, size };
}


/** Screen y for a 0..1 automation value inside a curve area. */
static float groupValueToY (juce::Rectangle<float> inner, float value)
{
    return inner.getBottom() - juce::jlimit (0.0f, 1.0f, value) * inner.getHeight();
}


double PianoRollComponent::snapCurveBeat (double beat, juce::Range<double> span) const
{
    if (span.isEmpty())
        return beat;

    const double wanted = snapEnabled ? snapBeat (beat) : beat;

    // Clamped to the span, not pinned to its ends: a group may start off-grid
    // (its first note can sit anywhere), and in that case the nearest legal
    // grid step is the honest answer. The ends stay reachable by turning snap
    // off, which is the same deal a note gets.
    return juce::jlimit (span.getStart(), span.getEnd(), wanted);
}


void PianoRollComponent::drawGridInside (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (area.getWidth() < 2.0f || area.getHeight() < 2.0f)
        return;

    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (area.toNearestInt());

    // The piano roll's own grid: same step the roll itself is using right now,
    // same bar / beat / subdivision emphasis, same pixel rounding as
    // drawGridLines(). A box that drew decorative quarter-marks put its curve on
    // a grid that did not exist anywhere else on screen, which is the sort of
    // thing that makes an editor feel slightly wrong without anyone being able
    // to say why.
    std::vector<double> lines;
    collectGridLines (beatAtScreenX (area.getX()), beatAtScreenX (area.getRight()), lines);

    for (double beat : lines)
    {
        const float x = std::floor (screenXForBeat (beat)) + 0.5f;

        if (x < area.getX() - 1.0f || x > area.getRight() + 1.0f)
            continue;

        g.setColour (juce::Colours::white.withAlpha (gridLineAlpha (beat)));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
    }

    // Value guides are the box's own business: 1/4, 1/2 and 3/4 of the lane's
    // 0..1 range. They are NOT time and deliberately look different.
    g.setColour (juce::Colours::white.withAlpha (0.045f));

    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * (float) i / 4.0f),
                              area.getX(), area.getRight());

    g.setColour (juce::Colours::white.withAlpha (0.075f));
    g.drawHorizontalLine ((int) (area.getY() + area.getHeight() * 0.5f),
                          area.getX(), area.getRight());
}


int PianoRollComponent::findGroupCurvePointAt (juce::Point<float> position) const
{
    auto* group = getExpandedGroup();

    if (group == nullptr)
        return -1;

    const auto inner = getGroupCurveBounds();

    if (inner.isEmpty())
        return -1;

    // Beats, not 0..1: a group lane is absolute project time (see
    // AutomationLane), and this box only shows the slice the group covers.
    const auto box  = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);
    const auto* layer = stack.getLayer (expandedGroupLayerIndex);

    if (layer == nullptr)
        return -1;

    const auto span = getGroupBeatSpan (*layer, *group);

    // Phase 82: the OPEN curve, not "the lane for a target". The box edits one
    // curve at a time and that curve is whatever the left dropdown says.
    const auto& points = group->activeAutomation().getPoints();

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto& p = points[(size_t) i];

        if (p.beat < span.getStart() - 1.0e-9 || p.beat > span.getEnd() + 1.0e-9)
            continue;       // outside the slice on screen: not a target

        const float t = (float) ((p.beat - span.getStart()) / (span.getEnd() - span.getStart()));
        const juce::Point<float> at { inner.getX() + t * inner.getWidth(),
                                      groupValueToY (inner, p.value) };

        if (at.getDistanceFrom (position) <= 7.0f)
            return i;
    }

    juce::ignoreUnused (box);
    return -1;
}


void PianoRollComponent::showGroupTargetMenu (juce::Point<int> screenPosition)
{
    auto* group = getExpandedGroup();

    if (group == nullptr)
        return;

    const int layerIndex = expandedGroupLayerIndex;
    const int groupId    = expandedGroupId;
    const int current    = group->curveTarget();        // Phase 82: -1 = 空

    // Phase 24: the third target drives a parameter of the synth the group is
    // mounted on. Which parameters exist is a question only the plugin can
    // answer, so the list is built here, on the message thread, once.
    const int slotId = resolvedSlotForGroup (layerIndex, groupId);

    std::vector<int> paramIndices;

    for (const auto& info : pluginParamsForSlot (slotId))
        paramIndices.push_back (info.index);

    juce::PopupMenu menu;
    menu.addSectionHeader (group->getName() + utf8 (" · 这条曲线驱动什么"));

    menu.addItem (kRouteMenuNone, utf8 ("空") + "  ·  " + utf8 ("曲线留着，不驱动任何东西"),
                  true, current < 0);

    menu.addSeparator();

    menu.addItem (kRouteItemBase + (int) GroupAutomationTarget::level,
                  utf8 ("组音量") + "  ·  " + utf8 ("整组的推子，乘进每个成员"),
                  true, current == (int) GroupAutomationTarget::level);
    menu.addItem (kRouteItemBase + (int) GroupAutomationTarget::pan,
                  utf8 ("组声相") + "  ·  " + utf8 ("整组的声相，加到每个成员"),
                  true, current == (int) GroupAutomationTarget::pan);

    if (! paramIndices.empty())
    {
        addPluginParamSubmenu (menu, slotId, group->getPluginParam(), kPluginParamItemBase);
    }
    else
    {
        // Saying why beats a menu that silently lists two thirds of itself.
        menu.addItem (-1, group->getPluginParam() >= 0 ? utf8 ("音源参数：音源已不在，曲线保留")
                                                       : utf8 ("挂到外部音源后可选它的参数"),
                      false, current == (int) GroupAutomationTarget::plugin);
    }

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerIndex, groupId, paramIndices] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* layer = safe->stack.getLayer (layerIndex);

        if (layer == nullptr)
            return;

        auto* target = layer->findGroup (groupId);

        if (target == nullptr)
            return;

        auto& bank = target->getCurveBank();
        const int curveId = bank.getActiveId();

        if (result == kRouteMenuNone)
        {
            // Parked, not deleted: the shape stays on screen and stops
            // driving anything. That is what makes 空 different from "remove
            // this curve", which is the left dropdown's business.
            bank.route (curveId, -1);
        }
        else if (result >= kPluginParamItemBase)
        {
            const int which = result - kPluginParamItemBase;

            if (! juce::isPositiveAndBelow (which, (int) paramIndices.size()))
                return;

            // The group's plugin ADDRESS lives on the group (v10), so it is
            // written there; the bank only records that this curve is the one
            // pointing at it.
            target->setPluginParam (paramIndices[(size_t) which]);
            bank.route (curveId, (int) GroupAutomationTarget::plugin);

            auto& lane = target->activeAutomation();

            // A lane that has never been drawn starts flat at the centre:
            // 1.0 would slam a cutoff wide open the first time you look at it.
            if (! lane.isEnabled() && lane.getPoints().size() <= 2)
                lane.resetFlat (0.5f, safe->timelineBeats);

            lane.setEnabled (true);
        }
        else if (result >= kRouteItemBase && result < kRouteItemBase + 2)
        {
            bank.route (curveId, result - kRouteItemBase);
            target->activeAutomation().setEnabled (true);
        }
        else
        {
            return;
        }

        safe->stack.notifyNotesChanged();
        safe->repaint();
    });
}


void PianoRollComponent::showGroupCurveMenu (juce::Point<int> screenPosition)
{
    auto* group = getExpandedGroup();

    if (group == nullptr)
        return;

    const int layerIndex = expandedGroupLayerIndex;
    const int groupId    = expandedGroupId;
    const int numCurves  = group->getCurveBank().getNumCurves();

    std::vector<CurveMenuItem> items;

    for (const auto& slot : group->getCurveBank().getCurves())
        items.push_back ({ slot.id, slot.name, groupRouteLabel (slot) });

    juce::PopupMenu menu;
    addCurveItems (menu, items, group->getActiveCurveId());

    // Phase 82 kept the old verb, but it acts on the OPEN curve now instead of
    // on "the lane the box happens to show" — same words, one fewer concept.
    menu.addSeparator();
    menu.addItem (kCurveMenuReset, utf8 ("清空这条曲线…"));

    addCurveVerbs (menu, numCurves > 1);

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerIndex, groupId] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* layer = safe->stack.getLayer (layerIndex);

        if (layer == nullptr)
            return;

        auto* target = layer->findGroup (groupId);

        if (target == nullptr)
            return;

        auto& bank = target->getCurveBank();

        if (result >= kCurveItemBase && result < kCurveItemBase + bank.getNumCurves())
        {
            bank.setActiveId (bank.getCurves()[(size_t) (result - kCurveItemBase)].id);
        }
        else if (result == kCurveMenuNew)
        {
            const int added = bank.add (AutomationLane());      // flat and parked

            if (added < 0)
                return;                                         // 满员

            bank.setActiveId (added);
        }
        else if (result == kCurveMenuDelete)
        {
            bank.remove (bank.getActiveId());
        }
        else if (result == kCurveMenuReset)
        {
            // "Clear" means switched off AND flattened: an emptied curve that
            // still carried old breakpoints would come back the moment it was
            // switched on again. A parked curve has no neutral of its own, so
            // it falls back to level's.
            const int route = target->curveTarget();
            auto& lane = target->activeAutomation();

            lane.resetFlat (neutralCurveValue (route >= 0 ? route
                                                          : (int) GroupAutomationTarget::level,
                                               true),
                            safe->timelineBeats);
            lane.setEnabled (false);
        }
        else
        {
            return;
        }

        safe->stack.notifyNotesChanged();
        safe->repaint();
    });
}


bool PianoRollComponent::handleGroupAutomationMouseDown (juce::Point<float> pos,
                                                        juce::ModifierKeys mods)
{
    auto* group = getExpandedGroup();

    if (group == nullptr)
        return true;


    if (getGroupCloseArea().contains (pos))
    {
        closeGroupAutomation();
        return true;
    }

    if (getGroupCurveMenuArea().contains (pos))
    {
        showGroupCurveMenu (localPointToGlobal (pos.roundToInt()));
        return true;
    }

    if (getGroupRouteMenuArea().contains (pos))
    {
        showGroupTargetMenu (localPointToGlobal (pos.roundToInt()));
        return true;
    }

    const auto inner = getGroupCurveBounds();

    if (inner.isEmpty() || ! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    auto& lane = group->activeAutomation();

    auto* layer = stack.getLayer (expandedGroupLayerIndex);

    if (layer == nullptr)
        return true;

    const auto span = getGroupBeatSpan (*layer, *group);

    const int hit = findGroupCurvePointAt (pos);

    if (mods.isPopupMenu())
    {
        // Right-click removes a point; the lane always keeps two.
        if (hit >= 0)
        {
            lane.removePoint (hit);
            stack.notifyNotesChanged();

            if (onNoteEdited != nullptr)
                onNoteEdited();

            repaint();
        }

        return true;
    }

    const float t = juce::jlimit (0.0f, 1.0f, (pos.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
    const float v = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - pos.y) / juce::jmax (1.0f, inner.getHeight()));

    if (hit >= 0)
    {
        dragGroupPoint = hit;
    }
    else
    {
        // Phase 24: the point lands on the same grid a note would. The value is
        // left continuous — an automation curve is read as a shape, and forcing
        // it onto 1/16 steps of height would make every curve look like a
        // staircase.
        const int added = lane.addPoint (snapCurveBeat (span.getStart()
                                                          + (span.getEnd() - span.getStart()) * t,
                                                        span),
                                         v, timelineBeats);

        if (added < 0)
            return true;        // lane is full

        dragGroupPoint = added;

        if (onNoteEdited != nullptr)
            onNoteEdited();
    }

    dragMode = DragMode::groupAutomationPoint;
    repaint();
    return true;
}


void PianoRollComponent::dragGroupCurvePoint (juce::Point<float> position)
{
    if (dragGroupPoint < 0)
        return;

    auto* group = getExpandedGroup();
    auto* layer = stack.getLayer (expandedGroupLayerIndex);

    if (group == nullptr || layer == nullptr)
        return;

    const auto inner = getGroupCurveBounds();

    if (inner.isEmpty())
        return;

    const auto span = getGroupBeatSpan (*layer, *group);

    const float t = juce::jlimit (0.0f, 1.0f, (position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
    const float v = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - position.y) / juce::jmax (1.0f, inner.getHeight()));

    auto& lane = group->activeAutomation();

    // Phase 24: a dragged point obeys the snap toggle too, otherwise snapping
    // would only ever apply to the first point of a curve.
    dragGroupPoint = lane.movePoint (dragGroupPoint,
                                     snapCurveBeat (span.getStart()
                                                      + (span.getEnd() - span.getStart()) * t, span),
                                     v, timelineBeats);

    // Deliberately NOT notifyNotesChanged() here: that would push one undo step
    // per mouse-move. The note box works the same way, and mouseUp commits the
    // whole gesture as one step.
    if (onNoteEdited != nullptr)
        onNoteEdited();

    repaint();
}


void PianoRollComponent::drawGroupAutomationBox (juce::Graphics& g)
{
    auto* group = getExpandedGroup();
    auto* layer = stack.getLayer (expandedGroupLayerIndex);

    if (group == nullptr || layer == nullptr)
        return;

    const auto colour = group->getColour();
    const auto box    = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);
    const auto label  = getGroupLabelBounds (expandedGroupLayerIndex, expandedGroupId);

    if (box.isEmpty())
        return;

    //-- Panel ---------------------------------------------------------------
    // Phase 22: same selection signal as the note box.
    const bool selected = curveSelection == CurveSelection::groupLane;

    g.setColour (theme::bgApp.withAlpha (0.97f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (colour.withAlpha (selected ? 1.0f : 0.8f));
    g.drawRoundedRectangle (box, 5.0f, selected ? 2.2f : 1.2f);

    // A stem from the group's pill down to the box, so a box that had to flip
    // is still obviously attached to this group and not the one above it.
    if (! label.isEmpty())
    {
        g.setColour (colour.withAlpha (0.55f));
        g.drawLine (label.getCentreX(), label.getBottom(),
                    label.getCentreX(), box.getY(), 1.0f);
    }

    //-- Curve ---------------------------------------------------------------
    const auto inner = getGroupCurveBounds();


    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (inner, 3.0f);

    // Phase 24: the roll's grid, inside the box. There used to be three
    // decorative quarter-marks here, which lined up with nothing.
    drawGridInside (g, inner);

    const auto& lane = group->activeAutomation();
    const auto  span = getGroupBeatSpan (*layer, *group);

    if (! span.isEmpty())
    {
        // Sample the lane per pixel column instead of joining the breakpoints:
        // the lane is absolute project time and this box is a window onto one
        // slice of it, so a point outside the slice still shapes the edge of
        // what you see. Drawing the points directly would silently clip them.
        juce::Path curve;
        bool started = false;

        for (float x = inner.getX(); x <= inner.getRight() + 0.5f; x += 1.5f)
        {
            const double t    = (double) ((x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
            const double beat = span.getStart() + (span.getEnd() - span.getStart()) * t;
            const float  y    = groupValueToY (inner, lane.valueAtBeat (beat));

            if (! started)
            {
                curve.startNewSubPath (x, y);
                started = true;
            }
            else
            {
                curve.lineTo (x, y);
            }
        }

        if (started)
        {
            auto filled = curve;
            filled.lineTo (inner.getRight(), inner.getBottom());
            filled.lineTo (inner.getX(), inner.getBottom());
            filled.closeSubPath();

            const float alpha = lane.isEnabled() ? 1.0f : 0.45f;

            g.setColour (colour.withAlpha (0.18f * alpha));
            g.fillPath (filled);

            g.setColour (colour.brighter (0.4f).withAlpha (0.95f * alpha));
            g.strokePath (curve, juce::PathStrokeType (1.6f));
        }

        // Handles for the breakpoints that fall inside the window.
        const auto& points = lane.getPoints();

        for (int i = 0; i < (int) points.size(); ++i)
        {
            const auto& p = points[(size_t) i];

            if (p.beat < span.getStart() - 1.0e-9 || p.beat > span.getEnd() + 1.0e-9)
                continue;

            const float t = (float) ((p.beat - span.getStart()) / (span.getEnd() - span.getStart()));
            const juce::Point<float> at { inner.getX() + t * inner.getWidth(),
                                          groupValueToY (inner, p.value) };
            const bool  hot = (i == dragGroupPoint);

            g.setColour (hot ? juce::Colours::white : colour.brighter (0.6f));
            g.fillEllipse (at.x - (hot ? 3.2f : 2.4f), at.y - (hot ? 3.2f : 2.4f),
                           hot ? 6.4f : 4.8f, hot ? 6.4f : 4.8f);
        }
    }

    //-- Two dropdowns ------------------------------------------------------
    // Phase 82: which curve, and what it drives. Two cells, two menus, one row
    // — the group used to name only the target because there was exactly one
    // curve to name.
    const auto curveCell = getGroupCurveMenuArea();
    const auto routeCell = getGroupRouteMenuArea();
    const auto& active   = group->getCurveBank().getActive();

    const int   route    = group->curveTarget();

    drawMenuCell (g, curveCell,
                  active.name.isNotEmpty() ? active.name : utf8 ("曲线"),
                  colour, hoveredGroupMenu == 1, route < 0);

    if (! curveCellFitsText (curveCell))
    {
        const auto glyph = juce::Rectangle<float> (ui::pf (11), ui::pf (11))
                             .withCentre (curveCell.getCentre());

        drawCurveGlyph (g, glyph, juce::Colours::white.withAlpha (0.92f));
    }

    drawMenuCell (g, routeCell, groupTargetLabel (*group), colour,
                  hoveredGroupMenu == 2, route < 0);

    if (! curveCellFitsText (routeCell))
    {
        const auto glyph = juce::Rectangle<float> (ui::pf (11), ui::pf (11))
                             .withCentre (routeCell.getCentre());

        if (route == (int) GroupAutomationTarget::plugin)
            drawKnobGlyph (g, glyph, juce::Colours::white.withAlpha (0.92f));
        else if (route < 0)
            g.drawText (utf8 ("空"), routeCell, juce::Justification::centred, false);
        else
            drawModTargetIcon (g, route == (int) GroupAutomationTarget::pan ? NoteModTarget::pan
                                                                            : NoteModTarget::level,
                               glyph, juce::Colours::white.withAlpha (0.92f));
    }

    //-- Close ---------------------------------------------------------------
    g.setColour (juce::Colours::white.withAlpha (0.30f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (12.0f))));
    g.drawText ("x", getGroupCloseArea(), juce::Justification::centred, false);
}


//==============================================================================
//  Phase 22: the curve clipboard
//==============================================================================
std::vector<AutomationPoint> PianoRollComponent::readSelectedCurve (double& lengthBeats,
                                                                  int& target) const
{
    lengthBeats = 0.0;
    target      = 0;

    if (curveSelection == CurveSelection::noteEnvelope)
    {
        auto* note = getExpandedNote();

        if (note == nullptr)
            return {};

        // Normalised time becomes a beat offset. From here on the note box and
        // the group box speak the same language, which is the only reason a
        // note's curve can be pasted onto a group at all.
        std::vector<AutomationPoint> out;

        for (const auto& p : note->envelope().getPoints())
            out.push_back ({ (double) p.time * juce::jmax (gridDivision, note->lengthBeats),
                             p.value });

        lengthBeats = juce::jmax (gridDivision, note->lengthBeats);
        target      = (int) note->modTarget();

        return out;
    }

    if (curveSelection == CurveSelection::groupLane)
    {
        auto* group = getExpandedGroup();
        auto* layer = stack.getLayer (expandedGroupLayerIndex);

        if (group == nullptr || layer == nullptr)
            return {};

        const auto span = getGroupBeatSpan (*layer, *group);

        if (span.isEmpty())
            return {};

        // Only the slice the box shows: points outside it are not part of what
        // the user is looking at, and copying them would paste a curve longer
        // than the thing they copied.
        std::vector<AutomationPoint> out;

        for (const auto& p : group->activeAutomation().getPoints())
            if (p.beat >= span.getStart() - 1.0e-9 && p.beat <= span.getEnd() + 1.0e-9)
                out.push_back ({ p.beat - span.getStart(), p.value });

        lengthBeats = juce::jmax (1.0e-6, span.getLength());
        target      = group->curveTarget();      // -1 = 空, carried as -1

        return out;
    }

    return {};
}


bool PianoRollComponent::canCopyAutomationCurve() const
{
    if (curveSelection == CurveSelection::noteEnvelope)
        return getExpandedNote() != nullptr;

    if (curveSelection == CurveSelection::groupLane)
        return getExpandedGroup() != nullptr;

    return false;
}


bool PianoRollComponent::copyAutomationCurve()
{
    double length = 0.0;
    int    target = 0;

    auto points = readSelectedCurve (length, target);

    if (points.size() < 2)
    {
        flashStatus (utf8 ("没有可复制的曲线：先点开一个自动化框"));
        return false;
    }

    curveClip.points      = std::move (points);
    curveClip.lengthBeats = length;
    curveClip.target      = target;
    curveClip.fromGroup   = curveSelection == CurveSelection::groupLane;

    flashStatus (utf8 ("已复制曲线 · ") + juce::String (curveClip.points.size())
                   + utf8 (" 个点 · ") + juce::String (curveClip.lengthBeats, 2) + utf8 (" 拍"));
    repaint();
    return true;
}


bool PianoRollComponent::cutAutomationCurve()
{
    const bool copied = copyAutomationCurve();

    if (! copied)
        return false;

    if (curveSelection == CurveSelection::noteEnvelope)
    {
        if (auto* note = getExpandedNote())
        {
            const float flat = neutralCurveValue ((int) note->modTarget(), false);
            note->envelope().loadPoints ({ { 0.0f, flat }, { 1.0f, flat } });
            note->envelope().setEnabled (true);
        }
    }
    else if (curveSelection == CurveSelection::groupLane)
    {
        if (auto* group = getExpandedGroup())
        {
            auto& lane = group->activeAutomation();
            const int route = group->curveTarget();

            lane.resetFlat (neutralCurveValue (route >= 0 ? route
                                                          : (int) GroupAutomationTarget::level,
                                               true),
                            timelineBeats);
            lane.setEnabled (true);
        }
    }

    flashStatus (utf8 ("已剪切曲线，框内回到中性值"));
    stack.notifyNotesChanged();
    repaint();
    return true;
}


bool PianoRollComponent::canPasteAutomationCurve() const
{
    return hasCurveClipboard() && canCopyAutomationCurve();
}


juce::String PianoRollComponent::describeCurvePaste() const
{
    double targetLength = 0.0;

    if (curveSelection == CurveSelection::noteEnvelope)
    {
        if (auto* note = getExpandedNote())
            targetLength = juce::jmax (gridDivision, note->lengthBeats);
    }
    else if (curveSelection == CurveSelection::groupLane)
    {
        if (auto* group = getExpandedGroup())
            if (const auto* layer = stack.getLayer (expandedGroupLayerIndex))
                targetLength = getGroupBeatSpan (*layer, *group).getLength();
    }

    juce::String text;
    text << utf8 ("复制的曲线 ") << juce::String (curveClip.lengthBeats, 2) << utf8 (" 拍")
         << utf8 ("，目标 ") << juce::String (targetLength, 2) << utf8 (" 拍。");

    if (curveClip.lengthBeats > targetLength + 1.0e-6)
        text << utf8 ("\n\n不拉伸：曲线会被截断到目标长度。");
    else if (curveClip.lengthBeats < targetLength - 1.0e-6)
        text << utf8 ("\n\n不拉伸：曲线之后补 0，目标长出来的部分保持为 0。");
    else
        text << utf8 ("\n\n两者等长，拉伸与不拉伸结果相同。");

    return text;
}


bool PianoRollComponent::pasteAutomationCurve (bool stretch)
{
    if (! hasCurveClipboard())
        return false;

    const auto&  points      = curveClip.points;
    const double sourceLength = juce::jmax (1.0e-6, curveClip.lengthBeats);
    const int    target       = curveClip.target;

    if (curveSelection == CurveSelection::noteEnvelope)
    {
        auto* note = getExpandedNote();

        if (note == nullptr)
            return false;

        const double length = juce::jmax (gridDivision, note->lengthBeats);

        std::vector<EnvelopePoint> out;

        for (const auto& p : points)
        {
            const double beat = stretch ? p.beat * (length / sourceLength) : p.beat;

            if (beat > length + 1.0e-9)
                continue;               // a shorter target truncates the copy

            out.push_back ({ (float) juce::jlimit (0.0, 1.0, beat / length),
                             juce::jlimit (0.0f, 1.0f, p.value) });
        }

        if (out.size() < 2)
            return false;

        // Only a LONGER target gets the zero tail. A shorter one is truncated
        // and its last surviving point holds to the end — dropping to 0 exactly
        // at the target's end would be a discontinuity the copy never had.
        if (! stretch && length > sourceLength + 1.0e-6 && out.back().time < 0.9995f)
        {
            // Three points, not two: a vertical drop needs the old value held
            // to the end of the source and the new one at the very end of the
            // target, or the tail ramps down instead of falling.
            out.push_back ({ juce::jmin (0.9995f, out.back().time + 1.0e-5f), out.back().value });
            out.push_back ({ 1.0f, 0.0f });
        }

        note->envelope().loadPoints (std::move (out));
        note->envelope().setEnabled (true);
        note->setModTarget ((NoteModTarget) juce::jlimit (0, 2, target));
    }
    else if (curveSelection == CurveSelection::groupLane)
    {
        auto* group = getExpandedGroup();
        auto* layer = stack.getLayer (expandedGroupLayerIndex);

        if (group == nullptr || layer == nullptr)
            return false;

        const auto span = getGroupBeatSpan (*layer, *group);

        if (span.isEmpty())
            return false;

        const double length = juce::jmax (1.0e-6, span.getLength());

        std::vector<AutomationPoint> out;

        for (const auto& p : points)
        {
            const double beat = stretch ? p.beat * (length / sourceLength) : p.beat;

            if (beat > length + 1.0e-9)
                continue;

            out.push_back ({ span.getStart() + beat, juce::jlimit (0.0f, 1.0f, p.value) });
        }

        if (out.size() < 2)
            return false;

        // Zero tail only when the target outlives the copy (see the note box).
        if (! stretch && length > sourceLength + 1.0e-6
             && out.back().beat < span.getEnd() - 1.0e-6)
        {
            out.push_back ({ span.getEnd() - 1.0e-4, out.back().value });
            out.push_back ({ span.getEnd(), 0.0f });
        }

        // The pasted curve IS the lane: replacing it wholesale is the only
        // behaviour that makes "paste" mean the same thing for a group as it
        // does for a note, where the whole envelope is replaced.
        //
        // Phase 82: the destination is a CURVE, not a slot in a fixed array.
        // The clip's target only names one when it fits this box (level / pan);
        // a note's pitch curve has no group equivalent, so it lands on whatever
        // is open rather than being silently mislabelled as pan.
        const int route = (target >= 0 && target < 2) ? target : group->curveTarget();

        auto& lane = route >= 0 ? group->ensureAutomationFor ((GroupAutomationTarget) route)
                                : group->activeAutomation();

        lane.loadPoints (std::move (out));
        lane.setEnabled (true);

        // Show the curve that was just written, or the box would keep
        // displaying one the paste did not touch.
        if (route >= 0)
            if (const auto* slot = group->getCurveBank().findRoutedTo (route))
                group->setActiveCurveId (slot->id);
    }
    else
    {
        return false;
    }

    stack.notifyNotesChanged();
    flashStatus (stretch ? utf8 ("已拉伸粘贴曲线") : utf8 ("已按原长粘贴曲线"));

    if (onNoteEdited != nullptr)
        onNoteEdited();

    repaint();
    return true;
}


void PianoRollComponent::dropCurveSelection()
{
    if (curveSelection == CurveSelection::none)
        return;

    curveSelection = CurveSelection::none;
    repaint();
}
} // namespace atomic

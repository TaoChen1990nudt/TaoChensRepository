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
//-- Phase 49: the clip's own automation box ---------------------------------
//
// A clip's volume curve, edited in a floating box exactly as wide as the
// block — the note box's pattern (Phase 8) with the group box's floating
// placement (Phase 21). The old fade pair was a two-point special case of
// this curve; the box replaces the handles, and the model no longer keeps
// both ways to shape a block.

void PianoRollComponent::openSampleAutomation (int layerIndex, int sampleId)
{
    // One floating editor at a time — the rule the note box and the group box
    // already obey between themselves, extended to the third tenant.
    closeNoteAutomation();
    closeGroupAutomation();

    expandedSampleLayerIndex = layerIndex;
    expandedSampleId         = sampleId;
    dragSampleCurveIndex     = -1;
    hoveredSampleCurvePoint  = -1;
    hoveredSampleCurveMenu   = 0;

    // Phase 65: opening the box is the moment a bypassed curve comes back.
    // The switch that used to do this is gone, so a curve left disabled by an
    // older build would otherwise stay drawn as a dim ghost with no way back —
    // and "the shape I drew is not playing" is exactly the state this phase
    // removed the control to prevent.
    if (auto* layer = stack.getLayer (layerIndex))
        if (auto* sample = layer->findSample (sampleId))
            if (! sample->activeCurve().isEnabled())
                sample->activeCurve().setEnabled (true);

    repaint();
}


void PianoRollComponent::closeSampleAutomation()
{
    if (expandedSampleId < 0)
        return;

    expandedSampleId         = -1;
    expandedSampleLayerIndex = -1;
    dragSampleCurveIndex     = -1;
    hoveredSampleCurvePoint  = -1;
    hoveredSampleCurveMenu   = 0;

    repaint();
}


SampleEvent* PianoRollComponent::getExpandedSample()
{
    if (auto* layer = stack.getLayer (expandedSampleLayerIndex))
        return layer->findSample (expandedSampleId);

    return nullptr;
}


const SampleEvent* PianoRollComponent::getExpandedSample() const
{
    if (const auto* layer = stack.getLayer (expandedSampleLayerIndex))
        return layer->findSample (expandedSampleId);

    return nullptr;
}


juce::Rectangle<float> PianoRollComponent::getSampleAutomationHandleBounds (const SampleEvent& sample) const
{
    // The note handle's own geometry, transplanted onto the block: the resize
    // band first, a hairline of air, then the button — and hidden when the
    // block is too short to carry one without covering a trim edge. Only the
    // ACTIVE layer's blocks draw or answer it, which is why the lookup goes
    // through the active index and no layerIndex is passed.
    //
    // Phase 51: it sits at the TOP of the block now, not vertically centred —
    // same row the note's chevron lives on (top edge + 1), and the same
    // distance from the right edge (the note's own band + air + width
    // formula). "The arrow at the top right opens the curve" is one habit for
    // both kinds of object; centring the block's arrow made it a different
    // control on a different row.
    const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), sample.id);

    const float band  = ui::pf (kResizeHandleWidth);
    const float air   = ui::pf (kNoteHandleTailAir);
    const float width = ui::pf (kNoteHandleWidth);

    const float inset = band + air + width;
    const float minimumWidth = inset + band + ui::pf (kNoteHandleMinBody);

    if (bounds.isEmpty() || bounds.getWidth() < minimumWidth)
        return {};

    // A block can be far taller than a note: the button takes a fixed strip
    // and pins it to the top instead of stretching full height.
    const float height = juce::jmin (ui::pf (14),
                                     juce::jmax (ui::pf (8), bounds.getHeight() - ui::pf (2)));

    return { bounds.getRight() - inset,
             bounds.getY() + ui::pf (1),
             width, height };
}


juce::Rectangle<float> PianoRollComponent::getSampleAutomationBoxBounds (int layerIndex,
                                                                         int sampleId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return {};

    const auto rect = getSampleScreenBounds (layerIndex, sampleId);
    const auto grid = getGridArea();

    if (rect.isEmpty())
        return {};

    // Exactly as wide as the block, and now exactly ON the block: the old x was
    // clamped into the viewport, which silently slid the box right by however
    // much of the block was scrolled off to the left — and the curve inside it
    // slid with it. A box that is off-screen is a box the user scrolled away
    // from; pretending it is still at beat 0 is worse than letting it be clipped.
    const float width  = juce::jmax (rect.getWidth(), 22.0f);
    const float height = ui::pf (92);

    const float x = rect.getX();

    float y = rect.getBottom() + ui::pf (3);

    // No room underneath (block near the bottom of the view): flip above it.
    if (y + height > grid.getBottom() - ui::pf (2))
        y = rect.getY() - ui::pf (3) - height;

    y = juce::jlimit (grid.getY(), juce::jmax (grid.getY(), grid.getBottom() - height), y);

    return { x, y, width, height };
}


juce::Rectangle<float> PianoRollComponent::getSampleCurveArea (int layerIndex, int sampleId) const
{
    // Phase 50: horizontally this IS the block. `getSampleScreenBounds` is the
    // same mapping the waveform and the playhead use, so "t = 0.5" lands on the
    // block's midpoint by construction rather than by a padding constant that
    // happened to be 10% of whatever width the box ended up with.
    const auto box  = getSampleAutomationBoxBounds (layerIndex, sampleId);
    const auto rect = getSampleScreenBounds (layerIndex, sampleId);

    if (box.isEmpty() || rect.isEmpty())
        return {};

    const float top    = box.getY() + ui::pf (7);
    const float bottom = box.getBottom() - ui::pf (32);

    return { rect.getX(), top,
             juce::jmax (2.0f, rect.getWidth()),
             juce::jmax (4.0f, bottom - top) };
}


juce::Rectangle<float> PianoRollComponent::getSampleMenuArea (int layerIndex, int sampleId) const
{
    const auto  box  = getSampleAutomationBoxBounds (layerIndex, sampleId);
    const float padX = juce::jlimit (ui::pf (2), ui::pf (7), box.getWidth() * 0.10f);

    auto inner = box.reduced (padX, ui::pf (6));
    return inner.removeFromBottom (ui::pf (26));
}


juce::Rectangle<float> PianoRollComponent::getSampleTargetArea (int layerIndex, int sampleId) const
{
    // Phase 65: the whole row. The enable switch that used to sit at its right
    // end is gone (it was the only thing separating the label from "a control
    // that does nothing"), so naming the target gets the full width instead of
    // sharing it with a toggle nobody asked for.
    //
    // Phase 82: the row is two buttons now — this is their union, which is what
    // "somewhere on the row" has to mean for the swallow-it fallback below it.
    return getSampleMenuArea (layerIndex, sampleId);
}


juce::Rectangle<float> PianoRollComponent::getSampleCurveMenuArea (int layerIndex,
                                                                 int sampleId) const
{
    const auto row = getSampleMenuArea (layerIndex, sampleId);

    return row.isEmpty() ? row : splitCurveRow (row).first;
}


juce::Rectangle<float> PianoRollComponent::getSampleRouteMenuArea (int layerIndex,
                                                                 int sampleId) const
{
    const auto row = getSampleMenuArea (layerIndex, sampleId);

    return row.isEmpty() ? row : splitCurveRow (row).second;
}


juce::Rectangle<float> PianoRollComponent::getSampleCloseArea (int layerIndex, int sampleId) const
{
    const auto  box  = getSampleAutomationBoxBounds (layerIndex, sampleId);
    const float size = juce::jlimit (ui::pf (9), ui::pf (17), box.getWidth() - ui::pf (7));

    return { box.getRight() - size - ui::pf (3), box.getY() + ui::pf (3), size, size };
}


int PianoRollComponent::findSampleCurvePointAt (int layerIndex, int sampleId,
                                                juce::Point<float> position) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return -1;

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return -1;

    const auto inner = getSampleCurveArea (layerIndex, sampleId);
    const auto& points = sample->activeCurve().getPoints();

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const juce::Point<float> p { inner.getX() + points[(size_t) i].time * inner.getWidth(),
                                     inner.getBottom() - points[(size_t) i].value * inner.getHeight() };

        if (p.getDistanceFrom (position) <= 7.0f)
            return i;
    }

    return -1;
}


void PianoRollComponent::dragSampleCurvePoint (int layerIndex, int sampleId,
                                               juce::Point<float> position)
{
    if (dragSampleCurveIndex < 0)
        return;

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return;

    const auto inner = getSampleCurveArea (layerIndex, sampleId);

    if (inner.isEmpty())
        return;

    // Normalised across the block, exactly like a note's envelope: the drag
    // never needs to know the block's length, so a resize during the drag
    // cannot throw the point off the pointer.
    const float t = juce::jlimit (0.0f, 1.0f,
                                  (position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
    const float v = juce::jlimit (0.0f, 1.0f,
                                  (inner.getBottom() - position.y) / juce::jmax (1.0f, inner.getHeight()));

    sample->activeCurve().movePoint (dragSampleCurveIndex, t, v);

    // Phase 62: a clip's volume/pan curve is the group's shape, so it travels.
    syncShapeOfClip (sample->id);

    // Live sound, no undo: onNoteEdited republishes the schedule only; the
    // history step is written once, in mouseUp, like the group box's drag.
    if (onNoteEdited != nullptr)
        onNoteEdited();

    repaint();
}


bool PianoRollComponent::handleSampleAutomationMouseDown (juce::Point<float> pos,
                                                          juce::ModifierKeys mods,
                                                          int layerIndex, int sampleId)
{
    auto* layer = stack.getLayer (layerIndex);
    auto* sample = layer != nullptr ? layer->findSample (sampleId) : nullptr;

    if (sample == nullptr)
        return false;


    if (getSampleCloseArea (layerIndex, sampleId).contains (pos))
    {
        closeSampleAutomation();
        return true;
    }

    // Phase 82: the row is two buttons. Left = which curve, right = where it
    // goes. Phase 65's "the row is the target menu" survives as the RIGHT half;
    // the left half is new, and it is the half that makes a second curve
    // reachable at all.
    if (getSampleCurveMenuArea (layerIndex, sampleId).contains (pos))
    {
        showSampleCurveListMenu (layerIndex, sampleId, localPointToGlobal (pos.roundToInt()));
        return true;
    }

    if (getSampleRouteMenuArea (layerIndex, sampleId).contains (pos))
    {
        showSampleCurveMenu (layerIndex, sampleId, localPointToGlobal (pos.roundToInt()));
        return true;
    }

    if (getSampleMenuArea (layerIndex, sampleId).contains (pos))
        return true;        // inside the row but between the parts: swallow it

    const auto inner = getSampleCurveArea (layerIndex, sampleId);

    if (! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    const int hit = findSampleCurvePointAt (layerIndex, sampleId, pos);

    if (mods.isPopupMenu())
    {
        // Right-click removes a point; the curve always keeps two.
        if (hit >= 0)
        {
            sample->activeCurve().removePoint (hit);

            if (onNoteEdited != nullptr)
                onNoteEdited();

            repaint();
        }

        return true;
    }

    if (hit >= 0)
    {
        dragSampleCurveIndex = hit;
    }
    else
    {
        const float t = juce::jlimit (0.0f, 1.0f,
                                      (pos.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
        const float v = juce::jlimit (0.0f, 1.0f,
                                      (inner.getBottom() - pos.y) / juce::jmax (1.0f, inner.getHeight()));

        const int added = sample->activeCurve().addPoint (t, v);

        if (added < 0)
            return true;    // curve is full

        dragSampleCurveIndex = added;
    }

    // A curve you can see and cannot hear is how automation gets taught wrong:
    // the first edit switches the curve on, the toggle in the menu is how it
    // is switched back off without losing the shape.
    sample->activeCurve().setEnabled (true);

    if (onNoteEdited != nullptr)
        onNoteEdited();

    dragMode = DragMode::sampleCurvePoint;
    repaint();
    return true;
}


void PianoRollComponent::showSampleCurveMenu (int layerIndex, int sampleId,
                                              juce::Point<int> screenPosition)
{
    auto* layer = stack.getLayer (layerIndex);
    auto* sample = layer != nullptr ? layer->findSample (sampleId) : nullptr;

    if (sample == nullptr)
        return;

    const int  target = sample->curveTarget();     // Phase 82: -1 = 空
    const bool onPan  = target == (int) SampleAutomationTarget::pan;

    juce::PopupMenu menu;

    //-- Where this curve goes ------------------------------------------------
    // Phase 82: this used to be "which of the two curves the block has" and
    // "which of the two parameters" in one menu. Those are two questions now —
    // the left dropdown picks the curve, this one picks the destination — so
    // what is left here is the destination, plus 空.
    menu.addSectionHeader (utf8 ("这条曲线驱动什么"));

    menu.addItem (kRouteMenuNone, utf8 ("空") + "  ·  " + utf8 ("曲线留着，不驱动任何东西"),
                  true, target < 0);

    menu.addSeparator();

    menu.addItem (kRouteItemBase + (int) SampleAutomationTarget::volume, utf8 ("音量"), true,
                  target == (int) SampleAutomationTarget::volume);
    menu.addItem (kRouteItemBase + (int) SampleAutomationTarget::pan, utf8 ("声相"), true, onPan);

    //-- Shape presets --------------------------------------------------------
    // Phase 65: no 启用 / 停用 item. The box's switch was removed for the same
    // reason — a drawn curve plays — and a menu item that brings the bypass back
    // would only move the lie one click away.
    menu.addSeparator();
    menu.addSectionHeader (onPan ? utf8 ("声相曲线") : utf8 ("音量曲线"));

    // "Flat" means the target's own neutral point: unity for volume, dead
    // centre for pan. A single hard-coded 1.0 here would make "reset" turn a
    // pan curve into "hard right", which is the opposite of resetting it.
    menu.addItem (2, utf8 ("重置为平线"), true, false);
    menu.addItem (3, onPan ? utf8 ("左 → 右") : utf8 ("淡入形状"), true, false);
    menu.addItem (4, onPan ? utf8 ("右 → 左") : utf8 ("淡出形状"), true, false);

    juce::Component::SafePointer<PianoRollComponent> safe (this);
    const int layerCapture = layerIndex;
    const int idCapture    = sampleId;

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerCapture, idCapture] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* l = safe->stack.getLayer (layerCapture);
        auto* s = l != nullptr ? l->findSample (idCapture) : nullptr;

        if (s == nullptr)
            return;

        const int curveId = s->curves.getActiveId();

        if (result == kRouteMenuNone)
        {
            // Parked, not deleted: the shape stays and stops driving anything.
            s->curves.route (curveId, -1);
        }
        else if (result >= kRouteItemBase && result < kRouteItemBase + 2)
        {
            // Exclusive: `route` takes the target away from whoever had it, so
            // two curves can never shape one parameter at once.
            s->curves.route (curveId, result - kRouteItemBase);
            s->activeCurve().setEnabled (true);
        }
        else
        {
            // Shapes, not routes. Still the same two ramps the v21 fade handles
            // drew, and still keyed off the target the OPEN curve drives.
            const float neutral = sampleAutomationNeutral (
                s->curveTarget() == (int) SampleAutomationTarget::pan ? SampleAutomationTarget::pan
                                                                     : SampleAutomationTarget::volume);

            switch (result)
            {
                case 2:
                    s->activeCurve().loadPoints ({ { 0.0f, neutral }, { 1.0f, neutral } });
                    break;

                case 3:
                    s->activeCurve().loadPoints ({ { 0.0f, 0.0f }, { 1.0f, 1.0f } });
                    break;

                case 4:
                    s->activeCurve().loadPoints ({ { 0.0f, 1.0f }, { 1.0f, 0.0f } });
                    break;

                default:
                    return;
            }

            s->activeCurve().setEnabled (true);
        }

        if (safe->onNoteEdited != nullptr)
            safe->onNoteEdited();

        safe->repaint();
    });
}


void PianoRollComponent::showSampleCurveListMenu (int layerIndex, int sampleId,
                                                 juce::Point<int> screenPosition)
{
    auto* layer = stack.getLayer (layerIndex);
    auto* sample = layer != nullptr ? layer->findSample (sampleId) : nullptr;

    if (sample == nullptr)
        return;

    std::vector<CurveMenuItem> items;

    for (const auto& slot : sample->curves.getCurves())
        items.push_back ({ slot.id, slot.name, sampleRouteLabel (slot) });

    juce::PopupMenu menu;
    addCurveItems (menu, items, sample->curves.getActiveId());
    addCurveVerbs (menu, sample->curves.getNumCurves() > 1);

    juce::Component::SafePointer<PianoRollComponent> safe (this);
    const int layerCapture = layerIndex;
    const int idCapture    = sampleId;

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerCapture, idCapture] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* l = safe->stack.getLayer (layerCapture);
        auto* s = l != nullptr ? l->findSample (idCapture) : nullptr;

        if (s == nullptr)
            return;

        if (result >= kCurveItemBase && result < kCurveItemBase + s->curves.getNumCurves())
        {
            // View state only: which curve is open is a stored fact (P42).
            s->curves.setActiveId (s->curves.getCurves()[(size_t) (result - kCurveItemBase)].id);
        }
        else if (result == kCurveMenuNew)
        {
            // A new curve starts flat at unity and PARKED: it must not take a
            // target away from the curve already driving it, and an untouched
            // shape should not look like something the user drew.
            const int added = s->curves.add (NoteEnvelope::makeIdle());

            if (added < 0)
                return;     // 满员

            s->curves.setActiveId (added);
        }
        else if (result == kCurveMenuDelete)
        {
            s->curves.remove (s->curves.getActiveId());
        }
        else
        {
            return;
        }

        if (safe->onNoteEdited != nullptr)
            safe->onNoteEdited();

        safe->repaint();
    });
}


void PianoRollComponent::drawSampleAutomationBox (juce::Graphics& g)
{
    auto* sample = getExpandedSample();
    auto* layer  = stack.getLayer (expandedSampleLayerIndex);

    if (sample == nullptr || layer == nullptr)
        return;

    const int   layerIndex = expandedSampleLayerIndex;
    const auto  colour     = layer->getColour();
    const auto  box        = getSampleAutomationBoxBounds (layerIndex, sample->id);

    if (box.isEmpty())
        return;

    //-- Panel ---------------------------------------------------------------
    g.setColour (theme::bgApp.withAlpha (0.97f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (colour.withAlpha (0.8f));
    g.drawRoundedRectangle (box, 5.0f, 1.2f);

    //-- Curve ---------------------------------------------------------------
    const auto inner = getSampleCurveArea (layerIndex, sample->id);

    if (! inner.isEmpty())
    {
        drawGridInside (g, inner);

        const auto& points = sample->activeCurve().getPoints();
        const bool  live   = sample->activeCurve().isEnabled();
        const bool  onPan  = sample->curveTarget() == (int) SampleAutomationTarget::pan;

        // Pan has a meaningful middle, so its area gets a centre hairline: a
        // flat line at 0.5 is silence of movement, and without the reference the
        // user cannot tell "centred" from "slightly right" by eye.
        //
        // Drawn AFTER the fill (see below) and not before: the pan fill runs
        // from the curve to exactly this line, so painting the line first buries
        // it under its own wash and the reference disappears the moment there is
        // anything to read. Brighter than a grid hairline on purpose —
        // `theme::hairline` on the box's near-black background is invisible.
        const auto drawCentreLine = [&]
        {
            if (! onPan || inner.isEmpty())
                return;

            g.setColour (colour.brighter (0.2f).withAlpha (0.30f));
            g.drawHorizontalLine ((int) std::round (inner.getCentreY()),
                                  inner.getX(), inner.getRight());
        };

        // The curve, filled like the note box's: the shape is the block's
        // loudness over its own length, so the fill grows from the bottom. The
        // pan lane fills towards its centre instead, which is what the shape
        // means — away from the middle is "panned".
        if (points.size() >= 2)
        {
            juce::Path fill;
            juce::Path line;

            const float baseY = onPan ? inner.getCentreY() : inner.getBottom();

            fill.startNewSubPath (inner.getX(), baseY);

            bool first = true;

            for (const auto& p : points)
            {
                const juce::Point<float> at { inner.getX() + p.time * inner.getWidth(),
                                              inner.getBottom() - p.value * inner.getHeight() };

                if (first)
                {
                    fill.lineTo (at.getX(), baseY);
                    fill.lineTo (at);
                    line.startNewSubPath (at);
                    first = false;
                }
                else
                {
                    fill.lineTo (at);
                    line.lineTo (at);
                }
            }

            fill.lineTo (inner.getRight(), baseY);
            fill.closeSubPath();

            g.setColour (colour.withAlpha (live ? 0.30f : 0.14f));
            g.fillPath (fill);

            // Between the wash and the line, so it reads as the reference the
            // shape is measured against rather than as the top edge of a block.
            drawCentreLine();

            g.setColour (colour.brighter (0.4f).withAlpha (live ? 0.95f : 0.4f));
            g.strokePath (line, juce::PathStrokeType (ui::pf (1.4f), juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

            for (int i = 0; i < (int) points.size(); ++i)
            {
                const auto& p = points[(size_t) i];
                const juce::Point<float> at { inner.getX() + p.time * inner.getWidth(),
                                              inner.getBottom() - p.value * inner.getHeight() };

                const bool hot = i == hoveredSampleCurvePoint || i == dragSampleCurveIndex;

                g.setColour (juce::Colours::white.withAlpha (hot ? 1.0f : 0.9f));
                g.fillEllipse (at.x - ui::pf (3), at.y - ui::pf (3),
                               ui::pf (6), ui::pf (6));

                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.drawEllipse (at.x - ui::pf (3), at.y - ui::pf (3),
                               ui::pf (6), ui::pf (6), 1.0f);
            }
        }
        else
        {
            // No envelope yet: the centre line is the only thing that says which
            // target is on show, so it still gets drawn.
            drawCentreLine();
        }
    }

    //-- Two dropdowns ------------------------------------------------------
    // Phase 82: the row used to name the target and nothing else, which was
    // fine while a block had exactly two curves and the "which one" question
    // was answered by that name. With a bank the two questions separate: left
    // picks the curve, right picks where it goes.
    const auto curveCell = getSampleCurveMenuArea (layerIndex, sample->id);
    const auto routeCell = getSampleRouteMenuArea (layerIndex, sample->id);

    if (! curveCell.isEmpty() && ! routeCell.isEmpty())
    {
        const auto& active = sample->curves.getActive();
        const int   route  = sample->curveTarget();

        drawMenuCell (g, curveCell,
                      active.name.isNotEmpty() ? active.name : utf8 ("曲线"),
                      colour, hoveredSampleCurveMenu == 1, route < 0);

        if (! curveCellFitsText (curveCell))
            drawCurveGlyph (g, juce::Rectangle<float> (ui::pf (11), ui::pf (11))
                                   .withCentre (curveCell.getCentre()),
                            juce::Colours::white.withAlpha (0.9f));

        const juce::String routeName = route < 0 ? utf8 ("空")
                                      : route == (int) SampleAutomationTarget::pan ? utf8 ("声相")
                                                                                  : utf8 ("音量");

        drawMenuCell (g, routeCell, routeName, colour, hoveredSampleCurveMenu == 2, route < 0);

        if (! curveCellFitsText (routeCell))
        {
            if (route < 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.45f));
                g.setFont (atomic::ui::bold (9.0f));
                g.drawText (utf8 ("空"), routeCell, juce::Justification::centred, false);
            }
            else
            {
                drawModTargetIcon (g, route == (int) SampleAutomationTarget::pan ? NoteModTarget::pan
                                                                                : NoteModTarget::level,
                                   juce::Rectangle<float> (ui::pf (11), ui::pf (11))
                                     .withCentre (routeCell.getCentre()),
                                   juce::Colours::white.withAlpha (0.9f));
            }
        }
    }

    //-- Close ---------------------------------------------------------------
    const auto close = getSampleCloseArea (layerIndex, sample->id);
    juce::Path x;

    x.startNewSubPath (close.getX() + ui::pf (2.5f), close.getY() + ui::pf (2.5f));
    x.lineTo (close.getRight() - ui::pf (2.5f), close.getBottom() - ui::pf (2.5f));
    x.startNewSubPath (close.getRight() - ui::pf (2.5f), close.getY() + ui::pf (2.5f));
    x.lineTo (close.getX() + ui::pf (2.5f), close.getBottom() - ui::pf (2.5f));

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.strokePath (x, juce::PathStrokeType (ui::pf (1.3f), juce::PathStrokeType::mitered,
                                           juce::PathStrokeType::butt));
}


bool PianoRollComponent::simulateSampleCurveEdit (int sampleId, float t, float v)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* clip = active->findSample (sampleId);

    if (clip == nullptr)
        return false;

    // The gesture path the box uses: add when the time is new, move when it
    // lands on an existing point, and the first edit always switches the
    // curve on — the same three lines handleSampleAutomationMouseDown runs.
    const auto& points = clip->activeCurve().getPoints();
    int index = -1;

    for (int i = 0; i < (int) points.size(); ++i)
        if (std::abs (points[(size_t) i].time - juce::jlimit (0.0f, 1.0f, t)) < 0.02f)
        {
            index = i;
            break;
        }

    if (index >= 0)
        clip->activeCurve().movePoint (index, t, v);
    else
        index = clip->activeCurve().addPoint (t, v);

    clip->activeCurve().setEnabled (true);

    return index >= 0;
}


juce::Rectangle<float> PianoRollComponent::getSampleAutomationBoxForTest (int layerIndex,
                                                                          int sampleId) const
{
    return getSampleAutomationBoxBounds (layerIndex, sampleId);
}
} // namespace atomic

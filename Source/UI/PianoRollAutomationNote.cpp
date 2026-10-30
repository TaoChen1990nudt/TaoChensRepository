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
//  Phase 8: the inline note automation box
//==============================================================================
void PianoRollComponent::closeNoteAutomation()
{
    if (expandedNoteId < 0)
        return;

    expandedNoteId     = -1;
    expandedLayerIndex = -1;
    dragEnvelopePoint  = -1;
    hoveredEnvelopePoint = -1;
    hoveredAutomationMenu = 0;

    if (curveSelection == CurveSelection::noteEnvelope)
        curveSelection = CurveSelection::none;

    repaint();
}


NoteEvent* PianoRollComponent::getExpandedNote()
{
    if (auto* layer = stack.getLayer (expandedLayerIndex))
        return layer->findNote (expandedNoteId);

    return nullptr;
}


const NoteEvent* PianoRollComponent::getExpandedNote() const
{
    if (const auto* layer = stack.getLayer (expandedLayerIndex))
        return layer->findNote (expandedNoteId);

    return nullptr;
}


void PianoRollComponent::openNoteAutomation (int layerIndex, int noteId)
{
    // Phase 21: the group box and the note box both float over the staff, so
    // only one may be open — otherwise whichever is drawn last hides the other.
    // Phase 49: the clip box is the third tenant of the same airspace.
    closeGroupAutomation();
    closeSampleAutomation();

    expandedLayerIndex = layerIndex;
    expandedNoteId     = noteId;
    dragEnvelopePoint  = -1;

    // Phase 22: opening a box selects it. The selection is what Ctrl+C and the
    // Edit menu act on, and "the box I just opened" is the only reading that
    // needs no explanation.
    curveSelection = CurveSelection::noteEnvelope;

    repaint();
}


juce::Rectangle<float> PianoRollComponent::getNoteAutomationHandleBounds (const NoteEvent& note) const
{
    const auto bounds = getNoteBounds (note);

    const float band  = ui::pf (kResizeHandleWidth);
    const float air   = ui::pf (kNoteHandleTailAir);
    const float width = ui::pf (kNoteHandleWidth);

    // Distance from the note's right edge to the button's left edge: the resize
    // band first, then a hairline of air. The button therefore never covers the
    // edge that lengthens the note, but it does sit close enough to it to read
    // as the note's own control.
    const float inset = band + air + width;

    // Not enough room for "left band + button + air + right band + a sliver of
    // body": hide the button. A short note keeps its curve — the inspector still
    // edits it — it just does not advertise a button it cannot fit.
    const float minimumWidth = inset + band + ui::pf (kNoteHandleMinBody);

    if (bounds.getWidth() < minimumWidth)
        return {};

    return { bounds.getRight() - inset, bounds.getY() + ui::pf (1), width,
             juce::jmax (ui::pf (8), bounds.getHeight() - ui::pf (2)) };
}


std::pair<juce::Rectangle<float>, juce::Rectangle<float>>
PianoRollComponent::splitCurveRow (juce::Rectangle<float> row)
{
    // The left cell is the wider one: it holds a curve's NAME, while the right
    // holds a target's name, which is one or two characters ("声相", "空").
    // Both keep room for a caret, and the gap is a finger's width so a click
    // meant for one cannot land in the other.
    const float gap  = ui::pf (3);
    const float left = juce::jmax (1.0f, (row.getWidth() - gap) * 0.56f);

    auto curves = row.withWidth (juce::jmin (left, row.getWidth()));
    auto route  = row.withTrimmedLeft (juce::jmin (left + gap, row.getWidth()));

    if (route.getWidth() < 1.0f)
        route.setWidth (1.0f);

    return { curves, route };
}


void PianoRollComponent::drawMenuCell (juce::Graphics& g, juce::Rectangle<float> area,
                                       const juce::String& text, juce::Colour accent,
                                       bool hot, bool parked) const
{
    if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
        return;

    g.setColour (accent.withAlpha (hot ? 0.30f : 0.13f));
    g.fillRoundedRectangle (area, 3.0f);

    if (! curveCellFitsText (area))
        return;             // the caller draws a glyph instead

    // A parked curve (空) keeps its name but loses its brightness: the shape is
    // still there, it just does not do anything, and the cell should say so
    // before the menu explains it.
    g.setColour (juce::Colours::white.withAlpha (parked ? 0.45f : 0.9f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f)).withStyle ("Bold")));
    g.drawText (text, area.reduced (5.0f, 0.0f).withTrimmedRight (9.0f),
                juce::Justification::centredLeft, true);

    const float cx = area.getRight() - 6.5f;
    const float cy = area.getCentreY();

    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.0f, cy - 1.6f);
    chevron.lineTo (cx, cy + 1.8f);
    chevron.lineTo (cx + 3.0f, cy - 1.6f);

    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}


void PianoRollComponent::drawCurveGlyph (juce::Graphics& g, juce::Rectangle<float> area,
                                         juce::Colour colour) const
{
    // A line that rises and falls: "this button is about a curve", said in the
    // eight pixels the narrow variant has. Deliberately not the mod-target
    // icons — those belong to the RIGHT cell, and the two cells must not be
    // mistakable for each other at a glance.
    const float x0 = area.getX() + 1.0f;
    const float x1 = area.getRight() - 1.0f;
    const float w  = juce::jmax (1.0f, x1 - x0);
    const float y0 = area.getCentreY();

    juce::Path squiggle;
    squiggle.startNewSubPath (x0, y0 + area.getHeight() * 0.22f);
    squiggle.cubicTo (x0 + w * 0.30f, y0 - area.getHeight() * 0.42f,
                      x0 + w * 0.70f, y0 + area.getHeight() * 0.42f,
                      x1, y0 - area.getHeight() * 0.22f);

    g.setColour (colour);
    g.strokePath (squiggle, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
}


juce::Rectangle<float> PianoRollComponent::getNoteAutomationBoxBounds (const NoteEvent& note) const
{
    const auto grid = getGridArea();
    const auto nb   = getNoteBounds (note);

    // Exactly as wide as the note. A 1/16 note is 12 px and nothing readable
    // fits in 12 px, so the bottom row switches to an icon instead of widening
    // the box: the curve has to line up with the note above it or the whole
    // point of a per-note envelope is lost.
    // 22 stays a design constant: the note's own width comes from beatWidth,
    // which does not scale, so a floor in scaled pixels would break the
    // "box is as wide as the note" rule the moment the UI grew.
    const float width  = juce::jmax (nb.getWidth(), 22.0f);
    const float height = ui::pf (92);

    const float x = juce::jlimit (grid.getX(),
                                  juce::jmax (grid.getX(), grid.getRight() - width),
                                  nb.getX());

    float y = nb.getBottom() + ui::pf (3);

    // No room underneath (note near the bottom of the view): flip above it.
    if (y + height > grid.getBottom() - ui::pf (2))
        y = nb.getY() - ui::pf (3) - height;

    y = juce::jlimit (grid.getY(), juce::jmax (grid.getY(), grid.getBottom() - height), y);

    return { x, y, width, height };
}


juce::Rectangle<float> PianoRollComponent::getAutomationCurveArea (const NoteEvent& note) const
{
    // Padding shrinks with the box, or a narrow box would have no curve left.
    const auto  box  = getNoteAutomationBoxBounds (note);
    const float padX = juce::jlimit (ui::pf (2), ui::pf (7), box.getWidth() * 0.10f);

    auto inner = box.reduced (padX, ui::pf (6));
    inner.removeFromBottom (ui::pf (30));
    return inner;
}


juce::Rectangle<float> PianoRollComponent::getAutomationMenuArea (const NoteEvent& note) const
{
    const auto  box  = getNoteAutomationBoxBounds (note);
    const float padX = juce::jlimit (ui::pf (2), ui::pf (7), box.getWidth() * 0.10f);

    auto inner = box.reduced (padX, ui::pf (6));
    return inner.removeFromBottom (ui::pf (26));
}


juce::Rectangle<float> PianoRollComponent::getAutomationCurveMenuArea (const NoteEvent& note) const
{
    return splitCurveRow (getAutomationMenuArea (note)).first;
}


juce::Rectangle<float> PianoRollComponent::getAutomationRouteMenuArea (const NoteEvent& note) const
{
    return splitCurveRow (getAutomationMenuArea (note)).second;
}


juce::Rectangle<float> PianoRollComponent::getAutomationCloseArea (const NoteEvent& note) const
{
    const auto  box  = getNoteAutomationBoxBounds (note);
    const float size = juce::jlimit (ui::pf (9), ui::pf (17), box.getWidth() - ui::pf (7));

    return { box.getRight() - size - ui::pf (3), box.getY() + ui::pf (3), size, size };
}


int PianoRollComponent::findEnvelopePointAt (const NoteEvent& note, juce::Point<float> position) const
{
    const auto inner = getAutomationCurveArea (note);
    const auto& points = note.envelope().getPoints();

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const juce::Point<float> p { inner.getX() + points[(size_t) i].time * inner.getWidth(),
                                     inner.getBottom() - points[(size_t) i].value * inner.getHeight() };

        if (p.getDistanceFrom (position) <= 7.0f)
            return i;
    }

    return -1;
}


void PianoRollComponent::dragAutomationPoint (NoteEvent& note, juce::Point<float> position)
{
    if (dragEnvelopePoint < 0)
        return;

    const auto inner = getAutomationCurveArea (note);

    const float t = juce::jlimit (0.0f, 1.0f, (position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
    const float v = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - position.y) / juce::jmax (1.0f, inner.getHeight()));

    note.envelope().movePoint (dragEnvelopePoint, t, v);

    // Phase 62: same rule for a note's own curve — one shape, every place.
    syncShapeOfMember (note.id);

    if (onNoteEdited != nullptr)
        onNoteEdited();

    repaint();
}


bool PianoRollComponent::handleAutomationMouseDown (juce::Point<float> pos,
                                                   juce::ModifierKeys mods, NoteEvent& note)
{

    if (getAutomationCloseArea (note).contains (pos))
    {
        closeNoteAutomation();
        return true;
    }

    if (getAutomationCurveMenuArea (note).contains (pos))
    {
        showNoteCurveMenu (note, localPointToGlobal (pos.roundToInt()));
        return true;
    }

    if (getAutomationRouteMenuArea (note).contains (pos))
    {
        showModTargetMenu (note, localPointToGlobal (pos.roundToInt()));
        return true;
    }

    const auto inner = getAutomationCurveArea (note);

    if (! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    const int hit = findEnvelopePointAt (note, pos);

    if (mods.isPopupMenu())
    {
        // Right-click removes a point; the curve always keeps two.
        if (hit >= 0)
        {
            note.envelope().removePoint (hit);

            if (onNoteEdited != nullptr)
                onNoteEdited();

            repaint();
        }

        return true;
    }

    if (hit >= 0)
    {
        dragEnvelopePoint = hit;
    }
    else
    {
        const float t = juce::jlimit (0.0f, 1.0f, (pos.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
        const float v = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - pos.y) / juce::jmax (1.0f, inner.getHeight()));

        const int added = note.envelope().addPoint (t, v);

        if (added < 0)
            return true;    // envelope is full

        dragEnvelopePoint = added;

        if (onNoteEdited != nullptr)
            onNoteEdited();
    }

    dragMode = DragMode::automationPoint;
    repaint();
    return true;
}


void PianoRollComponent::showModTargetMenu (const NoteEvent& note, juce::Point<int> screenPosition)
{
    const int layerIndex = expandedLayerIndex;
    const int noteId     = note.id;
    const int current    = note.curveTarget();      // Phase 82: -1 = 空

    // Phase 24: the synth this note plays through decides whether there is a
    // second menu half at all. A hosted instrument has a parameter list; the
    // internal oscillator has the three built-in targets and nothing else.
    const int slotId = resolvedSlotForNote (note);

    std::vector<int> paramIndices;

    for (const auto& info : pluginParamsForSlot (slotId))
        paramIndices.push_back (info.index);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("这条曲线驱动什么"));

    // Phase 82: 空 comes first and is never disabled. "Draw it, do not use it"
    // is a real thing to want — a shape kept for later, a curve being built —
    // and it has to be reachable from every state, including from 空 itself.
    menu.addItem (kRouteMenuNone, utf8 ("空") + "  ·  " + utf8 ("曲线留着，不驱动任何东西"),
                  true, current < 0);

    menu.addSeparator();

    for (auto target : { NoteModTarget::level, NoteModTarget::pan, NoteModTarget::pitch })
        menu.addItem (kRouteItemBase + (int) target,
                      modTargetLabel (target) + "  ·  " + modTargetHint (target),
                      true, current == (int) target);

    if (! paramIndices.empty())
    {
        menu.addSeparator();
        addPluginParamSubmenu (menu, slotId, note.modPluginParam(), kPluginParamItemBase);
    }
    else if (slotId >= 0)
    {
        menu.addSeparator();
        menu.addItem (-1, utf8 ("挂到外部音源后可选它的参数"), false, false);
    }

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerIndex, noteId, paramIndices] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* layer = safe->stack.getLayer (layerIndex);

        if (layer == nullptr)
            return;

        auto* target = layer->findNote (noteId);

        if (target == nullptr)
            return;

        if (result == kRouteMenuNone)
        {
            // The curve is NOT deleted: parking is a routing statement, and
            // the shape that was under this menu is still the shape the box
            // draws. `route(-1)` is the only door that says so.
            target->curves.route (target->curves.getActiveId(), -1);
        }
        else if (result >= kPluginParamItemBase)
        {
            const int which = result - kPluginParamItemBase;

            if (! juce::isPositiveAndBelow (which, (int) paramIndices.size()))
                return;

            target->setModPluginParam (paramIndices[(size_t) which]);

            // Switching to a plugin parameter has to make the curve audible
            // for the same reason opening a box does: a curve you can see and
            // cannot hear is the classic way automation gets taught wrong.
            target->envelope().setEnabled (true);
        }
        else if (result >= kRouteItemBase && result < kRouteItemBase + 3)
        {
            // Exclusive by construction: `route` takes the target away from
            // whoever had it (P82's "the later one wins"), so two curves can
            // never end up shaping one parameter.
            target->curves.route (target->curves.getActiveId(), result - kRouteItemBase);
            target->envelope().setEnabled (true);
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


void PianoRollComponent::showNoteCurveMenu (const NoteEvent& note, juce::Point<int> screenPosition)
{
    const int layerIndex = expandedLayerIndex;
    const int noteId     = note.id;

    std::vector<CurveMenuItem> items;

    for (const auto& slot : note.curves.getCurves())
        items.push_back ({ slot.id, slot.name, noteRouteLabel (slot) });

    juce::PopupMenu menu;
    addCurveItems (menu, items, note.curves.getActiveId());
    addCurveVerbs (menu, note.curves.getNumCurves() > 1);

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1)),
                        [safe, layerIndex, noteId] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* layer = safe->stack.getLayer (layerIndex);

        if (layer == nullptr)
            return;

        auto* target = layer->findNote (noteId);

        if (target == nullptr)
            return;

        if (result >= kCurveItemBase && result < kCurveItemBase + target->curves.getNumCurves())
        {
            // Only the view changes. Which curve is on screen has to be a
            // stored fact (P42), not something inferred from the drawing order.
            target->curves.setActiveId (target->curves.getCurves()[(size_t) (result - kCurveItemBase)].id);
        }
        else if (result == kCurveMenuNew)
        {
            // Flat and parked: a brand-new curve must not steal a target from
            // the curve that is already driving it, and a default-shaped ADSR
            // would be a shape nobody drew. It becomes live the moment it is
            // routed or first edited.
            const int added = target->curves.add (NoteEnvelope::makeIdle());

            if (added < 0)
                return;     // 满员

            target->curves.setActiveId (added);
        }
        else if (result == kCurveMenuDelete)
        {
            target->curves.remove (target->curves.getActiveId());
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


void PianoRollComponent::drawNoteAutomationBox (juce::Graphics& g)
{
    auto* note = getExpandedNote();

    if (note == nullptr)
        return;

    auto* layer = stack.getLayer (expandedLayerIndex);

    if (layer == nullptr)
        return;

    const auto colour = layer->getColour();
    const auto box    = getNoteAutomationBoxBounds (*note);
    const auto noteBounds = getNoteBounds (*note);

    //-- Panel ---------------------------------------------------------------
    // Phase 22: the selected box says so. Copy/paste act on "the selected
    // curve", and that has to be a thing you can see without guessing.
    const bool selected = curveSelection == CurveSelection::noteEnvelope;

    g.setColour (theme::bgApp.withAlpha (0.97f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (colour.withAlpha (selected ? 1.0f : 0.8f));
    g.drawRoundedRectangle (box, 5.0f, selected ? 2.2f : 1.2f);

    // A short stem ties the box to the note it belongs to, so a box that had to
    // flip above its note is still unambiguous.
    g.setColour (colour.withAlpha (0.55f));
    g.drawLine (noteBounds.getCentreX(), noteBounds.getBottom(),
                noteBounds.getCentreX(), box.getY(), 1.0f);

    //-- Curve ---------------------------------------------------------------
    const auto inner = getAutomationCurveArea (*note);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (inner, 3.0f);

    g.setColour (juce::Colours::white.withAlpha (0.06f));

    if (inner.getWidth() > 26.0f)
        for (int i = 1; i < 4; ++i)
            g.drawVerticalLine ((int) (inner.getX() + inner.getWidth() * (float) i / 4.0f),
                                inner.getY(), inner.getBottom());

    g.drawHorizontalLine ((int) (inner.getY() + inner.getHeight() * 0.5f),
                          inner.getX(), inner.getRight());

    drawCurveInBox (g, *note, colour, inner);

    // Where the playhead is inside this note, mapped to the curve's own time.
    if (const double p = getPlayheadProgress (expandedLayerIndex, note->id); p >= 0.0)
    {
        const float x = inner.getX() + (float) p * inner.getWidth();

        g.setColour (kPlayheadColour.withAlpha (0.8f));
        g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
    }

    //-- Two dropdowns ------------------------------------------------------
    // Phase 82: which curve (left), and what it drives (right). A note can now
    // carry several shapes, so "the parameter" stopped being one question.
    //
    // Wide box: both names and two carets. Narrow box: two glyphs. "音量"
    // needs 24 px and a 1/16 note is 12 px wide, so text would be clipped into
    // an unreadable glyph anyway — but both cells still open their menus.
    const auto curveCell = getAutomationCurveMenuArea (*note);
    const auto routeCell = getAutomationRouteMenuArea (*note);
    const auto& active   = note->curves.getActive();

    drawMenuCell (g, curveCell,
                  active.name.isNotEmpty() ? active.name : utf8 ("曲线"),
                  colour, hoveredAutomationMenu == 1, ! active.isRouted());

    if (! curveCellFitsText (curveCell))
    {
        const float side  = juce::jmin (12.0f, juce::jmin (curveCell.getWidth(),
                                                           curveCell.getHeight()));
        drawCurveGlyph (g, juce::Rectangle<float> (side, side).withCentre (curveCell.getCentre()),
                        juce::Colours::white.withAlpha (0.92f));
    }

    drawMenuCell (g, routeCell, noteTargetLabel (*note), colour,
                  hoveredAutomationMenu == 2, note->curveTarget() < 0);

    if (! curveCellFitsText (routeCell))
    {
        const float side  = juce::jmin (12.0f, juce::jmin (routeCell.getWidth(),
                                                           routeCell.getHeight()));
        const auto  glyph = juce::Rectangle<float> (side, side).withCentre (routeCell.getCentre());

        if (note->drivesPluginParam())
            drawKnobGlyph (g, glyph, juce::Colours::white.withAlpha (0.92f));
        else if (note->curveTarget() < 0)
            g.drawText (utf8 ("空"), routeCell, juce::Justification::centred, false);
        else
            drawModTargetIcon (g, note->modTarget(), glyph, juce::Colours::white.withAlpha (0.92f));
    }

    //-- Close ---------------------------------------------------------------
    const auto close = getAutomationCloseArea (*note);

    g.setColour (juce::Colours::white.withAlpha (0.30f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (12.0f))));
    g.drawText ("x", close, juce::Justification::centred, false);
}


void PianoRollComponent::drawModTargetIcon (juce::Graphics& g, NoteModTarget target,
                                            juce::Rectangle<float> area, juce::Colour colour) const
{
    g.setColour (colour);

    const float midY = area.getCentreY();

    switch (target)
    {
        case NoteModTarget::level:
        {
            // A fader: a track with a knob on it.
            g.drawLine (area.getX(), midY, area.getRight(), midY, 1.2f);
            g.fillEllipse (juce::Rectangle<float> (4.6f, 4.6f)
                             .withCentre ({ area.getX() + area.getWidth() * 0.62f, midY }));
            break;
        }

        case NoteModTarget::pan:
        {
            // Two end stops with a dot between them: left / right.
            g.drawLine (area.getX() + 1.0f, midY, area.getRight() - 1.0f, midY, 1.2f);
            g.drawVerticalLine ((int) area.getX(), midY - 2.5f, midY + 2.5f);
            g.drawVerticalLine ((int) (area.getRight() - 1.0f), midY - 2.5f, midY + 2.5f);
            g.fillEllipse (juce::Rectangle<float> (3.4f, 3.4f).withCentre ({ area.getCentreX(), midY }));
            break;
        }

        case NoteModTarget::pitch:
        default:
        {
            // A rising staircase.
            const float x0 = area.getX() + 1.0f;
            const float y0 = area.getBottom() - 1.0f;
            const float w  = juce::jmax (2.0f, (area.getWidth() - 2.0f) / 3.0f);

            juce::Path stairs;
            stairs.startNewSubPath (x0, y0);
            stairs.lineTo (x0, y0 - 3.0f);
            stairs.lineTo (x0 + w, y0 - 3.0f);
            stairs.lineTo (x0 + w, y0 - 6.0f);
            stairs.lineTo (x0 + w * 2.0f, y0 - 6.0f);
            stairs.lineTo (x0 + w * 2.0f, y0 - 9.0f);
            stairs.lineTo (x0 + w * 3.0f, y0 - 9.0f);

            g.strokePath (stairs, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered,
                                                        juce::PathStrokeType::butt));
            break;
        }
    }
}


void PianoRollComponent::drawCurveInBox (juce::Graphics& g, const NoteEvent& note,
                                         juce::Colour colour, juce::Rectangle<float> inner)
{
    const auto& envelope = note.envelope();
    const auto& points   = envelope.getPoints();

    auto toPoint = [&inner] (const EnvelopePoint& p)
    {
        return juce::Point<float> (inner.getX() + p.time * inner.getWidth(),
                                   inner.getBottom() - p.value * inner.getHeight());
    };

    if (points.size() >= 2)
    {
        juce::Path curve;
        curve.startNewSubPath (toPoint (points.front()));

        // Straight segments between breakpoints: this box is an editor, so it
        // has to show exactly what movePoint() stored, not a smoothed guess.
        for (size_t i = 1; i < points.size(); ++i)
            curve.lineTo (toPoint (points[i]));

        // Fill under the curve for a body the eye can follow at 108 px wide.
        auto filled = curve;
        filled.lineTo (inner.getRight(), inner.getBottom());
        filled.lineTo (inner.getX(), inner.getBottom());
        filled.closeSubPath();

        g.setColour (colour.withAlpha (0.18f));
        g.fillPath (filled);

        g.setColour (colour.brighter (0.4f).withAlpha (envelope.isEnabled() ? 0.95f : 0.4f));
        g.strokePath (curve, juce::PathStrokeType (1.6f));
    }

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto p = toPoint (points[(size_t) i]);
        const bool hot = i == hoveredEnvelopePoint || i == dragEnvelopePoint;

        g.setColour (hot ? juce::Colours::white : colour.brighter (0.6f));
        g.fillEllipse (p.x - (hot ? 3.2f : 2.4f), p.y - (hot ? 3.2f : 2.4f),
                       hot ? 6.4f : 4.8f, hot ? 6.4f : 4.8f);
    }
}
} // namespace atomic

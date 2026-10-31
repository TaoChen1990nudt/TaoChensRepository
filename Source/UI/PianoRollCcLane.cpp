#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Model/MidiCc.h"
#include "Model/Routing.h"
#include "Model/VelocityLane.h"

#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{
using namespace roll;

//==============================================================================
//  Phase 98: the controller lane
//
//  The strip at the bottom of the sheet now has two kinds of tenant. The
//  velocity bars live in PianoRollVelocity.cpp; the three MIDI controllers live
//  here — and so do the four tabs and the dispatch, because "which tenant is
//  showing" is a fact about the strip, not about either tenant.
//
//  Three decisions, and the first two are the ones that would be wrong if this
//  were written as "another velocity lane":
//
//  1. ONE strip, four tabs, never four stacked lanes. A CC curve wants the full
//     width to read as a shape; four lanes at 14 px each would read as
//     decoration and would eat 200 px of the note grid for something the user
//     edits one at a time.
//
//  2. A point is a CIRCLE you can grab, and `ccPointFor` is the only place that
//     decides where it is. Paint, hit test and drag all ask it — the same rule
//     `velocityBarFor` follows, for the same reason.
//
//  3. Touching a curve SWITCHES IT ON. A lane that is switched off transmits
//     nothing (see `midicc`), so the alternative — draw first, discover later
//     that nothing happened, hunt for a switch in a menu — is the most
//     expensive kind of confusion this UI can produce. Making the first touch
//     the enable is what most hosts do, and it costs one line.
//==============================================================================

juce::Rectangle<int> PianoRollComponent::getLaneTabBounds (int tab) const
{
    const auto strip = getVelocityLaneArea();

    if (strip.isEmpty() || ! juce::isPositiveAndBelow (tab, numLaneTabs))
        return {};

    // Four rows in the gutter, one per tab. Vertical because the gutter is
    // 74 px wide and 56 px tall: four 18-px-wide columns would not fit "力度",
    // and the lane's height is not negotiable (it is shared with the bars).
    auto gutter = strip.withWidth (keyboardWidth).reduced (ui::px (4), ui::px (3));

    const int rowHeight = gutter.getHeight() / numLaneTabs;

    if (rowHeight <= 0)
        return {};

    return { gutter.getX(), gutter.getY() + tab * rowHeight,
             gutter.getWidth(), rowHeight };
}


int PianoRollComponent::laneTabAt (juce::Point<int> position) const
{
    for (int tab = 0; tab < numLaneTabs; ++tab)
        if (getLaneTabBounds (tab).contains (position))
            return tab;

    return -1;
}


juce::Point<float> PianoRollComponent::ccPointFor (int lane, int index) const
{
    const auto track = laneTrackArea();

    if (track.isEmpty())
        return {};

    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return {};

    const auto& points = active->ccLane (lane).getPoints();

    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return {};

    const auto& point = points[(size_t) index];

    // Vertical mapping is the velocity lane's, deliberately: both are 0..1 with
    // 1 at the top of the strip. A second linear-to-y function would be a second
    // place for "which way is up" to be decided.
    return { screenXForBeat (point.beat),
             velocitylane::toY (point.value, track) };
}


int PianoRollComponent::ccPointAt (int lane, juce::Point<float> position) const
{
    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return -1;

    const auto& points = active->ccLane (lane).getPoints();

    // NEAREST wins within a radius, rather than first-within-radius. Two
    // breakpoints can be a few pixels apart after a zoom-out, and "grab the one
    // I aimed at" is the whole difference between an editor and a lottery.
    const float radius = ui::pf (7.0f);

    int   best      = -1;
    float bestRange = radius;

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const float range = ccPointFor (lane, i).getDistanceFrom (position);

        if (range <= bestRange)
        {
            bestRange = range;
            best      = i;
        }
    }

    return best;
}


void PianoRollComponent::handleCcPress (juce::Point<float> position, juce::ModifierKeys mods)
{
    if (mods.isPopupMenu())
    {
        showCanvasMenu (localPointToGlobal (position.roundToInt()));
        return;
    }

    int lane = laneTabToCc (laneTab);

    if (lane < 0)
        return;

    const auto track = laneTrackArea();

    if (track.isEmpty())
        return;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    auto& curve = active->ccLane (lane);

    const int hit = ccPointAt (lane, position);

    if (hit >= 0)
    {
        // Rule 3: grabbing a point is a statement that this curve is in use.
        // The alternative — a curve that is drawn, looks alive, and transmits
        // nothing because of a switch three levels up a menu — is worse than
        // the rare case of a user who wanted to edit a switched-off curve
        // without waking it.
        if (! curve.isEnabled())
            curve.setEnabled (true);

        ccDragLane  = lane;
        ccDragIndex = hit;
        dragMode    = DragMode::ccDrag;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        repaint();
        return;
    }

    // Empty lane: insert a point where the pointer is and start dragging it.
    // The press that adds is also the press that grabs, so "click and pull" is
    // one gesture — and a click that does not move leaves a point exactly where
    // the user clicked, which is what a click on an empty curve should do.
    const double beat  = beatAtScreenX (position.x);
    const float  value = velocitylane::fromY (position.y, track);

    const int added = curve.addPoint (beat, value, timeline::defaultLengthBeats);

    if (added < 0)
        return;                       // the curve is full; no gesture starts

    curve.setEnabled (true);

    ccDragLane  = lane;
    ccDragIndex = added;
    dragMode    = DragMode::ccDrag;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);

    stack.notifyNotesChanged();
    repaint();
}


void PianoRollComponent::applyCcDragToScreen (juce::Point<float> position)
{
    if (ccDragLane < 0 || ccDragIndex < 0)
        return;

    const auto track = laneTrackArea();

    if (track.isEmpty())
        return;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    auto& curve = active->ccLane (ccDragLane);

    // Absolute, not a delta — the opposite of the velocity drag, and for a
    // reason that belongs to the object being edited. A velocity drag moves a
    // SET of bars by one amount, so a delta is the only thing that survives the
    // clamping of its loudest member. A controller point is one point, and
    // "the point is under my pointer" is a statement an absolute set makes
    // exactly true. `movePoint` re-sorts and hands back the new index, which is
    // what keeps the gesture attached to the same breakpoint after it passes a
    // neighbour.
    const double beat  = beatAtScreenX (position.x);
    const float  value = velocitylane::fromY (position.y, track);

    ccDragIndex = curve.movePoint (ccDragIndex, beat, value, timeline::defaultLengthBeats);
}


void PianoRollComponent::simulateCcClickForTest (double beat, float value)
{
    const auto track = laneTrackArea();

    if (track.isEmpty())
        return;

    handlePress ({ screenXForBeat (beat), velocitylane::toY (value, track) }, {}, 1);
}


void PianoRollComponent::simulateCcDragForTest (double beat, float value)
{
    const auto track = laneTrackArea();

    if (track.isEmpty() || dragMode != DragMode::ccDrag)
        return;

    applyCcDragToScreen ({ screenXForBeat (beat), velocitylane::toY (value, track) });
    stack.notifyNotesChanged();

    dragMode    = DragMode::none;
    ccDragLane  = -1;
    ccDragIndex = -1;
    updateCursor();
}


//==============================================================================
void PianoRollComponent::drawLaneArea (juce::Graphics& g)
{
    if (getVelocityLaneArea().isEmpty())
        return;

    if (laneTab == LaneTab::velocity)
        drawVelocityLane (g);
    else
        drawCcLane (g);

    drawLaneTabs (g);
}


void PianoRollComponent::drawLaneTabs (juce::Graphics& g)
{
    static const char* const labels[numLaneTabs] = { "力度", "CC1", "CC11", "CC64" };

    for (int tab = 0; tab < numLaneTabs; ++tab)
    {
        const auto bounds = getLaneTabBounds (tab);

        if (bounds.isEmpty())
            continue;

        const bool  picked = (int) laneTab == tab;
        const auto  row    = bounds.toFloat().reduced (0.0f, ui::pf (1.0f));

        if (picked)
        {
            g.setColour (theme::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (row, ui::pf (3.0f));

            g.setColour (theme::accent.withAlpha (0.55f));
            g.drawRoundedRectangle (row.reduced (0.5f), ui::pf (3.0f), 1.0f);
        }

        // A dot beside the three controller tabs once that curve is switched
        // on: "which of these three did I actually draw" is otherwise three
        // clicks away, and the answer is invisible from the tab strip.
        const int lane = (int) tab - 1;

        if (lane >= 0)
            if (const auto* active = stack.getActiveLayer(); active != nullptr)
                if (active->ccLane (lane).isEnabled())
                {
                    g.setColour (theme::accent.withAlpha (0.85f));
                    g.fillEllipse (row.getRight() - ui::pf (4.0f), row.getCentreY() - ui::pf (1.5f),
                                   ui::pf (3.0f), ui::pf (3.0f));
                }

        g.setColour (picked ? theme::accent : kText.withAlpha (0.55f));
        g.setFont (ui::section (8.0f));
        g.drawText (utf8 (labels[tab]), bounds.reduced (ui::px (2), 0),
                    juce::Justification::centredLeft, false);
    }
}


bool PianoRollComponent::hasMidiTargetForLane() const
{
    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // The same resolver the publisher uses, asked here rather than re-derived:
    // "can this layer's instrument receive controllers" is
    // `routing::layerSlotId`'s answer plus the slot's plugin id, and a panel that
    // worked it out a second way would eventually describe a different
    // instrument than the one that sounds.
    const auto* slot = rack.findSlot (routing::layerSlotId (*active, rack));

    return slot != nullptr && slot->pluginId > 0;
}


void PianoRollComponent::drawCcLane (juce::Graphics& g)
{
    const auto strip = getVelocityLaneArea();
    const auto track = laneTrackArea();

    if (strip.isEmpty())
        return;

    g.setColour (kGridBackground);
    g.fillRect (strip);

    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRect (strip.withWidth (keyboardWidth));

    const int lane = laneTabToCc (laneTab);

    if (lane < 0 || track.isEmpty())
        return;

    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    const auto& curve  = active->ccLane (lane);
    const auto& points = curve.getPoints();

    const bool live = curve.isEnabled();

    if (! live)
        g.setOpacity (0.45f);

    //-- The two meaningful lines ---------------------------------------------
    // Halfway up on a mod wheel means nothing; on expression it means "half
    // loud". Drawn anyway because "which way is more" has to be answerable at a
    // glance, and the floor is where a curve that was never drawn sits.
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (juce::Rectangle<float> (track.getX(), velocitylane::toY (0.5f, track),
                                        track.getWidth(), 1.0f));

    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillRect (juce::Rectangle<float> (track.getX(), track.getBottom() - 1.0f,
                                        track.getWidth(), 1.0f));

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (track.toNearestInt());

        const auto colour = active->getColour().withMultipliedBrightness (1.05f);

        //-- The curve, including its flat tails ------------------------------
        // A controller curve is FLAT outside its first and last point, not
        // absent — that is what `automationValueAt` does and what the plugin
        // will receive. Drawing only between the points would show a line that
        // stops, which reads as "no data here" instead of "held at this value".
        if (points.size() >= 2)
        {
            const float headX = screenXForBeat (points.front().beat);
            const float headY = velocitylane::toY (points.front().value, track);
            const float tailY = velocitylane::toY (points.back().value, track);

            juce::Path path;
            path.startNewSubPath (track.getX(), headY);
            path.lineTo (headX, headY);

            for (size_t i = 1; i < points.size(); ++i)
                path.lineTo (screenXForBeat (points[i].beat),
                             velocitylane::toY (points[i].value, track));

            path.lineTo (track.getRight(), tailY);

            g.setColour (colour.withAlpha (0.85f));
            g.strokePath (path, juce::PathStrokeType (ui::pf (1.6f),
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        //-- The handles ------------------------------------------------------
        for (int i = 0; i < (int) points.size(); ++i)
        {
            const auto centre = ccPointFor (lane, i);
            const bool held   = (dragMode == DragMode::ccDrag
                                  && ccDragLane == lane && ccDragIndex == i);

            const float radius = ui::pf (held ? 4.5f : 3.2f);

            g.setColour (held ? juce::Colours::white : colour);
            g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

            g.setColour (juce::Colours::white.withAlpha (held ? 0.95f : 0.45f));
            g.drawEllipse (centre.x - radius, centre.y - radius,
                           radius * 2.0f, radius * 2.0f, 1.0f);
        }
    }

    //-- The HUD: the number under the pointer, or where this curve goes ------
    // One corner for both, and for both tenants of the strip (see the velocity
    // lane's readout): the eye learns one place, and a tab switch does not move
    // the thing it is following.
    //
    // Why the DESTINATION is permanent and the VALUE is not: a controller is the
    // one thing in this roll whose effect is invisible here — it leaves through an
    // instrument that may not even be able to receive it — and "the mod wheel does
    // nothing" versus "your synth has no MIDI input" is the difference between a
    // bug report and a shrug. The user needs that BEFORE drawing, not after. The
    // number, on the other hand, is wallpaper if it is always up: it matters
    // exactly while choosing what to make it.
    {
        const int hovered = ccDragLane == lane && ccDragIndex >= 0
                              ? ccDragIndex
                              : ccPointAt (lane, getMouseXYRelative().toFloat());

        const bool holding = juce::isPositiveAndBelow (hovered, (int) points.size());
        const bool routed  = hasMidiTargetForLane();

        const juce::String text = holding
                                    ? juce::String (midicc::toMidi (points[(size_t) hovered].value))
                                    : (routed ? utf8 ("→ 已连到宿主音源")
                                              : utf8 ("→ 内置音源，收不到 CC"));

        const auto hud = track.toNearestInt()
                              .withHeight (ui::px (14))
                              .withWidth (holding ? ui::px (34) : ui::px (128))
                              .translated (ui::px (5), ui::px (3));

        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (hud.toFloat().reduced (ui::pf (1.0f)), ui::pf (3.0f));

        // Orange-ish accent when the curve has nowhere to go, so the sentence is
        // a warning rather than a caption.
        g.setColour (routed || holding ? theme::accent : theme::accent.withAlpha (0.90f));
        g.setFont (ui::section (8.0f));
        g.drawText (text, hud.reduced (ui::px (3), 0),
                    holding ? juce::Justification::centred : juce::Justification::centredLeft,
                    false);
    }

    // Seam above the lane, drawn last so nothing paints over it.
    g.setColour (kOutline);
    g.drawHorizontalLine (strip.getY(), 0.0f, (float) getWidth());
}

} // namespace atomic

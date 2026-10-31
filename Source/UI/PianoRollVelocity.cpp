#include "PianoRollComponent.h"
#include "PianoRollInternals.h"

#include "Model/VelocityLane.h"

#include "Util/Text.h"
#include "Util/Theme.h"

namespace atomic
{
using namespace roll;

//==============================================================================
//  Phase 97: the velocity lane
//
//  A bar per note along the bottom of the sheet. It is a VIEW of
//  `NoteEvent::velocity` — the same number the engine reads — so editing here
//  writes the note, fires `notesChanged`, and inherits the undo bracket, the
//  group shape sync and the project format without any of them being told a
//  lane exists.
//
//  Three decisions shape the code below:
//
//  1. ONE rectangle per bar, asked by paint, hit test and marquee alike
//     (`velocityBarFor`). A bar you can see and a bar you can grab are the same
//     pixels or they are two bugs.
//
//  2. A press on a bar is a DELTA from where the press landed, not an absolute
//     set to where the pointer is. Absolute would make "grab the middle of a
//     phrase and lift it" impossible: the first pixel of the gesture would
//     rewrite every bar to one value.
//
//  3. Only the ACTIVE layer is drawn. A bar you cannot drag is a bar that lies
//     about being one, and this lane edits, it does not report.
//==============================================================================

juce::Rectangle<float> PianoRollComponent::velocityBarFor (const NoteEvent& note) const
{
    const auto track = laneTrackArea();

    if (track.isEmpty())
        return {};

    const float head = screenXForBeat (note.startBeat);
    const float tail = screenXForBeat (note.endBeat());

    // A stick at the note's head: as wide as a short note really is, capped at
    // 16 px so a held note does not turn the lane into a bar chart of lengths,
    // and never narrower than 3 px so a 1/64 note stays grabbable at any zoom.
    const float right = juce::jmax (head + ui::pf (3.0f),
                                    juce::jmin (tail, head + ui::pf (16.0f)));

    const float bottom = track.getBottom();
    const float top    = velocitylane::toY (note.velocity, track);

    // A bar never gets shorter than 2 px. At the floor the honest height is one
    // and a bit pixels, which is a pixel you cannot hit — and "the quietest bar
    // is the one you cannot select" is a trap. Two pixels reads as "almost
    // nothing", which is what the floor means.
    const float height = juce::jmax (ui::pf (2.0f), bottom - top);

    return { head, bottom - height, right - head, height };
}


juce::Rectangle<float> PianoRollComponent::getVelocityBarBounds (int noteId) const
{
    if (! showVelocityLane)
        return {};

    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return {};

    const auto* note = active->findNote (noteId);

    return note != nullptr ? velocityBarFor (*note) : juce::Rectangle<float>();
}


int PianoRollComponent::velocityBarAt (juce::Point<float> position) const
{
    if (! showVelocityLane)
        return -1;

    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return -1;

    const auto& notes = active->getNotes();

    // Backwards through the paint order: two notes that start on the same beat
    // at different pitches have bars in exactly the same place, and the one the
    // user can see on top is the one that answers. (Rule 9, applied to a second
    // surface: the pixel belongs to whoever was drawn last on it.)
    for (int i = (int) notes.size(); --i >= 0;)
    {
        const auto& note = notes[(size_t) i];

        if (velocityBarFor (note).contains (position))
            return note.id;
    }

    return -1;
}


void PianoRollComponent::handleVelocityPress (juce::Point<float> position,
                                              juce::ModifierKeys mods)
{
    if (mods.isPopupMenu())
    {
        showCanvasMenu (localPointToGlobal (position.roundToInt()));
        return;
    }

    const int barId = velocityBarAt (position);

    if (barId < 0)
    {
        // Empty lane: a sweep over bars, starting now. The selection is let go
        // first, exactly as a click on empty canvas in the grid does — otherwise
        // "click away to deselect" would work everywhere except here.
        if (! (mods.isShiftDown() || mods.isCommandDown()))
        {
            selection.clear();
            sampleSelection.clear();
            notifySelectionChanged();
        }

        dragMode = DragMode::marquee;
        marqueeStart = marqueeCurrent = position;
        marqueeAdditive = mods.isShiftDown() || mods.isCommandDown();
        marqueeInLane = true;          // this band selects bars, not rectangles
        repaint();
        return;
    }

    // What the drag will move. A bar already in the selection keeps the
    // selection it is part of — that is what lets a sweep pick a phrase and then
    // one drag lift the whole phrase (rule 11: pressing a member of the current
    // selection must not replace that selection).
    if (! isSelected (barId))
    {
        std::vector<int> notes { barId };
        std::vector<int> clips;

        // A closed group answers as one appearance (rule 10): moving one
        // member's dynamics out of formation is the accident the container
        // exists to prevent.
        expandToWholeOccurrences (notes, clips);

        if (mods.isShiftDown() || mods.isCommandDown())
        {
            auto mergedNotes = selection;
            auto mergedClips = sampleSelection;

            for (int id : notes)
                if (std::find (mergedNotes.begin(), mergedNotes.end(), id) == mergedNotes.end())
                    mergedNotes.push_back (id);

            for (int id : clips)
                if (std::find (mergedClips.begin(), mergedClips.end(), id) == mergedClips.end())
                    mergedClips.push_back (id);

            setMixedSelection (std::move (mergedNotes), std::move (mergedClips));
        }
        else
        {
            setSelection (std::move (notes));
        }
    }

    // Capture the heights. The grabbed note leads, because the gutter readout
    // names the bar under the pointer and not "some bar in the set".
    velocityOriginals.clear();
    velocityDragAnchorY = position.y;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    auto remember = [this, active] (int id)
    {
        if (const auto* note = active->findNote (id))
            velocityOriginals.push_back ({ id, note->velocity });
    };

    remember (barId);

    for (int id : selection)
        if (id != barId)
            remember (id);

    if (velocityOriginals.empty())
        return;

    dragMode = DragMode::velocityDrag;
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    repaint();
}


void PianoRollComponent::applyVelocityDragToScreenY (float screenY)
{
    const auto track = laneTrackArea();

    if (track.isEmpty() || velocityOriginals.empty())
        return;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    // ONE delta for the whole set, measured from the press and never from the
    // previous mouse-move: a drag that comes back to where it started restores
    // the velocities it started with, to the last bit. Clamping is per note
    // (`velocitylane::shifted`) — a phrase that runs into the ceiling flattens
    // there instead of dragging its quiet end up to meet it.
    const float delta = (velocityDragAnchorY - screenY) / track.getHeight();

    for (const auto& original : velocityOriginals)
        if (auto* note = active->findNote (original.id))
            note->velocity = velocitylane::shifted (original.velocity, delta);
}


void PianoRollComponent::simulateVelocityDragForTest (int noteId, float fromY, float toY)
{
    const auto bar = getVelocityBarBounds (noteId);

    if (bar.isEmpty())
        return;

    handlePress ({ bar.getCentreX(), fromY }, {}, 1);

    if (dragMode != DragMode::velocityDrag)
        return;

    applyVelocityDragToScreenY (toY);
    stack.notifyNotesChanged();

    dragMode = DragMode::none;
    velocityOriginals.clear();
    updateCursor();
}


void PianoRollComponent::drawVelocityLane (juce::Graphics& g)
{
    const auto strip = getVelocityLaneArea();

    if (strip.isEmpty())
        return;

    const auto track  = laneTrackArea();
    const auto gutter = strip.withWidth (keyboardWidth);

    g.setColour (kGridBackground);
    g.fillRect (strip);

    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRect (gutter);

    //-- The value axis -------------------------------------------------------
    if (! track.isEmpty())
    {
        // Halfway up, faint: a reference for "this bar is louder than that one"
        // without turning the strip into a ruler nobody reads.
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float> (track.getX(), velocitylane::toY (0.5f, track),
                                            track.getWidth(), 1.0f));

        // The floor. Bars stand on it, so it has to be visible.
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (track.getX(), track.getBottom() - 1.0f,
                                            track.getWidth(), 1.0f));
    }

    //-- The bars -------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (track.toNearestInt());

        if (const auto* active = stack.getActiveLayer())
        {
            for (const auto& note : active->getNotes())
            {
                const auto bar = velocityBarFor (note);

                if (! bar.intersects (track))
                    continue;

                // Same colour rule as the note up there (brightness carries the
                // velocity too), so a bar and the note it belongs to are the
                // same colour at a glance.
                const auto colour = active->getColour()
                                      .withMultipliedBrightness (0.78f + 0.35f * note.velocity);
                const bool  picked = isSelected (note.id);
                const float alpha  = solidAlpha * takeFadeOf (*active, note.groupId);

                g.setColour ((picked ? colour.brighter (0.30f) : colour)
                               .withAlpha (alpha * 0.90f));
                g.fillRoundedRectangle (bar, ui::pf (1.5f));

                if (picked)
                {
                    g.setColour (juce::Colours::white.withAlpha (alpha * 0.55f));
                    g.drawRoundedRectangle (bar.reduced (0.5f), ui::pf (1.5f), 1.0f);
                }
            }
        }
    }

    //-- A sweep that started here -------------------------------------------
    // Drawn here rather than in the grid, which is clipped to its own area: the
    // band lives in the lane, so the lane draws it.
    if (dragMode == DragMode::marquee && marqueeInLane)
    {
        const auto band = juce::Rectangle<float> (marqueeStart, marqueeCurrent);

        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRect (band);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawRect (band, 1.0f);
    }

    //-- The readout ----------------------------------------------------------
    // Phase 98 moved this out of the gutter, because the gutter is now four tab
    // buttons wide and a title there would have overlapped them.
    //
    // Top-LEFT of the lane, in a dark HUD, which is also where the controller
    // lane puts its own status line. One corner for both tenants on purpose:
    // the eye learns one place, and a tab switch does not move the number it is
    // following. The HUD is opaque enough to sit over a bar rather than beside
    // it — an 8 px bar at beat 0 is exactly where the first number appears.
    juce::String readout;

    if (dragMode == DragMode::velocityDrag && ! velocityOriginals.empty())
    {
        if (const auto* active = stack.getActiveLayer())
            if (const auto* grabbed = active->findNote (velocityOriginals.front().id))
                readout = juce::String (grabbed->velocity, 2);
    }
    else if (selection.size() == 1)
    {
        if (const auto* active = stack.getActiveLayer())
            if (const auto* only = active->findNote (selection.front()))
                readout = juce::String (only->velocity, 2);
    }

    if (readout.isNotEmpty() && ! track.isEmpty())
    {
        const auto hud = track.toNearestInt()
                              .withHeight (ui::px (14))
                              .withWidth (ui::px (34))
                              .translated (ui::px (5), ui::px (3));

        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (hud.toFloat().reduced (ui::pf (1.0f)), ui::pf (3.0f));

        g.setColour (theme::accent);
        g.setFont (ui::section (9.0f));
        g.drawText (readout, hud, juce::Justification::centred, false);
    }

    // Seam above the lane, drawn last so the bars cannot paint over it.
    g.setColour (kOutline);
    g.drawHorizontalLine (strip.getY(), 0.0f, (float) getWidth());
}

} // namespace atomic

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


NoteEvent* PianoRollComponent::hitTestActiveLayer (juce::Point<float> position)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return nullptr;

    auto& notes = active->getNotes();

    // Reverse: the topmost drawn note wins, matching what the user sees.
    for (auto it = notes.rbegin(); it != notes.rend(); ++it)
        if (getNoteBounds (*it).contains (position))
            return &(*it);

    return nullptr;
}


const NoteEvent* PianoRollComponent::hitTestActiveLayer (juce::Point<float> position) const
{
    const auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return nullptr;

    const auto& notes = active->getNotes();

    // Reverse: the topmost drawn note wins, matching what the user sees.
    for (auto it = notes.rbegin(); it != notes.rend(); ++it)
        if (getNoteBounds (*it).contains (position))
            return &(*it);

    return nullptr;
}


//==============================================================================
//  Mouse
//==============================================================================
void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    handlePress (e.getPosition().toFloat(), e.mods, e.getNumberOfClicks());
}


void PianoRollComponent::handlePress (juce::Point<float> pos, juce::ModifierKeys mods,
                                      int clicks)
{
    grabKeyboardFocus();

    if (onCanvasMouseDown != nullptr)
        onCanvasMouseDown();

    // The event's own point is integral; the grid is in floats. Both are needed
    // below, so the conversion happens once, at the top.
    const auto ipos = pos.roundToInt();

    // ---- Tool strip ---------------------------------------------------------
    if (getToolbarArea().contains (ipos))
    {
        static const Tool tools[5] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand,
                                     Tool::knife };

        for (int i = 0; i < 5; ++i)
            if (getToolButtonBounds (i).contains (ipos))
            {
                setTool (tools[i]);
                return;
            }

        if (getToolButtonBounds (5).contains (ipos))
        {
            // Copy / clear. Copying switches to the pencil in the same click:
            // "I copied this note's state" is only useful if the next thing you
            // do is draw with it.
            if (hasPaintState)
                clearPencilState();
            else if (copyNoteStateFromSelection())
                setTool (Tool::pencil);

            return;
        }

        if (getToolButtonBounds (6).contains (ipos))
        {
            // Phase 16. With nothing selected there is nothing to group, and
            // silently doing nothing is worse than saying so in the strip.
            // Phase 38: "something" means either kind — a clip-only selection
            // used to reach this line and get nothing.
            if (hasAnySelection())
                groupSelection();

            return;
        }

        if (getToolButtonBounds (7).contains (ipos))
        {
            // Phase 20: the inverse of 编组, next to it — and Phase 21 moved the
            // magnet past it, because 编组 / 解组 are one pair and the snap
            // toggle is a different kind of control. Detaching the selection
            // never moves a note; it only drops the membership, and an emptied
            // group is pruned (which is what tells the host to drop its curve).
            // Phase 38: clips too, for the same reason 编组 takes them.
            if (hasAnySelection())
                ungroupSelection();

            repaint (getToolbarArea());
            return;
        }

        if (getToolButtonBounds (8).contains (ipos))
        {
            setSnapEnabled (! snapEnabled);
            repaint (getToolbarArea());
            return;
        }

        if (getToolButtonBounds (9).contains (ipos))
        {
            // Phase 97: a VIEW toggle, like the magnet — it changes what the
            // sheet shows, never what a click on a note means.
            setVelocityLaneVisible (! showVelocityLane);
            repaint();
            return;
        }

        return;
    }

    // ---- Horizontal scrollbar ----------------------------------------------
    if (getScrollBarArea().contains (ipos))
    {
        const auto thumb = getScrollThumb();

        if (thumb.expanded (4.0f, 2.0f).contains (pos))
        {
            dragMode = DragMode::scrollThumb;
            scrollDragAnchorX = pos.x;
            scrollDragAnchorBeats = scrollBeats;
        }
        else
        {
            // Clicking the empty track jumps there, centring the thumb.
            const auto track = getScrollBarArea().reduced (2).toFloat();
            const double visible = getVisibleBeats();
            const float span = juce::jmax (1.0f, track.getWidth() - thumb.getWidth());
            const float t = juce::jlimit (0.0f, 1.0f,
                                          (pos.x - track.getX() - thumb.getWidth() * 0.5f) / span);
            setScrollBeats ((double) t * juce::jmax (0.0, timelineBeats - visible));
        }

        repaint();
        return;
    }

    // ---- Phase 91: the metre, in the ruler's corner ------------------------
    // Before the ruler and before the keys: the corner cell belongs to neither
    // (the ruler starts after the keyboard, the keys start below the ruler), and
    // it is the only pixel that means "how long is a bar".
    if (getMeterCornerArea().contains (ipos))
    {
        if (onMeterRequested != nullptr)
            onMeterRequested (localPointToGlobal (ipos));

        return;
    }

    // ---- Phase 97: the velocity lane ---------------------------------------
    // Before the keyboard, because the lane's gutter sits in the same column as
    // the piano keys: without this, a click on the word "力度" would audition a
    // pitch. The hand tool is exempt — panning the view is not an edit, and a
    // lane that swallowed the pan gesture would be a strip you cannot scroll
    // past.
    if (getVelocityLaneArea().contains (ipos))
    {
        if (tool == Tool::hand)
        {
            dragMode = DragMode::panView;
            panAnchor = pos;
            panAnchorScrollY = scrollY;
            panAnchorBeats = scrollBeats;
            return;
        }

        handleVelocityPress (pos, mods);
        return;
    }

    // ---- Phase 22: the piano column is a keyboard, not a graphic -----------
    // Before the ruler and the grid: the keys are outside both, and the old
    // code simply fell through to "not in the grid, ignore". A pitch grid you
    // cannot hear while pointing at it is a picture of a keyboard.
    if (getKeyboardArea().contains (ipos))
    {
        const int pitch = juce::jlimit (0, numPitches - 1, pitchAtScreenY ((float) pos.y));

        pressedKeyPitch = pitch;
        auditionPitchOnActiveLayer (pitch, true);
        repaint (getKeyboardArea());
        return;
    }

    // ---- Phase 18/19: the ruler is the transport's own strip ---------------
    // Left-drag scrubs the playhead; right-drag stretches the horizontal scale.
    // Both live on the same strip because that is how the users of this editor
    // expect a time axis to behave: pointing at a beat moves time there, and
    // "the thing that is scaled" is the scale itself, not the transport.
    if (getRulerArea().contains (ipos))
    {
        // Phase 87: the loop strip answers before the transport does. A press
        // here is about where the loop goes, never about where the playhead
        // goes — the two live on the same 32 pixels, and a gesture that could
        // mean either would be a gesture that means neither.
        if (getLoopBarArea().contains (ipos) && ! mods.isPopupMenu())
        {
            const double beat = beatAtScreenX (pos.x);

            loopDragAnchorBeat   = beat;
            loopDragAnchorLength = loopRegion.lengthBeats();
            loopDragOriginal     = loopRegion;

            const float startX = screenXForBeat (loopRegion.startBeat);
            const float endX   = screenXForBeat (loopRegion.endBeat);

            if (loopRegion.isUsable() && std::abs (pos.x - startX) <= ui::px (5))
                dragMode = DragMode::loopStart;
            else if (loopRegion.isUsable() && std::abs (pos.x - endX) <= ui::px (5))
                dragMode = DragMode::loopEnd;
            else if (loopRegion.isUsable() && loopRegion.contains (beat))
                dragMode = DragMode::loopMove;
            else
                dragMode = DragMode::loopNew;    // the region is built by the DRAG

            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        if (mods.isPopupMenu())
        {
            // Right button: zoom. Anchored on the value the drag started with,
            // so the scale cannot wind up or down as the mouse jitters.
            //
            // Phase 31: and anchored on the BEAT UNDER THE POINTER, which is the
            // point of the gesture. A right-drag on a ruler is "pull this moment
            // apart" — the moment under the mouse is the one that must not move.
            dragMode = DragMode::rulerZoom;
            rulerDragAnchorY      = pos.y;
            rulerDragAnchorBeatWidth = beatWidth;
            rulerZoomAnchorX      = pos.x;
            rulerZoomAnchorBeat   = beatAtScreenX (pos.x);
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        dragMode = DragMode::rulerScrub;
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

        // Phase 67: one function, so a click and a drag cannot disagree about
        // where the playhead is.
        scrubRulerTo (pos.x);

        return;
    }

    if (! getGridArea().contains (pos))
        return;

    //-- Phase 73: the second press of a double click on empty canvas ---------
    // mouseDoubleClick runs AFTER this, so without the early exit the pencil
    // would draw a note here and the double click would then close the group —
    // "get me out of here" leaving a note behind. Only genuinely empty space
    // counts: anything the double click would open must not be swallowed.
    if (clicks >= 2 && editingGroupId >= 0
          && hitTestActiveLayer (pos) == nullptr
          && hitTestSample (pos) == nullptr
          && getGroupLabelAt (pos) < 0
          && getGroupFrameAt (pos) < 0)
    {
        exitGroupEdit();
        return;
    }

    // ---- Phase 29: the way out of the spotlight ----------------------------
    // Checked before everything else in the grid: the chip is an override, and
    // a click that lands on it must not also move something out from under it.
    if (focusSlotId >= 0 && getSpotlightExitChipBounds().toFloat().contains (pos))
    {
        dismissSpotlight();
        return;
    }

    // ---- Phase 8: the inline automation box owns its own clicks ------------
    // Checked before the right-click bail-out, because right-clicking a point
    // in the box deletes it and right-clicking the menu opens the target menu.
    if (expandedNoteId >= 0)
    {
        if (auto* expanded = getExpandedNote())
        {
            if (getNoteAutomationBoxBounds (*expanded).contains (pos))
            {
                // Phase 22: a click inside a box (re)selects its curve, whether
                // that click lands on a point, the parameter cell or the frame.
                curveSelection = CurveSelection::noteEnvelope;
                handleAutomationMouseDown (pos, mods, *expanded);
                return;
            }
        }
        else
        {
            closeNoteAutomation();       // the note it pointed at is gone
        }
    }

    // ---- Phase 21: the group's inline curve box ----------------------------
    if (expandedGroupId >= 0)
    {
        if (auto* expanded = getExpandedGroup())
        {
            if (getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId)
                  .contains (pos))
            {
                curveSelection = CurveSelection::groupLane;
                handleGroupAutomationMouseDown (pos, mods);
                return;
            }
        }
        else
        {
            closeGroupAutomation();      // the group it pointed at is gone
        }
    }

    // ---- Phase 49: the clip's own curve box ---------------------------------
    if (expandedSampleId >= 0)
    {
        if (getExpandedSample() != nullptr)
        {
            if (getSampleAutomationBoxBounds (expandedSampleLayerIndex, expandedSampleId)
                  .contains (pos))
            {
                handleSampleAutomationMouseDown (pos, mods, expandedSampleLayerIndex, expandedSampleId);
                return;
            }
        }
        else
        {
            closeSampleAutomation();     // the block it pointed at is gone
        }
    }

    // Phase 22: anything else on the canvas deselects the curve. The boxes stay
    // OPEN — comparing two curves is a real workflow, and closing one on every
    // stray click would make it impossible.
    dropCurveSelection();

    // ---- Phase 21: the curve handle on a group's pill ----------------------
    // Before the label hit test: the handle sits inside the pill, so without
    // this the click would select the group and the box would never open.
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr || ! isLayerShown (i))
            continue;

        for (const auto& group : layer->getGroups())
        {
            if (! getGroupAutomationHandleBounds (i, group.getId()).contains (pos))
                continue;

            if (expandedGroupId == group.getId() && expandedGroupLayerIndex == i)
                closeGroupAutomation();
            else
                openGroupAutomation (i, group.getId(), getExpandedGroupTarget());

            return;
        }
    }

    // ---- Phase 16: a group's label is a click target of its own ------------
    // Checked before the notes because the pill sits above them: clicking the
    // name is the obvious way to say "the whole group", and it costs one hit
    // test rather than a modifier key nobody would guess.
    int labelOccurrence = 0;

    if (const int groupId = getGroupLabelAt (pos, &labelOccurrence); groupId >= 0)
    {
        const int layerIndex = layerIndexOfGroup (groupId);

        if (mods.isPopupMenu())
        {
            showGroupMenu (layerIndex, groupId, labelOccurrence, localPointToGlobal (pos.roundToInt()));
            return;
        }

        armGroupMove (groupId, labelOccurrence, pos, isAdditive (mods));
        return;
    }

    // ---- Phase 17: an audio clip under the cursor --------------------------
    // Before the notes: a block is the size of several lanes, so it is the
    // thing you are aiming at when both are under the pointer.
    if (auto* clipUnder = hitTestSample (pos))
    {
        const int clipId = clipUnder->id;

        //-- Phase 73: a block inside a CLOSED group answers as the group -------
        // A group is one object until it is opened: the press selects the whole
        // appearance and arms a rigid move of it. Without this, grabbing what
        // looks like one block took that one block out of formation — the group
        // was still on screen, still named, and its contents had quietly
        // scattered. The trim bands are locked too: "the group's block is
        // longer" is a shape edit, and shapes belong to all appearances.
        if (isGroupLocked (clipUnder->groupId))
        {
            if (mods.isPopupMenu())
            {
                showGroupMenu (stack.getActiveLayerIndex(), clipUnder->groupId,
                               clipUnder->groupOccurrence,
                               localPointToGlobal (pos.roundToInt()));
                return;
            }

            armGroupMove (clipUnder->groupId, clipUnder->groupOccurrence, pos,
                          isAdditive (mods));
            return;
        }

        // Inside the open group, or a stranger: a block that is not a member of
        // the group being edited means the user has moved on, so the group
        // closes here. Clicking OUT of a container leaves it — that is the same
        // rule as clicking out of a text box.
        if (editingGroupId >= 0 && clipUnder->groupId != editingGroupId)
            exitGroupEdit();

        //-- Phase 58: a SELECTED block's edge outranks the hit --------------
        // The hit test can only answer with ONE block, and with two blocks
        // overlapping the pixel you aimed at may belong to a stranger that is
        // drawn on top. "I selected three and dragged one edge" then became
        // "I dragged the stranger" — the trim looked like it failed.
        // The pointer's own cursor already says "trim band", so ask that
        // question first: if the edge belongs to something selected, that edge
        // is what the gesture is about, and the selection must survive.
        const auto edge      = clipEdgeAt (pos);
        const int  edgeId    = edge.first;
        const auto edgeKind  = edge.second;
        const int  grabbedId = edgeId >= 0 ? edgeId : clipId;

        // Phase 28: the click is also the selection — except on a trim grab
        // that belongs to an already-selected block, where rewriting the
        // selection would throw away exactly the blocks the trim is for.
        if (mods.isShiftDown() || mods.isCommandDown())
            toggleSampleSelection (clipId);
        else if (edgeId < 0 && ! isSampleSelected (clipId))
            setSampleSelection ({ clipId });

        auto* grabbed = stack.getActiveLayer() != nullptr
                          ? stack.getActiveLayer()->findSample (grabbedId)
                          : nullptr;

        if (grabbed == nullptr)
            return;

        if (mods.isPopupMenu())
        {
            showSampleMenu (grabbedId, localPointToGlobal (pos.roundToInt()));
            return;
        }

        if (tool == Tool::eraser)
        {
            if (auto* activeLayer = stack.getActiveLayer())
            {
                activeLayer->removeSample (grabbedId);
                sampleSelection.erase (std::remove (sampleSelection.begin(),
                                                    sampleSelection.end(), grabbedId),
                                       sampleSelection.end());
                pruneEmptyGroups();
                stack.notifyNotesChanged();
            }

            return;
        }

        if (tool == Tool::knife)
        {
            // Phase 51: cut HERE — where the crosshair points, snapped when the
            // magnet is on (a cut between grid lines sounds like an edit, a cut
            // on the grid sounds like music). The two halves become the
            // selection, which is the cut's own feedback.
            const double at = beatAtScreenX (pos.x);

            splitSamplesAt ({ grabbedId }, snapEnabled ? snapBeat (at) : at);
            return;
        }

        // Phase 72: four edges, one answer. A block that was just selected by
        // this very press has no entry in the selection hit test yet, so the
        // band is computed from the block the press landed on.
        const auto bounds    = getSampleScreenBounds (stack.getActiveLayerIndex(), grabbedId);
        const ClipEdge edgeUnder = edgeId >= 0 ? edgeKind : edgeOfClip (bounds, pos);

        // Phase 49: the chevron on the block opens its curve box. Checked
        // before the trim bands because it sits between them — the same order
        // the note's tail button answers in.
        const auto handle = getSampleAutomationHandleBounds (*grabbed);

        if (! handle.isEmpty() && handle.contains (pos))
        {
            if (expandedSampleId == grabbedId && expandedSampleLayerIndex == stack.getActiveLayerIndex())
                closeSampleAutomation();
            else
                openSampleAutomation (stack.getActiveLayerIndex(), grabbedId);

            return;
        }

        // Phase 28/29: a grab on a block's edge is a trim of the SELECTION
        // (Phase 56 widened it from "the grabbed block only"); a grab anywhere
        // else arms a RIGID move of the whole selection, clips and notes
        // together.
        if ((edgeUnder == ClipEdge::tail || edgeUnder == ClipEdge::head)
              && isSampleSelected (grabbedId))
        {
            dragSampleId       = grabbedId;
            dragSampleOriginal = *grabbed;
            dragAnchorBeat     = beatAtScreenX (pos.x);
            dragMode           = edgeUnder == ClipEdge::tail ? DragMode::resizeSample
                                                             : DragMode::resizeSampleLeft;

            // Phase 56: the trim is a selection-wide edit. Capture the other
            // blocks now, while they are still the shapes the user sees; the
            // drag then applies one delta to all of them.
            dragTrimOriginals.clear();

            if (sampleSelection.size() > 1)
                if (auto* activeLayer = stack.getActiveLayer())
                    for (int id : sampleSelection)
                        if (const auto* other = activeLayer->findSample (id))
                            dragTrimOriginals.push_back (*other);
        }
        else if ((edgeUnder == ClipEdge::top || edgeUnder == ClipEdge::bottom)
                   && isSampleSelected (grabbedId))
        {
            // Phase 72: the two horizontal edges change how many pitch lanes the
            // block covers — the same selection-wide rule as the trim, with the
            // same one-delta arithmetic (see applySampleSpanDrag).
            dragSampleId       = grabbedId;
            dragSampleOriginal = *grabbed;
            dragAnchorBeat     = beatAtScreenX (pos.x);
            dragMode           = edgeUnder == ClipEdge::top ? DragMode::resizeSampleTop
                                                            : DragMode::resizeSampleBottom;

            dragSpanOriginals.clear();

            if (sampleSelection.size() > 1)
                if (auto* activeLayer = stack.getActiveLayer())
                    for (int id : sampleSelection)
                        if (const auto* other = activeLayer->findSample (id))
                            dragSpanOriginals.push_back (*other);
        }
        else
        {
            armSampleMove (pos);
        }

        repaint();
        return;
    }

    //-- Phase 73: a closed group answers for ALL of its members ---------------
    // Placed before the note's chevron, before the canvas menu, and before the
    // knife / eraser / note body, because "only drag and select" has to hold for
    // every tool and every button: the same note must not be untouchable with
    // the selector and one click away from being cut in half with the knife.
    // The press becomes the group's own grab — select the appearance, arm the
    // rigid move — which is exactly what the pill and the bracket already do,
    // so there is one gesture here and not three.
    //
    // The hand tool is exempt: panning the view is not an edit.
    if (tool != Tool::hand)
    {
        if (auto* member = hitTestActiveLayer (pos))
        {
            if (isGroupLocked (member->groupId))
            {
                if (mods.isPopupMenu())
                    showGroupMenu (stack.getActiveLayerIndex(), member->groupId,
                                   member->groupOccurrence,
                                   localPointToGlobal (pos.roundToInt()));
                else
                    armGroupMove (member->groupId, member->groupOccurrence, pos,
                                  isAdditive (mods));

                return;
            }

            // A note that is NOT in the open group means the user has left it.
            if (editingGroupId >= 0 && member->groupId != editingGroupId)
                exitGroupEdit();
        }
    }

    if (mods.isPopupMenu())
    {
        // Right-click anywhere else: the group actions for whatever the
        // selection currently is. Before Phase 16 this click was simply ignored.
        showCanvasMenu (localPointToGlobal (pos.roundToInt()));
        return;
    }

    // ---- Phase 8: the "v" handle at the note's tail -------------------------
    if (auto* noteUnderMouse = hitTestActiveLayer (pos))
    {
        const auto handle = getNoteAutomationHandleBounds (*noteUnderMouse);

        if (! handle.isEmpty() && handle.contains (pos))
        {
            const int id = noteUnderMouse->id;

            if (id == expandedNoteId && expandedLayerIndex == stack.getActiveLayerIndex())
                closeNoteAutomation();
            else
                openNoteAutomation (stack.getActiveLayerIndex(), id);

            return;
        }
    }

    // Any other click in the canvas dismisses the box, like a popover.
    if (expandedNoteId >= 0)
        closeNoteAutomation();

    if (expandedGroupId >= 0)
        closeGroupAutomation();

    if (expandedSampleId >= 0)
        closeSampleAutomation();

    // ---- Hand tool: drag to pan --------------------------------------------
    if (tool == Tool::hand)
    {
        dragMode = DragMode::panView;
        panAnchor = pos;
        panAnchorScrollY = scrollY;
        panAnchorBeats = scrollBeats;
        return;
    }

    // ---- Phase 51: the knife cuts notes too --------------------------------
    // One tool, two materials, one verb: click a note and it becomes two notes
    // at the click, the same strictly-inside rule the block's cut uses. Both
    // halves keep the group (a slice through a group member makes two members,
    // not one member and one stranger) and the curve is split at the seam, so
    // a ramp stays a ramp.
    if (tool == Tool::knife)
    {
        if (auto* hit = hitTestActiveLayer (pos))
        {
            const double at = beatAtScreenX (pos.x);

            splitNotesAt ({ hit->id }, snapEnabled ? snapBeat (at) : at);
            return;
        }

        return;     // a knife click on empty canvas cuts nothing
    }

    // ---- Eraser: one click, one note ---------------------------------------
    if (tool == Tool::eraser)
    {
        if (auto* hit = hitTestActiveLayer (pos))
        {
            if (auto* active = stack.getActiveLayer())
            {
                active->removeNote (hit->id);
                setSelection ({});
                hoveredNoteId = -1;
                pruneEmptyGroups();
                stack.notifyNotesChanged();
            }
        }

        return;
    }

    // ---- Pencil: click empty grid to draw a note ---------------------------
    if (tool == Tool::pencil && hitTestActiveLayer (pos) == nullptr)
    {
        // Phase 11: the note is stamped, not invented — if a state was copied
        // off another note it carries that length, velocity, curve and synth.
        if (const int newId = addNoteAt (beatAtScreenX (pos.x),
                                          pitchAtScreenY (pos.y));
            newId > 0)
        {
            setSelection ({ newId });

            // Phase 19: hear what you just drew. Read back through findNote so
            // the audition reports the pitch the model actually stored (clamped,
            // snapped) rather than the raw mouse position.
            if (auto* active = stack.getActiveLayer())
                if (const auto* drawn = active->findNote (newId))
                    auditionNoteOnActiveLayer (*drawn, true);

            stack.notifyNotesChanged();
        }

        return;
    }

    // ---- Is there an editable note under the cursor? -------------------------
    if (auto* hit = hitTestActiveLayer (pos))
    {
        const int  hitId = hit->id;
        const int  hitPitch = hit->pitch;
        const auto noteBounds = getNoteBounds (*hit);
        const float band = ui::pf (kResizeHandleWidth);

        // Phase 19: both ends resize. The right end keeps priority when the note
        // is short enough that the two bands would overlap.
        const bool onRightEdge = pos.x > noteBounds.getRight() - band;
        const bool onLeftEdge  = ! onRightEdge && pos.x < noteBounds.getX() + band;

        if (mods.isShiftDown() || mods.isCommandDown())
            toggleSelection (hitId);
        else if (! isSelected (hitId))
            setSelection ({ hitId });

        // Alt-drag: leave the original where it is and drag a copy — which
        // carries the envelope, because the copy is a whole NoteEvent.
        if (mods.isAltDown() && ! onRightEdge && ! onLeftEdge)
        {
            const auto copies = duplicateSelectedNotes (0.0);

            if (! copies.empty())
                setSelection (copies);
        }

        if (isSelected (hitId))
        {
            dragMode = onRightEdge ? DragMode::resizeNote
                       : (onLeftEdge ? DragMode::resizeNoteLeft : DragMode::moveNote);
            dragAnchorBeat = beatAtScreenX (pos.x);
            dragAnchorPitch = pitchAtScreenY (pos.y);
            dragAuditionPitch = hitPitch;

            // Phase 25: only the grabbed note resizes. The selection may be a
            // whole group, but "make this one longer" is about the note whose
            // edge is under the pointer — resizing the selection turned every
            // member's tail into one line the moment you touched either end.
            dragResizeNoteId = hitId;
            captureEditOriginals();     // Phase 29: clips travel with the notes
        }

        return;
    }

    // ---- Phase 25: the group's frame is the group's handle ------------------
    // Notes were already hit-tested above, so reaching here means the pointer is
    // on the empty part of a group's bracket — the exact place a user aims at
    // when they mean "this whole thing". Before this the click fell through to
    // the marquee, which deselected the group: the bracket announced an object
    // that could not be moved.
    //
    // Shift is exempt on purpose: additive marquee INSIDE a group still has to
    // work, and that is what Shift already means on empty canvas.
    if (! mods.isShiftDown())
    {
        int frameOccurrence = 0;

        if (const int frameGroup = getGroupFrameAt (pos, &frameOccurrence); frameGroup >= 0)
        {
            // Phase 75: inside the OPEN group the frame is not a handle — it is
            // just the patch of canvas between the members. A click there has to
            // mean what a click on empty canvas means everywhere else: nothing
            // is selected. It falls through to the marquee below instead of
            // arming a rigid move of a group the user is already inside.
            if (frameGroup != editingGroupId)
            {
                armGroupMove (frameGroup, frameOccurrence, pos);
                return;
            }
        }
    }

    // ---- Empty grid: marquee over the ACTIVE layer only ----------------------
    // Phase 30: and the grid itself ends the spotlight. "Click somewhere that is
    // not a note" is the gesture everyone tries first. Checked HERE, after every
    // hit test, so clicking a note or a clip inside the highlight never
    // dismisses it.
    dismissSpotlight();

    // Phase 74: Shift/Ctrl is "and also", for the band as well as for a click.
    // It has to be remembered here — by the release the modifier is long gone,
    // and a sweep that silently replaced the selection made Shift look broken.
    marqueeAdditive = isAdditive (mods);

    if (! marqueeAdditive)
    {
        selection.clear();
        sampleSelection.clear();     // Phase 28: clicking the grid lets go of both
        notifySelectionChanged();
    }

    dragMode = DragMode::marquee;
    marqueeStart = marqueeCurrent = pos;
    repaint();
}


void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    handleDoubleClick (e.getPosition().toFloat());
}


void PianoRollComponent::handleDoubleClick (juce::Point<float> pos)
{
    // Phase 87: the loop strip, before the grid test below. A double click on
    // the region is the only way to switch it off — and switching it OFF has to
    // keep the two numbers, so this is a flag change plus a report, not a
    // rebuild of the region.
    if (getLoopBarArea().toFloat().contains (pos))
    {
        const auto before = loopRegion;

        loopRegion.enabled = ! loopRegion.enabled;

        if (loopRegion.enabled && ! loopRegion.isUsable())
            loopRegion = LoopRegion::around (snapBeat (beatAtScreenX (pos.x)));

        repaint (getRulerArea());

        if (loopRegion != before && onLoopRegionChanged != nullptr)
            onLoopRegionChanged (loopRegion);

        return;
    }

    if (! getGridArea().contains (pos))
        return;

    //-- Phase 73: 双击 = 打开组。空白处双击 = 关掉它 ------------------------
    // The order is the whole behaviour: members first (open or switch), then the
    // group's own frame and pill, then — only if nothing at all is under the
    // pointer — "close". A double click on a note inside the open group keeps
    // its old meaning (open that note), because "open what is already open" is
    // not a second gesture, it is the gesture that was there before.
    //-- Phase 76: opening is not selecting ---------------------------------
    // A click on a closed group already answered "the whole appearance" (P73),
    // so the press that opened the double click left every member in the
    // selection. Rewriting the selection to the one member under the pointer
    // then threw that away — and the next height or length drag moved that one
    // block while its group's copies, which the shape rule does propagate,
    // still followed. The user saw "I had several selected, only one moved,
    // and only after selecting again did the rest keep up".
    //
    // Same rule the group's pill got in P65: a gesture that lands on something
    // already selected must not replace what is selected. A double click on a
    // member OUTSIDE the selection still narrows to it, which is how "open this
    // one and start here" keeps working.
    if (auto* clip = hitTestSample (pos))
    {
        if (clip->groupId >= 0 && clip->groupId != editingGroupId)
        {
            enterGroupEdit (clip->groupId);

            if (! isSampleSelected (clip->id))
                setSampleSelection ({ clip->id });

            return;
        }
    }

    if (auto* hit = hitTestActiveLayer (pos))
    {
        if (hit->groupId >= 0 && hit->groupId != editingGroupId)
        {
            enterGroupEdit (hit->groupId);

            if (! isSelected (hit->id))
                setSelection ({ hit->id });

            return;
        }

        dragMode = DragMode::none;
        dragOriginals.clear();
        dragResizeNoteId = -1;

        if (onNoteDoubleClicked != nullptr)
            onNoteDoubleClicked (stack.getActiveLayerIndex(), hit->id);

        return;
    }

    //-- Phase 78: double click a block = open its envelope window -----------
    // The chevron in the block's top-right corner opens the inline curve box
    // (Phase 49) — a strip for a quick tweak while arranging. A double click
    // asks for the other thing: the note panel's twin, a window that says what
    // this block is, which of its two curves you have, where it goes, and has
    // room for the shape itself. Two editors for one curve, at two levels of
    // commitment.
    //
    // After the note test, not before, because that is the painter's order:
    // blocks go under the notes (drawSamples, then drawNotes). A note drawn
    // over a block is what the pointer is looking at, so it answers first.
    // Only where the block is the topmost thing does the double click reach
    // it — which is exactly where it looks like it should.
    //
    // A grouped member never gets here while its group is closed: the branch
    // above turns that double click into "let me inside" (P73), and opening an
    // editor on the way in would drop a window in front of the thing you asked
    // to reach. Once inside the group the double click lands here.
    if (auto* clip = hitTestSample (pos))
    {
        // The inline box and the window show the same curve. Leaving both up
        // would be two editors disagreeing about which one is authoritative, so
        // opening the window shuts the strip first.
        closeSampleAutomation();

        if (onSampleDoubleClicked != nullptr)
            onSampleDoubleClicked (stack.getActiveLayerIndex(), clip->id);

        return;
    }

    // The bracket and the name tag are the group too: double-clicking the pill
    // is what a user tries when they want "inside this thing".
    int occurrence = 0;

    if (const int frameGroup = getGroupFrameAt (pos, &occurrence); frameGroup >= 0)
    {
        if (frameGroup != editingGroupId)
            enterGroupEdit (frameGroup);

        return;
    }

    if (const int labelGroup = getGroupLabelAt (pos, &occurrence); labelGroup >= 0)
    {
        if (labelGroup != editingGroupId)
            enterGroupEdit (labelGroup);

        return;
    }

    //-- Phase 77: double click on a ghost = go and edit it -------------------
    // The ghost is click-through on the press (P1/P20), and stays that way: a
    // sheet you are not editing is not something a click can grab. A double
    // click is a different verb though — "take me there" — and over there it has
    // no other meaning, because the pixel is empty on the sheet you are looking
    // at. Checked after EVERY hit test on the active sheet, which is the order
    // the painter uses: ghosts go underneath all of it.
    if (const auto ghost = hitTestGhost (pos); ghost.layerIndex >= 0)
    {
        // Switching sheets is the whole jump. layersChanged() closes the open
        // group and drops the old selection there — both belonged to the sheet
        // we are leaving — so nothing below can fight with either.
        stack.setActiveLayerIndex (ghost.layerIndex);

        auto* layer = stack.getLayer (ghost.layerIndex);

        if (layer == nullptr)
            return;

        if (ghost.isSample)
        {
            if (const auto* clip = layer->findSample (ghost.itemId))
            {
                // A grouped block answers as its whole appearance (P74), and the
                // group is opened so the very next drag edits it instead of
                // pushing the whole thing around (P73). Same gesture, same answer
                // as a double click on the sheet it lives on.
                if (clip->groupId >= 0)
                {
                    selectGroup (clip->groupId, clip->groupOccurrence);
                    enterGroupEdit (clip->groupId);
                }
                else
                {
                    setSampleSelection ({ clip->id });

                    //-- Phase 78: and open its envelope window --------------
                    // The half of the gesture that would have happened on that
                    // sheet: an ungrouped block's double click opens its curve
                    // editor, so the jump does the same instead of just landing
                    // you there.
                    closeSampleAutomation();

                    if (onSampleDoubleClicked != nullptr)
                        onSampleDoubleClicked (ghost.layerIndex, clip->id);
                }
            }
        }
        else if (const auto* note = layer->findNote (ghost.itemId))
        {
            if (note->groupId >= 0)
            {
                selectGroup (note->groupId, note->groupOccurrence);
                enterGroupEdit (note->groupId);
            }
            else
            {
                setSelection ({ note->id });

                // The double click that would have happened on that sheet, made
                // here: an ungrouped note's double click opens its envelope
                // inspector. Doing only half of it would make the same gesture
                // on the same note mean two different things depending on which
                // sheet you happened to be standing on — and the sheet you were
                // on is exactly what this gesture is about.
                if (onNoteDoubleClicked != nullptr)
                    onNoteDoubleClicked (ghost.layerIndex, note->id);
            }
        }

        repaint();
        return;
    }

    // Nothing under the pointer. Before Phase 73 this was simply "nothing
    // happens"; now it is the way out of a group that has been opened.
    if (editingGroupId >= 0)
        exitGroupEdit();
}


void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    // ---- Phase 22: glissando along the keys --------------------------------
    // A key press is a held gesture here, not a one-shot: dragging down the
    // column sounds each pitch it crosses, which is the only way to find the
    // note you want before you commit it to the grid.
    if (pressedKeyPitch >= 0 && getKeyboardArea().contains (e.getPosition()))
    {
        const int pitch = juce::jlimit (0, numPitches - 1, pitchAtScreenY (e.position.y));

        if (pitch != pressedKeyPitch)
        {
            pressedKeyPitch = pitch;
            auditionPitchOnActiveLayer (pitch, true);
            repaint (getKeyboardArea());
        }

        return;
    }

    if (dragMode == DragMode::rulerScrub)
    {
        // Left-drag on the ruler is the transport: the playhead follows the
        // pointer, one locate per move. Snapping applies here exactly as it does
        // to a click, so a drag can still land on the 1/16 grid.
        //
        // Phase 67: and when the pointer is outside the grid, the window comes
        // with it — see scrubRulerTo.
        scrubRulerTo (e.position.x);
        return;
    }

    //-- Phase 87: the loop strip ---------------------------------------------
    if (dragMode == DragMode::loopStart || dragMode == DragMode::loopEnd
          || dragMode == DragMode::loopMove || dragMode == DragMode::loopNew)
    {
        applyLoopDragToScreenX (e.position.x);
        return;
    }

    if (dragMode == DragMode::rulerZoom)
    {
        // Right-drag. Up widens the beat, i.e. zooms in. Exponential so the same
        // drag distance does the same musical thing at every zoom level.
        // Phase 31: pinned on the column and the beat the drag started from.
        const float dy = rulerDragAnchorY - e.position.y;

        setBeatWidthAround ((float) (rulerDragAnchorBeatWidth * std::pow (1.006, (double) dy)),
                            rulerZoomAnchorBeat, rulerZoomAnchorX);
        return;
    }

    if (dragMode == DragMode::scrollThumb)
    {
        const auto track   = getScrollBarArea().reduced (2).toFloat();
        const auto thumb   = getScrollThumb();
        const double visible = getVisibleBeats();
        const float  span  = juce::jmax (1.0f, track.getWidth() - thumb.getWidth());

        setScrollBeats (scrollDragAnchorBeats
                          + (double) (e.position.x - scrollDragAnchorX) / span
                              * juce::jmax (0.0, timelineBeats - visible));
        return;
    }

    if (dragMode == DragMode::panView)
    {
        const float maxScrollY = juce::jmax (0.0f, (float) numPitches * keyHeight - getGridArea().getHeight());

        setScrollBeats (panAnchorBeats - (double) (e.position.x - panAnchor.x) / beatWidth);
        scrollY = juce::jlimit (0.0f, maxScrollY, panAnchorScrollY - (e.position.y - panAnchor.y));
        repaint();
        return;
    }

    // Phase 21: the group box is owned by whichever layer the group lives on,
    // which need not be the active one — so this branch runs before the "no
    // active layer, nothing to drag" guard below.
    if (dragMode == DragMode::groupAutomationPoint)
    {
        dragGroupCurvePoint (e.position);
        return;
    }

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    if (dragMode == DragMode::automationPoint)
    {
        if (auto* note = getExpandedNote())
            dragAutomationPoint (*note, e.position);

        return;
    }

    switch (dragMode)
    {
        case DragMode::moveNote:
        {
            // Phase 22: the drag is a RIGID move — the clamp lives in
            // clampMove() and is applied to the DELTA, not to each note. Per
            // note clamping meant the leftmost note stopped dead against beat 0
            // while its neighbours kept sliding left, so the intervals the user
            // was dragging as one object collapsed without a word.
            const auto move = clampMove (dragOriginals,
                                         beatAtScreenX (e.position.x) - dragAnchorBeat,
                                         pitchAtScreenY (e.position.y) - dragAnchorPitch,
                                         snapEnabled,
                                         dragClipOriginals);

            applyMove (move);

            // Phase 19: the pitch you are dragging onto has to sound — you are
            // choosing it by ear. Once per new pitch, not once per mouse move,
            // and never for a purely horizontal drag.
            if (! dragOriginals.empty())
            {
                const auto& first = dragOriginals.front();

                if (const auto* moved = active->findNote (first.id);
                    moved != nullptr && moved->pitch != dragAuditionPitch)
                {
                    dragAuditionPitch = moved->pitch;
                    auditionNoteOnActiveLayer (*moved, false);
                }
            }

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::resizeNote:
        {
            const double rawX      = beatAtScreenX (e.position.x);
            const double wantedEnd = snapEnabled ? snapBeat (rawX) : rawX;
            const int    target    = resizeTargetId();

            for (const auto& origin : dragOriginals)
                if (origin.id == target)
                {
                    applyNoteResize (origin, wantedEnd, true);
                    break;
                }

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::resizeNoteLeft:
        {
            // The tail stays where it is and the head moves. That is what makes
            // "trim the front" possible without also re-typing the length: the
            // note the user sees end at beat 8 still ends at beat 8 afterwards.
            const double rawX        = beatAtScreenX (e.position.x);
            const double wantedStart = snapEnabled ? snapBeat (rawX) : rawX;
            const int    target      = resizeTargetId();

            for (const auto& origin : dragOriginals)
                if (origin.id == target)
                {
                    applyNoteResize (origin, wantedStart, false);
                    break;
                }

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::resizeSample:
        case DragMode::resizeSampleLeft:
        {
            // Phase 29: both ends trim, through ONE function. Two edges that
            // each computed a length would eventually disagree about which end
            // moves, and the block would jump when you switched sides.
            const double rawX   = beatAtScreenX (e.position.x);
            const double wanted = snapEnabled ? snapBeat (rawX) : rawX;
            const bool   fromTail = dragMode == DragMode::resizeSample;

            applySampleTrimDrag (dragSampleOriginal, wanted, fromTail, dragTrimOriginals);

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::sampleCurvePoint:
            // The point lives in the box that opened it; the drag writes
            // through the same path a click on the curve does.
            dragSampleCurvePoint (expandedSampleLayerIndex, expandedSampleId, e.position);
            break;

        case DragMode::resizeSampleTop:
        case DragMode::resizeSampleBottom:
        {
            // Phase 72: the pointer's ROW is the edge, so the block grows and
            // shrinks in whole lanes as the mouse crosses them — the same
            // "snap to the thing you are aiming at" a note's resize gets from
            // the grid.
            applySampleSpanDrag (dragSampleOriginal, e.position.y,
                                 dragMode == DragMode::resizeSampleTop,
                                 dragSpanOriginals);

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::marquee:
            marqueeCurrent = e.position;
            repaint();
            break;

        case DragMode::velocityDrag:
            // Phase 97: the one shared arithmetic, then the one notification.
            // The notes themselves are written by `applyVelocityDragToScreenY`,
            // so a fixture that calls it and the mouse both end up in the same
            // place.
            applyVelocityDragToScreenY (e.position.y);
            stack.notifyNotesChanged();
            break;

        case DragMode::none:
        default:
            break;
    }
}


void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    // Phase 22: the key lights up for as long as it is held. The sound has its
    // own envelope and finishes on its own — a held key in this editor has no
    // note-off to send because it never made a note.
    if (pressedKeyPitch >= 0)
    {
        pressedKeyPitch = -1;
        repaint (getKeyboardArea());
    }

    const bool wasRulerDrag = dragMode == DragMode::rulerScrub
                                || dragMode == DragMode::rulerZoom;

    // Phase 87: a loop gesture reports itself ONCE, on release, for the same
    // reason a note drag does — the host turns one report into one undo step,
    // and a report per mouse move would turn a drag into sixty undos.
    const bool wasLoopDrag = finishLoopGesture();     // reports, if anything moved

    // Phase 21: one undo step per gesture, not per mouse-move — the point drag
    // itself only republishes (see dragGroupCurvePoint).
    const bool wasGroupCurveDrag = dragMode == DragMode::groupAutomationPoint;
    const bool wasSampleCurveDrag = dragMode == DragMode::sampleCurvePoint;

    //-- Phase 62: what this gesture touched, read while it is still here -----
    // A drag that changes a group member changes the group's SHAPE, and every
    // other appearance of that group has to follow. Collected before the state
    // below is cleared, and applied before the gesture ends so the edit is one
    // undo step instead of two.
    std::vector<int> touchedNotes, touchedClips;

    for (const auto& note : dragOriginals)
        touchedNotes.push_back (note.id);

    for (const auto& clip : dragClipOriginals)
        touchedClips.push_back (clip.id);

    // The inline boxes edit ONE member without arming a drag: a note's envelope
    // handle and a clip's volume/pan handle are both shape, both have to travel.
    if (expandedNoteId >= 0)
        touchedNotes.push_back (expandedNoteId);

    if (expandedSampleId >= 0)
        touchedClips.push_back (expandedSampleId);

    const auto touchedOccurrences = groupOccurrencesOf (touchedNotes, touchedClips);

    if (dragMode == DragMode::marquee)
    {
        auto marquee = juce::Rectangle<float> (marqueeStart, marqueeCurrent).getSmallestIntegerContainer().toFloat();

        if (marquee.getWidth() > 3.0f || marquee.getHeight() > 3.0f)
            if (marqueeStart.getDistanceFrom (marqueeCurrent) > 0.0f)
                simulateMarquee (marquee);

        marqueeAdditive = false;      // the band is over; the next one decides again
        marqueeInLane   = false;      // Phase 97: same, for which surface it swept
    }

    dragMode = DragMode::none;
    dragOriginals.clear();
    dragClipOriginals.clear();     // Phase 28: armed by a clip move as well
    dragTrimOriginals.clear();     // Phase 56: armed by a clip trim as well
    dragSpanOriginals.clear();     // Phase 72: armed by a clip height drag
    velocityOriginals.clear();     // Phase 97: armed by a velocity bar drag
    dragResizeNoteId = -1;
    dragEnvelopePoint = -1;
    dragSampleId = -1;
    dragGroupPoint = -1;        // Phase 21: the group box's handle, if any
    dragSampleCurveIndex = -1;  // Phase 49: the clip box's handle, if any

    if (wasGroupCurveDrag || wasSampleCurveDrag)
        stack.notifyNotesChanged();     // the whole drag, as one history step

    // Phase 62: and the shape edit lands on every other appearance of the group.
    syncGroupShapesAfterEdit (touchedOccurrences);

    // The ruler borrows the vertical-resize pointer while it is being dragged;
    // letting the tool take it back is the only way out of that state.
    if (wasRulerDrag || wasLoopDrag)
        updateCursor();

    repaint();
}


//==============================================================================
//  Phase 87: the loop gestures, as functions
//==============================================================================
void PianoRollComponent::applyLoopDragToScreenX (float screenX)
{
    // Snapped like everything else on this grid, and always rebuilt from the
    // ORIGINAL region rather than nudged: a move that accumulated deltas would
    // drift by the rounding error of every mouse move.
    if (dragMode == DragMode::loopStart)
    {
        loopRegion.setStart (snapBeat (beatAtScreenX (screenX)));
    }
    else if (dragMode == DragMode::loopEnd)
    {
        loopRegion.setEnd (snapBeat (beatAtScreenX (screenX)));
    }
    else if (dragMode == DragMode::loopMove)
    {
        loopRegion = loopDragOriginal;
        loopRegion.moveBy (snapBeat (beatAtScreenX (screenX)) - snapBeat (loopDragAnchorBeat));
    }
    else if (dragMode == DragMode::loopNew)
    {
        loopRegion.setRange (loopDragAnchorBeat, snapBeat (beatAtScreenX (screenX)));

        // Dragging a region out is the request itself. Requiring a second click
        // to switch on the thing you just drew would be a rule nobody could
        // guess from the picture.
        loopRegion.enabled = true;
    }

    repaint (getRulerArea());
}

bool PianoRollComponent::finishLoopGesture()
{
    if (dragMode != DragMode::loopStart && dragMode != DragMode::loopEnd
          && dragMode != DragMode::loopMove && dragMode != DragMode::loopNew)
        return false;

    // Reported only when something actually changed. A press that never dragged
    // is a click, and a click in the strip means "start here next time", not
    // "I have edited the project".
    if (loopRegion != loopDragOriginal && onLoopRegionChanged != nullptr)
        onLoopRegionChanged (loopRegion);

    return true;
}

void PianoRollComponent::simulateLoopGestureForTest (float downX, float dragX)
{
    const auto bar = getLoopBarArea();

    handlePress ({ downX, (float) bar.getCentreY() }, {}, 1);
    applyLoopDragToScreenX (dragX);
    finishLoopGesture();

    dragMode = DragMode::none;
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    int newTool = -1;

    if (getToolbarArea().contains (e.getPosition()))
        for (int i = 0; i < numToolButtons; ++i)
            if (getToolButtonBounds (i).contains (e.getPosition()))
                newTool = i;

    if (newTool != hoveredTool)
    {
        hoveredTool = newTool;
        repaint (getToolbarArea());
    }

    if (getToolbarArea().contains (e.getPosition()) || getScrollBarArea().contains (e.getPosition()))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        return;
    }

    if (getRulerArea().contains (e.getPosition()))
    {
        // Left-drag scrubs time, right-drag zooms; the pointer says which one
        // the next gesture will be, so neither is a guess.
        setMouseCursor (juce::ModifierKeys::getCurrentModifiers().isPopupMenu()
                          ? juce::MouseCursor::UpDownResizeCursor
                          : juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    //-- Phase 8: the inline box ---------------------------------------------
    if (expandedNoteId >= 0)
    {
        if (auto* expanded = getExpandedNote())
        {
            const auto box = getNoteAutomationBoxBounds (*expanded);

            if (box.contains (e.position))
            {
                // Phase 82: which of the two cells, not "somewhere on the row".
                // Two adjacent buttons that both light up would be a hover that
                // lies about what the click is aimed at.
                const int overMenu = getAutomationCurveMenuArea (*expanded).contains (e.position) ? 1
                                   : getAutomationRouteMenuArea (*expanded).contains (e.position) ? 2 : 0;
                const int  overPoint = getAutomationCurveArea (*expanded).contains (e.position)
                                         ? findEnvelopePointAt (*expanded, e.position) : -1;

                if (overMenu != hoveredAutomationMenu || overPoint != hoveredEnvelopePoint)
                {
                    hoveredAutomationMenu  = overMenu;
                    hoveredEnvelopePoint   = overPoint;
                    repaint (box.expanded (6.0f).getSmallestIntegerContainer());
                }

                setMouseCursor (overMenu != 0 ? juce::MouseCursor::PointingHandCursor
                                              : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                                : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredAutomationMenu != 0 || hoveredEnvelopePoint != -1)
    {
        hoveredAutomationMenu = 0;
        hoveredEnvelopePoint  = -1;
        repaint();
    }

    //-- Phase 21: the group's inline box ------------------------------------
    if (expandedGroupId >= 0)
    {
        if (auto* expanded = getExpandedGroup())
        {
            const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);

            if (box.contains (e.position))
            {
                const int overMenu = getGroupCurveMenuArea().contains (e.position) ? 1
                                   : getGroupRouteMenuArea().contains (e.position) ? 2 : 0;
                const int  overPoint = getGroupCurveBounds().contains (e.position)
                                         ? findGroupCurvePointAt (e.position) : -1;

                if (overMenu != hoveredGroupMenu)
                {
                    hoveredGroupMenu = overMenu;
                    repaint (box.expanded (6.0f).getSmallestIntegerContainer());
                }

                setMouseCursor (overMenu != 0 ? juce::MouseCursor::PointingHandCursor
                                              : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                                : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredGroupMenu != 0)
    {
        hoveredGroupMenu = 0;
        repaint();
    }

    //-- Phase 49: the clip's own box ----------------------------------------
    if (expandedSampleId >= 0)
    {
        if (getExpandedSample() != nullptr)
        {
            const auto box = getSampleAutomationBoxBounds (expandedSampleLayerIndex,
                                                           expandedSampleId);

            if (box.contains (e.position))
            {
                const int   overMenu  = getSampleCurveMenuArea (expandedSampleLayerIndex,
                                                                expandedSampleId).contains (e.position) ? 1
                                      : getSampleRouteMenuArea (expandedSampleLayerIndex,
                                                                expandedSampleId).contains (e.position) ? 2
                                      : 0;
                const int   overPoint = getSampleCurveArea (expandedSampleLayerIndex,
                                                            expandedSampleId).contains (e.position)
                                          ? findSampleCurvePointAt (expandedSampleLayerIndex,
                                                                    expandedSampleId, e.position)
                                          : -1;

                if (overMenu != hoveredSampleCurveMenu || overPoint != hoveredSampleCurvePoint)
                {
                    hoveredSampleCurveMenu  = overMenu;
                    hoveredSampleCurvePoint = overPoint;
                    repaint (box.expanded (6.0f).getSmallestIntegerContainer());
                }

                setMouseCursor (overMenu != 0 ? juce::MouseCursor::PointingHandCursor
                                              : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                                : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredSampleCurveMenu != 0 || hoveredSampleCurvePoint != -1)
    {
        hoveredSampleCurveMenu  = 0;
        hoveredSampleCurvePoint = -1;
        repaint();
    }

    //-- Phase 21: the curve handle on a group's pill ------------------------
    {
        bool overHandle = false;
        int  handleGroup = -1;
        int  handleLayer = -1;

        for (int i = 0; i < stack.getNumLayers() && ! overHandle; ++i)
        {
            auto* layer = stack.getLayer (i);

            if (layer == nullptr || ! isLayerShown (i))
                continue;

            for (const auto& group : layer->getGroups())
                if (getGroupAutomationHandleBounds (i, group.getId()).contains (e.position))
                {
                    overHandle  = true;
                    handleGroup = group.getId();
                    handleLayer = i;
                    break;
                }
        }

        if (overHandle != hoveredGroupHandle
             || handleGroup != hoveredHandleGroupId
             || handleLayer != hoveredHandleLayer)
        {
            hoveredGroupHandle   = overHandle;
            hoveredHandleGroupId = handleGroup;
            hoveredHandleLayer   = handleLayer;
            repaint();
        }

        if (overHandle)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            hoveredNoteId = -1;
            return;
        }
    }

    if (tool == Tool::hand)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    //-- Phase 16: group labels ---------------------------------------------
    int       occurrenceUnderMouse = 0;
    const int groupUnderMouse      = getGroupLabelAt (e.position, &occurrenceUnderMouse);
    const int groupLayer           = groupUnderMouse >= 0 ? layerIndexOfGroup (groupUnderMouse) : -1;

    // Phase 58: the pill under the pointer is one appearance, so the highlight
    // follows the copy — two pills of the same group must not both light up.
    if (groupUnderMouse != hoveredGroupId || groupLayer != hoveredGroupLayer
         || (groupUnderMouse >= 0 && occurrenceUnderMouse != hoveredGroupOccurrence))
    {
        hoveredGroupId         = groupUnderMouse;
        hoveredGroupLayer      = groupLayer;
        hoveredGroupOccurrence = occurrenceUnderMouse;
        repaint();
    }

    if (groupUnderMouse >= 0)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        hoveredNoteId = -1;
        return;
    }

    //-- Phase 29: the chip first, then the clip's own handles ---------------
    // Both are pointer affordances that only exist while the user is over them:
    // a chip you cannot tell is clickable is the state this phase removed.
    if (focusSlotId >= 0 && getSpotlightExitChipBounds().toFloat().contains (e.position))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        return;
    }

    if (auto* clip = hitTestSample (e.position); clip != nullptr)
    {
        //-- Phase 73: a block of a CLOSED group has no edges to grab ----------
        // The pointer must agree with the press: the press answers "the whole
        // appearance", so an edge cursor here would promise a trim that will not
        // happen. Cursor and behaviour have to be the same sentence.
        if (isGroupLocked (clip->groupId))
        {
            if (hoveredSampleHandle)
            {
                hoveredSampleHandle   = false;
                hoveredSampleHandleId = -1;
                repaint();
            }

            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            return;
        }

        const auto  handle = getSampleAutomationHandleBounds (*clip);
        const bool  overHandle = ! handle.isEmpty() && handle.contains (e.position);

        if (overHandle != hoveredSampleHandle
             || (overHandle && hoveredSampleHandleId != clip->id))
        {
            hoveredSampleHandle   = overHandle;
            hoveredSampleHandleId = overHandle ? clip->id : -1;
            repaint();
        }

        if (overHandle)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            return;
        }

        // Phase 58: a SELECTED block's band is a trim target even when another
        // block covers it — the press will grab the selected one (see
        // mouseDown), so the pointer has to say so.
        // Phase 72: four edges now, so the band itself says WHICH resize: the
        // two vertical edges change the length, the two horizontal ones the
        // height.
        auto edge = clipEdgeAt (e.position);

        if (edge.first < 0)
            edge = { clip->id, edgeOfClip (getSampleScreenBounds (stack.getActiveLayerIndex(),
                                                                  clip->id),
                                           e.position) };

        if (edge.second == ClipEdge::top || edge.second == ClipEdge::bottom)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        if (edge.second == ClipEdge::head || edge.second == ClipEdge::tail)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        setMouseCursor (juce::MouseCursor::PointingHandCursor);

        if (hoveredNoteId != -1)
        {
            hoveredNoteId = -1;
            repaint();
        }

        return;
    }

    auto* hit = hitTestActiveLayer (e.position);
    const int newHovered = hit != nullptr ? hit->id : -1;

    if (hit != nullptr)
    {
        const auto handle = getNoteAutomationHandleBounds (*hit);
        const auto bounds = getNoteBounds (*hit);
        const float band  = ui::pf (kResizeHandleWidth);

        if (! handle.isEmpty() && handle.contains (e.position))
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else if (isGroupLocked (hit->groupId))
            setMouseCursor (juce::MouseCursor::PointingHandCursor);   // Phase 73: one object
        else if (e.position.x > bounds.getRight() - band || e.position.x < bounds.getX() + band)
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);   // either end trims
        else
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    else
        updateCursor();       // empty grid: the tool owns the pointer

    if (newHovered != hoveredNoteId)
    {
        hoveredNoteId = newHovered;
        repaint();
    }
}


void PianoRollComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredNoteId != -1 || hoveredTool != -1
         || hoveredAutomationMenu != 0 || hoveredEnvelopePoint != -1
         || hoveredGroupId != -1 || hoveredGroupHandle || hoveredGroupMenu != 0
         || hoveredSampleHandle || hoveredSampleCurveMenu != 0
         || hoveredSampleCurvePoint != -1)
    {
        hoveredNoteId = -1;
        hoveredTool = -1;
        hoveredAutomationMenu = 0;
        hoveredEnvelopePoint  = -1;
        hoveredGroupId        = -1;
        hoveredGroupLayer     = -1;
        hoveredGroupHandle    = false;
        hoveredGroupMenu      = 0;
        hoveredSampleHandle     = false;
        hoveredSampleHandleId   = -1;
        hoveredSampleCurveMenu  = 0;
        hoveredSampleCurvePoint = -1;
        repaint();
    }
}


void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl+wheel is the horizontal zoom. On Windows Cmd *is* Ctrl, so one check
    // covers both platforms.
    if (e.mods.isCommandDown())
    {
        const float step = std::abs (wheel.deltaY) > 0.0001f ? wheel.deltaY : -wheel.deltaX;
        setBeatWidth (beatWidth * (1.0f + step * 0.8f));
        return;
    }

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const float dx = std::abs (wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY;
        scrollBeats -= (double) dx * 4.0;
    }
    else
    {
        scrollY -= wheel.deltaY * 120.0f;
    }

    clampScroll();
    repaint();
}


bool PianoRollComponent::keyPressed (const juce::KeyPress& key)
{
    // Phase 11: copy the selected note's state to the pencil. Shift+Cmd/Ctrl+C
    // because plain copy is the OS clipboard's, and Cmd/Ctrl+C on notes will be
    // a real note copy the day this gets a clipboard.
    if (key.getKeyCode() == 'C'
         && key.getModifiers().isCommandDown()
         && key.getModifiers().isShiftDown())
    {
        if (! hasPaintState && copyNoteStateFromSelection())
        {
            setTool (Tool::pencil);
            return true;
        }
    }

    //-- Phase 29: Esc is the way back out -----------------------------------
    // It closes whatever floating editor is open and it ends the spotlight. The
    // spotlight matters most: it veils the whole grid, and until this key
    // existed the only exits were in another panel.
    if (key == juce::KeyPress::escapeKey)
    {
        bool didSomething = false;

        if (expandedNoteId >= 0)
        {
            closeNoteAutomation();
            didSomething = true;
        }

        if (expandedGroupId >= 0)
        {
            closeGroupAutomation();
            didSomething = true;
        }

        if (expandedSampleId >= 0)
        {
            closeSampleAutomation();
            didSomething = true;
        }

        if (dismissSpotlight())     // Phase 30: one exit, shared with the grid click
            didSomething = true;

        if (didSomething)
            return true;
    }

    if (key.getKeyCode() == 'D' && key.getModifiers().isCommandDown())
    {
        // Phase 29: one gesture, one copy — a mixed selection copies as a mixed
        // selection. The two kinds keep their own offsets (notes by a beat,
        // clips by their length), because those are the rules they always had.
        if (hasAnySelection())
        {
            duplicateEditSelection();
            return true;
        }
    }

    //-- Phase 16: G groups the selection, Shift+G takes it apart ------------
    // No modifier for the plain case: G is unbound in this roll, and a single
    // key is what makes grouping feel like an editing move, not a dialog.
    if (key.getKeyCode() == 'G')
    {
        if (key.getModifiers().isShiftDown())
        {
            if (ungroupSelection())
                return true;
        }
        else if (! key.getModifiers().isAnyModifierKeyDown()
                  && hasAnySelection())
        {
            if (groupSelection() > 0)
                return true;
        }
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        // Phase 29: "delete what is selected", not "delete one kind of it".
        // A marquee can leave both kinds selected, and a Delete that quietly
        // kept half the selection would be the worst kind of surprise.
        const bool deletedClips = deleteSelectedSamples();
        const bool deletedNotes = deleteSelectedNotes();

        if (deletedClips || deletedNotes)
            return true;
    }

    return false;
}
} // namespace atomic

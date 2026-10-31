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
PianoRollComponent::PianoRollComponent (LayerStack& layerStackToUse, RackModel& rackToUse)
    : stack (layerStackToUse),
      rack (rackToUse)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);

    stack.addListener (this);
    rack.addListener (this);
    lastActiveLayer = stack.getActiveLayerIndex();

    // Open the view around C5 so the demo content is on screen immediately.
    scrollY = pitchToContentY (84);
}


PianoRollComponent::~PianoRollComponent()
{
    stack.removeListener (this);
    rack.removeListener (this);
}


//==============================================================================
int PianoRollComponent::getEditableNoteIdAt (juce::Point<float> position) const
{
    if (auto* note = hitTestActiveLayer (position))
        return note->id;

    return -1;
}


juce::Rectangle<float> PianoRollComponent::getNoteScreenBounds (int layerIndex, int noteId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    if (auto* note = layer->findNote (noteId))
        return getNoteBounds (*note);

    return {};
}


bool PianoRollComponent::isLayerShown (int layerIndex) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    // The sheet being edited is always drawn. Anything else is a ghost and
    // needs its eye open — that is the whole of Phase 20's visibility rule, so
    // it lives in exactly one place and the painter cannot drift from it.
    return layerIndex == stack.getActiveLayerIndex() || layer->isVisible();
}


PianoRollComponent::GhostHit PianoRollComponent::hitTestGhost (juce::Point<float> position) const
{
    const int activeIndex = stack.getActiveLayerIndex();

    // Descending, and in the painter's own order turned inside out: ghosts are
    // painted in ascending layer order (drawNotes pass 1), so the highest index
    // is the one the user sees on top; notes are painted after blocks, so a note
    // wins over a block it sits on. No active sheet here at all — it is opaque,
    // drawn last, and its own hit tests ran before this one.
    for (int i = stack.getNumLayers() - 1; i >= 0; --i)
    {
        if (i == activeIndex || ! isLayerShown (i))
            continue;

        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        const auto& notes = layer->getNotes();

        for (auto it = notes.rbegin(); it != notes.rend(); ++it)
            if (getNoteBounds (*it).contains (position))
                return { i, it->id, false };

        for (const auto& clip : layer->getSamples())
            if (getSampleScreenBounds (i, clip.id).contains (position))
                return { i, clip.id, true };
    }

    return {};
}


//==============================================================================
void PianoRollComponent::layersChanged()
{
    //-- Phase 73: any structural change closes the open group ---------------
    // Unconditional, and deliberately so. This notification is how a project
    // load, an undo and a layer switch all arrive. After an undo the SAME group
    // id can come back (ids are serialised), so "does the group still exist"
    // would answer yes and quietly re-open a group the user never double-clicked
    // in this session. The cost of clearing is one double click; the cost of
    // keeping is an editor that thinks it is still inside something.
    editingGroupId = -1;

    // Switching sheets must not carry a stale selection across layers, or you
    // would end up dragging notes you cannot see.
    if (stack.getActiveLayerIndex() != lastActiveLayer)
    {
        lastActiveLayer = stack.getActiveLayerIndex();
        selection.clear();
        sampleSelection.clear();     // Phase 28: clip ids are per-layer too
        dragMode = DragMode::none;
        dragOriginals.clear();
        dragClipOriginals.clear();
        dragTrimOriginals.clear();
        hoveredNoteId = -1;
        notifySelectionChanged();
    }

    clampScroll();
    repaint();
}


void PianoRollComponent::notesChanged()
{
    // Phase 73: an emptied group is pruned on this notification (see
    // pruneEmptyGroups), so this is where the open group can have just ceased
    // to exist. Healing here rather than at each of the four call sites keeps
    // the rule "a group id is valid only while it names a group".
    if (editingGroupId >= 0 && layerIndexOfGroup (editingGroupId) < 0)
        editingGroupId = -1;

    repaint();
}


void PianoRollComponent::rackChanged()
{
    // Synth colours drive the override badges, so a name/colour change matters.
    repaint();
}


void PianoRollComponent::namesChanged()
{
    // P79: nothing but words moved. Deliberately does NOT go through
    // layersChanged() — that one closes the open group, and the group being
    // renamed is very often the one being edited.
    repaint();
}


juce::String PianoRollComponent::sampleLabelText (const SampleEvent& sample) const
{
    juce::String label = sample.name;

    // Phase 27: the block says which sampler plays it — but only when that is
    // news. A clip dropped from a file is mounted on a sampler grown FOR it, so
    // the two names start identical and the arrow would just repeat itself; it
    // earns its space the moment the clip points at a DIFFERENT sampler, which
    // is exactly when the sound changes without the block moving.
    //
    // P79: this is also why renaming a sampler has to rename the block. The
    // two are the same word by construction, so a rename that updated only one
    // of them would make the block claim it is mounted somewhere else.
    if (sample.outputSlotId >= 0)
        if (const auto* slot = rack.findSlot (sample.outputSlotId);
            slot != nullptr && slot->name != sample.name)
            label += juce::String ("  ") + utf8 ("\xe2\x86\x92") + " " + slot->name;

    return label;
}


juce::String PianoRollComponent::getSampleLabelForTest (int layerIndex, int sampleId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || sampleId < 0)
        return {};

    for (const auto& sample : layer->getSamples())
        if (sample.id == sampleId)
            return sampleLabelText (sample);

    return {};
}


void PianoRollComponent::setPlayheadBeat (double beats)
{
    if (juce::approximatelyEqual (beats, playheadBeat))
        return;

    const double previous = playheadBeat;

    const double visible = getVisibleBeats();

    //-- Phase 58: hand the view back when the playhead comes home -----------
    // Whatever moved the playhead — the transport, a scrub, a loop jump — if it
    // is inside the window the user set up before the view was pushed away, that
    // window comes back. Without this the scroll bar was a one-way trip: the
    // playhead returned, the picture did not.
    //
    // Phase 64: ...but "comes home" has to mean it MOVED BACK. The window the
    // push captured is the one the playhead was sitting in at that moment, so on
    // the very next frame the playhead is still inside it: the hand-back fired
    // immediately, put the scroll back where it started, the push fired again on
    // the frame after, and the picture jumped 85% of the window back and forth at
    // 60 fps. That was the flicker, and it started exactly where the push does —
    // three quarters of the way across. A loop jump, a scrub and a seek all move
    // the playhead backwards; the ordinary forward rollout never does.
    //
    // Phase 67: and it stands aside while the ruler is being dragged. A scrub is
    // the user moving the playhead by hand, so a hand-back would be two writers
    // on the same number, one per mouse-move: the window would snap back to
    // wherever the playback left it and the drag would have to push it out
    // again. The gesture owns the view until the button comes up.
    if (followScrolled && visible > 1.0
         && dragMode != DragMode::rulerScrub
         && beats < previous - 1.0e-9
         && beats >= userScrollBeats - 1.0e-9
         && beats <= userScrollBeats + visible + 1.0e-9)
    {
        const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
        const double target   = juce::jlimit (0.0, maxBeats, userScrollBeats);

        followScrolled = false;

        const bool moved = ! juce::approximatelyEqual (target, scrollBeats);

        scrollBeats  = target;
        playheadBeat = beats;
        soundingNotes.clear();

        if (transportActive)
            collectSoundingNotes (beats, soundingNotes);

        if (moved)
        {
            repaint();

            if (onScrollChanged != nullptr)
                onScrollChanged();
        }

        return;
    }

    //-- Phase 66: follow at the EDGE of the window, not at three quarters ----
    // The old trigger fired when the playhead reached 75% of the window and
    // dropped it back at 15% — a page-turn: the picture held still, then jumped
    // a screen and held still again. What the user asked for is the other kind
    // of follow: the playhead walks up to the right edge and STAYS there while
    // the ruler slides under it, and the same on the way back, pinned to the
    // left edge. So the condition is "outside the window" and the landing spot
    // is the edge itself.
    //
    // Beat 0 is the one exception, and it is the user's: a rewind to the very
    // start is a return, not a scroll. Chasing it would yank the window off
    // whatever the user was looking at, and the hand-back above already owns
    // that case — so the left edge only follows while the playhead is
    // genuinely somewhere past the start.
    //
    // Phase 67: the follow also yields to a ruler drag, for the reason above —
    // while the button is down, scrubRulerTo is the one deciding where the
    // window sits. Everything else (playback, a loop jump, a seek from the
    // top bar) still lands here.
    if (transportActive && visible > 1.0
         && dragMode != DragMode::rulerScrub
         && ! juce::approximatelyEqual (previous, beats))
    {
        const double rightEdge = scrollBeats + visible;
        const bool   pastRight = beats > rightEdge + 1.0e-9;
        const bool   pastLeft  = beats > 1.0e-9 && beats < scrollBeats - 1.0e-9;

        if (pastRight || pastLeft)
        {
            // The first push is where "the state before I moved over there" is
            // captured — once, not on every frame, or the window would follow
            // the playhead and there would be nothing to come back to.
            if (! followScrolled)
            {
                userScrollBeats = scrollBeats;
                followScrolled  = true;
            }

            if (pastRight)
                ensureTimelineExtendsTo (beats + visible);

            const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
            const double target   = juce::jlimit (0.0, maxBeats, pastRight ? beats - visible : beats);

            scrollBeats = target;

            playheadBeat = beats;
            soundingNotes.clear();
            collectSoundingNotes (beats, soundingNotes);
            repaint();

            if (onScrollChanged != nullptr)
                onScrollChanged();

            return;
        }
    }

    // Which notes are sounding at the old and the new position. `soundingNotes`
    // is last frame's answer, so the "before" set is already known.
    std::vector<juce::int64> after;

    if (transportActive)
        collectSoundingNotes (beats, after);

    std::vector<juce::int64> before;
    before.swap (soundingNotes);

    playheadBeat = beats;
    soundingNotes = after;

    const auto area = getGridArea();

    // 1. The thin band the playhead itself swept through. This is all that
    //    changes while it moves inside a note: the envelope fill grows into
    //    exactly that sliver.
    auto repaintBand = [this, &area] (double beat)
    {
        if (beat < 0.0)
            return;

        const float x = screenXForBeat (beat);

        if (x < area.getX() - 10.0f || x > area.getRight() + 10.0f)
            return;

        repaint (juce::Rectangle<float> (x - 7.0f, 0.0f, 14.0f, (float) getHeight())
                   .getSmallestIntegerContainer());
    };

    repaintBand (previous);
    repaintBand (beats);

    // 2. Notes that started or stopped sounding: their whole block changes in
    //    one go (the lit outline and the envelope fill switch on or off), so a
    //    thin band would leave half of it stale. Notes that are in both sets
    //    did not change state and are left alone.
    auto contains = [] (const std::vector<juce::int64>& list, juce::int64 value)
    {
        return std::find (list.begin(), list.end(), value) != list.end();
    };

    auto repaintBlock = [this] (juce::int64 packed)
    {
        const int layerIndex = (int) (packed >> 32);
        const int noteId     = (int) (packed & 0xffffffff);

        const auto bounds = getNoteScreenBounds (layerIndex, noteId);

        if (! bounds.isEmpty())
            repaint (bounds.expanded (3.0f).getSmallestIntegerContainer());
    };

    for (auto packed : before)
        if (! contains (after, packed))
            repaintBlock (packed);

    for (auto packed : after)
        if (! contains (before, packed))
            repaintBlock (packed);
}


void PianoRollComponent::collectSoundingNotes (double beat, std::vector<juce::int64>& out) const
{
    out.clear();

    if (beat < 0.0)
        return;


    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
            if (envelopeProgress (note, beat) >= 0.0)
                out.push_back (((juce::int64) i << 32) | (juce::int64) (juce::uint32) note.id);
    }
}


double PianoRollComponent::envelopeProgress (const NoteEvent& note, double playheadBeat)
{
    // Delegates to the note: "is this note sounding here" is a property of the
    // note and the musical position, not of this component.
    return note.progressAt (playheadBeat);
}


double PianoRollComponent::getPlayheadProgress (int layerIndex, int noteId) const
{
    if (! transportActive || playheadBeat < 0.0)
        return -1.0;

    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return -1.0;

    if (auto* note = layer->findNote (noteId))
        return envelopeProgress (*note, playheadBeat);

    return -1.0;
}


void PianoRollComponent::setBpm (double newBpm)
{
    const double clamped = juce::jlimit (30.0, 300.0, newBpm);

    if (juce::approximatelyEqual (clamped, bpm))
        return;

    bpm = clamped;

    // The ruler's time column and the one-second grid both move; the beat grid
    // does not, which is the whole point of laying out in beats.
    repaint();
}


void PianoRollComponent::setTransportActive (bool shouldBeActive)
{
    if (transportActive == shouldBeActive)
        return;

    transportActive = shouldBeActive;

    if (! transportActive)
        soundingNotes.clear();

    repaint();
}


//==============================================================================
juce::Rectangle<float> PianoRollComponent::getGridArea() const
{
    return getLocalBounds()
             .withTrimmedLeft (keyboardWidth)
             .withTrimmedTop (toolbarHeight + rulerHeight)
             .withTrimmedBottom (scrollbarHeight + getVelocityLaneArea().getHeight())
             .toFloat();
}


juce::Rectangle<int> PianoRollComponent::getVelocityLaneArea() const
{
    if (! showVelocityLane)
        return {};

    // Never more than a third of the component: the lane is a tool, and a tool
    // that can eat the sheet it edits is worse than a tool that gets shorter.
    const int height = juce::jmin (velocityLaneHeight,
                                   juce::jmax (0, getHeight() / 3));

    if (height <= 0)
        return {};

    auto strip = getLocalBounds().withTrimmedBottom (scrollbarHeight);

    return strip.removeFromBottom (height);
}


void PianoRollComponent::setVelocityLaneVisible (bool shouldShow)
{
    if (showVelocityLane == shouldShow)
        return;

    showVelocityLane = shouldShow;

    // Every y in this view is derived from the grid area, so growing the lane
    // moves the music up. The vertical scroll has to be re-clamped or the pitch
    // the view was showing slides under the ruler.
    clampScroll();
    updateCursor();
    repaint();
}


juce::Rectangle<int> PianoRollComponent::getRulerArea() const
{
    return getLocalBounds().withTrimmedLeft (keyboardWidth)
                           .withTrimmedTop (toolbarHeight)
                           .withHeight (rulerHeight);
}

//==============================================================================
//  Phase 87: the loop strip
//==============================================================================
juce::Rectangle<int> PianoRollComponent::getLoopBarArea() const
{
    // The bottom of the ruler, one pixel in from each edge.
    //
    // Bottom and not top: the bar numbers and their seconds live in the upper
    // two thirds, and a band across them would make the ruler unreadable at
    // exactly the zoom levels where you are trying to decide where the loop
    // goes. The beat ticks it covers are drawn first and show through — the
    // band is a tint, not an opaque bar.
    const auto ruler = getRulerArea();

    // Seven pixels, and no label: the band sits under the seconds row, where
    // the only thing it covers is the minor beat ticks. It used to be twelve
    // and carry the word 循环 — which landed on top of "0:06.0" and made both
    // unreadable. The grid tint (drawGridLines) is what says what the loop IS.
    return ruler.withTrimmedTop (ruler.getHeight() - ui::px (7))
                .withTrimmedBottom (ui::px (1))
                .reduced (1, 0);
}

void PianoRollComponent::setLoopRegion (const LoopRegion& region)
{
    if (loopRegion == region)
        return;

    loopRegion = region;
    repaint (getRulerArea());
}


juce::Rectangle<int> PianoRollComponent::getMeterCornerArea() const
{
    // The cell the ruler's corner readout is painted into: the keyboard's width,
    // the ruler's height, above the grid. `drawRuler` draws "1 小节 = 2.00 s"
    // here, and this is the same rectangle — the clickable one and the painted
    // one have to be one expression, or the control is invisible in the wrong
    // place.
    return { 0, toolbarHeight, keyboardWidth, rulerHeight };
}


void PianoRollComponent::setBeatsPerBar (int beats)
{
    const int clamped = meter::clamp (beats);

    if (clamped == beatsPerBar)
        return;

    beatsPerBar = clamped;

    // The bar lines get heavier or lighter and the numbers move; the notes do
    // not. Nothing else in the roll needs telling — `beatsPerBar` is read where
    // it is used rather than cached into a shape.
    repaint();
}


juce::Rectangle<int> PianoRollComponent::getKeyboardArea() const
{
    // Phase 97: the keys stop above the velocity lane's gutter. They used to
    // run to the scrollbar, which was the same pixels until the lane existed —
    // and a keyboard painted over the lane's label is a "力度" you can play.
    return getLocalBounds().withTrimmedTop (toolbarHeight + rulerHeight)
                           .withTrimmedBottom (scrollbarHeight
                                                 + getVelocityLaneArea().getHeight())
                           .withWidth (keyboardWidth);
}


juce::Rectangle<int> PianoRollComponent::getToolbarArea() const
{
    return getLocalBounds().withHeight (toolbarHeight);
}


juce::Rectangle<int> PianoRollComponent::getScrollBarArea() const
{
    return getLocalBounds().removeFromBottom (scrollbarHeight)
                           .withTrimmedLeft (keyboardWidth);
}


juce::Rectangle<float> PianoRollComponent::getScrollThumb() const
{
    auto area = getScrollBarArea().reduced (ui::px (2)).toFloat();
    const double visible = getVisibleBeats();

    if (visible <= 0.0 || visible >= timelineBeats)
        return area;

    const float width = juce::jmax (ui::pf (24), area.getWidth() * (float) (visible / timelineBeats));
    const double maxScroll = timelineBeats - visible;
    const float t = maxScroll <= 0.0 ? 0.0f : (float) (scrollBeats / maxScroll);

    return { area.getX() + t * (area.getWidth() - width), area.getY(), width, area.getHeight() };
}


juce::Rectangle<int> PianoRollComponent::getToolButtonBounds (int index) const
{
    constexpr int buttonWidth = ui::px (58);
    constexpr int gap = ui::px (4);

    // Start after the keyboard column, otherwise the first button is painted
    // underneath the keys.
    return { keyboardWidth + ui::px (10) + index * (buttonWidth + gap), ui::px (4),
             buttonWidth, toolbarHeight - ui::px (8) };
}


double PianoRollComponent::getVisibleBeats() const
{
    const auto area = getGridArea();
    return area.getWidth() > 0.0f ? (double) area.getWidth() / beatWidth : 0.0;
}


void PianoRollComponent::setTool (Tool newTool)
{
    if (tool == newTool)
        return;

    tool = newTool;
    dragMode = DragMode::none;

    updateCursor();
    repaint();
}


//==============================================================================
namespace
{
    /** Draws a pencil into a 32x32 cursor image. Built by hand because JUCE has
        no pencil or eraser in its standard cursor set, and the whole point of
        Phase 18 is that the pointer names the tool.

        Drawn upright in a local frame — tip at the origin, body running up the
        negative y axis, every shape symmetric about x = 0 — and then rotated 45°
        so the tip lands bottom-left. Phase 19: the previous version hand-placed
        a parallelogram, and its tip sat 4 px off the barrel's centreline, which
        read as "the pencil points somewhere else". Symmetry in the local frame
        makes that mistake impossible: there is no coordinate to get wrong. */
    juce::Image drawPencilImage()
    {
        juce::Image image (juce::Image::ARGB, 32, 32, true);
        juce::Graphics g (image);

        constexpr float halfWidth  = 4.2f;
        constexpr float tipLength  = 7.5f;    // graphite cone
        constexpr float woodEnd    = 13.0f;   // where the barrel starts
        constexpr float barrelEnd  = 25.0f;
        constexpr float ferruleEnd = 28.0f;
        constexpr float eraserEnd  = 31.5f;   // total length

        // The hotspot is the apex, so the transform's translation IS the tip.
        constexpr float hotX = 3.0f, hotY = 27.0f;

        const auto toScreen = juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f)
                                  .translated (hotX, hotY);

        auto fillAndOutline = [&] (juce::Path path, juce::Colour colour)
        {
            path.applyTransform (toScreen);

            g.setColour (colour);
            g.fillPath (path);
            g.setColour (theme::surface3);
            g.strokePath (path, juce::PathStrokeType (1.2f));
        };

        // Graphite tip: an isosceles triangle on the axis. Deliberately not the
        // near-black a real pencil has: on a dark piano roll a black tip is an
        // invisible tip, and the point is the only part that has to be locatable.
        juce::Path lead;
        lead.startNewSubPath (0.0f, 0.0f);
        lead.lineTo (-3.2f, -tipLength);
        lead.lineTo ( 3.2f, -tipLength);
        lead.closeSubPath();
        lead.applyTransform (toScreen);

        g.setColour (theme::inkFaint);
        g.fillPath (lead);
        g.setColour (theme::surface3);
        g.strokePath (lead, juce::PathStrokeType (1.0f));

        // Sharpened wood, then the painted barrel.
        juce::Path wood;
        wood.startNewSubPath (-3.2f, -tipLength);
        wood.lineTo ( 3.2f, -tipLength);
        wood.lineTo ( 4.2f, -woodEnd);
        wood.lineTo (-4.2f, -woodEnd);
        wood.closeSubPath();
        fillAndOutline (wood, juce::Colour (0xfff2d9a0));

        juce::Path barrel;
        barrel.addRectangle (-halfWidth, -barrelEnd, halfWidth * 2.0f, barrelEnd - woodEnd);
        fillAndOutline (barrel, juce::Colour (0xffe8b84b));

        juce::Path ferrule;
        ferrule.addRectangle (-halfWidth, -ferruleEnd, halfWidth * 2.0f, ferruleEnd - barrelEnd);
        fillAndOutline (ferrule, juce::Colour (0xffb6bcc8));

        juce::Path eraser;
        eraser.addRoundedRectangle (-halfWidth, -eraserEnd, halfWidth * 2.0f, eraserEnd - ferruleEnd,
                                    1.6f, 1.6f, true, true, false, false);
        fillAndOutline (eraser, juce::Colour (0xffef8fa3));

        return image;
    }

    juce::Image drawEraserImage()
    {
        juce::Image image (juce::Image::ARGB, 32, 32, true);
        juce::Graphics g (image);

        const auto spin = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.25f)
                              .translated (17.0f, 15.0f);

        // Two halves, because a rubber is two colours with a seam down the
        // middle. Same rotation for both, applied to the finished paths.
        juce::Path pink, white;
        pink.addRoundedRectangle  (-11.0f, -6.0f, 11.0f, 12.0f, 3.0f, 3.0f,
                                   true, false, true, false);
        white.addRoundedRectangle (  0.0f, -6.0f, 11.0f, 12.0f, 3.0f, 3.0f,
                                    false, true, false, true);
        pink.applyTransform (spin);
        white.applyTransform (spin);

        g.setColour (juce::Colour (0xffef8fa3));
        g.fillPath (pink);
        g.setColour (juce::Colour (0xfff4f0f6));
        g.fillPath (white);

        juce::Path outline;
        outline.addRoundedRectangle (-11.0f, -6.0f, 22.0f, 12.0f, 3.0f);
        outline.applyTransform (spin);

        g.setColour (theme::surface3);
        g.strokePath (outline, juce::PathStrokeType (1.4f));

        return image;
    }
}


juce::Image PianoRollComponent::renderToolCursorImage (Tool which, juce::Point<int>& hotspot)
{
    switch (which)
    {
        case Tool::eraser: hotspot = { 12, 26 }; return drawEraserImage();
        case Tool::pencil:
        default:           hotspot = { 3, 27 };  return drawPencilImage();
    }
}


void PianoRollComponent::rebuildToolCursors()
{
    if (cursorsBuilt)
        return;

    cursorsBuilt  = true;

    juce::Point<int> hot;

    // One source for "where does this pointer point": the image and its hotspot
    // come out of the same call, so the dev harness that renders the PNG and the
    // OS cursor on screen cannot disagree.
    pencilCursor = juce::MouseCursor (renderToolCursorImage (Tool::pencil, hot), hot.x, hot.y, 1.0f);
    eraserCursor = juce::MouseCursor (renderToolCursorImage (Tool::eraser, hot), hot.x, hot.y, 1.0f);
    handCursor   = juce::MouseCursor (juce::MouseCursor::DraggingHandCursor);
}


void PianoRollComponent::updateCursor()
{
    rebuildToolCursors();

    switch (tool)
    {
        case Tool::pencil: setMouseCursor (pencilCursor); break;
        case Tool::eraser: setMouseCursor (eraserCursor); break;
        case Tool::hand:   setMouseCursor (handCursor);   break;

        case Tool::knife:
            // Phase 51: a crosshair, not a custom blade image. The other tools
            // draw their own cursors because their TIP is the tool; the knife's
            // cut happens AT the click, so the crosshair's centre — one pixel,
            // unambiguous — names the cut position better than a picture of a
            // blade whose tip is somewhere near the hotspot.
            setMouseCursor (juce::MouseCursor::CrosshairCursor); break;

        case Tool::select:
        default:           setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}


void PianoRollComponent::auditionNoteOnActiveLayer (const NoteEvent& note, bool retrigger)
{
    if (onAuditionRequested == nullptr)
        return;

    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    // Same resolution the engine publishes with: the preview has to come out of
    // the synth you will hear in the mix, not out of "the first one".
    const auto resolution = routing::resolve (*layer, note, rack);

    onAuditionRequested (resolution.slotId, note.pitch, note.velocity, retrigger);
}


void PianoRollComponent::auditionPitchOnActiveLayer (int pitch, bool retrigger)
{
    if (onAuditionRequested == nullptr)
        return;

    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    // A bare key has no note to override anything and no group to be mounted
    // through, so it goes exactly where the computer keyboard goes —
    // `routing::auditionSlotId`, which prefers the spotlit instrument and falls
    // back to the layer's default.
    //
    // Phase 43: this used to call `layerSlotId` directly, which ignores the
    // spotlit card. With a VST selected, the computer keyboard played the VST
    // and these keys played the layer's built-in synth: same instrument, two
    // answers, and the user hears the difference immediately.
    const int slotId = routing::auditionSlotId (focusSlotId, *layer, rack);

    onAuditionRequested (slotId, juce::jlimit (0, numPitches - 1, pitch), 0.8f, retrigger);
}


void PianoRollComponent::setSnapEnabled (bool shouldSnap)
{
    if (snapEnabled == shouldSnap)
        return;

    snapEnabled = shouldSnap;

    // Phase 18: one switch for the whole editor. The automation window draws on
    // the same grid, so telling it here is what makes "snap off" mean off.
    if (onSnapChanged != nullptr)
        onSnapChanged (snapEnabled);

    repaint();
}


void PianoRollComponent::setGhostAlpha (float alpha)
{
    const float clamped = juce::jlimit (0.12f, 0.75f, alpha);

    if (juce::approximatelyEqual (clamped, ghostAlphaValue))
        return;

    ghostAlphaValue = clamped;
    repaint();
}


void PianoRollComponent::setScrollBeats (double beats)
{
    const double visible = getVisibleBeats();
    const double lengthBefore = timelineBeats;

    // Phase 11: no wall at 64 beats. Scrolling past the end adds another chunk
    // (bounded per call), so the view can keep going as long as the user keeps
    // pushing, while a single bad argument cannot make the ruler five hours long.
    if (beats + visible > timelineBeats)
        ensureTimelineExtendsTo (beats + visible);

    const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
    const double clamped = juce::jlimit (0.0, maxBeats, beats);

    const bool moved = ! juce::approximatelyEqual (clamped, scrollBeats);

    if (! moved && juce::approximatelyEqual (lengthBefore, timelineBeats))
        return;

    scrollBeats = clamped;

    if (moved)
        noteUserScroll();      // Phase 58: this is the window the user wants

    // Phase 60: scrolling BACK is what has to give the empty tail up again. The
    // grow above is the other half of the same sentence — the roll extends when
    // you push into nowhere, and it stops being long once you leave.
    trimTimelineToView();

    repaint();

    if (onScrollChanged != nullptr)
        onScrollChanged();
}


void PianoRollComponent::setTopPitch (int pitch)
{
    // The vertical twin of `setScrollBeats`, and for the same reason: without it
    // nothing outside could say "make sure THIS pitch is on screen". `resized()`
    // and the mouse wheel both write `scrollY`, so a caller that had to point a
    // camera at a note could only hope the roll happened to be somewhere useful.
    const float contentHeight = (float) numPitches * keyHeight;
    const float maxScrollY = juce::jmax (0.0f, contentHeight - getGridArea().getHeight());
    const float wanted = juce::jlimit (0.0f, maxScrollY, pitchToContentY (pitch));

    if (juce::approximatelyEqual (wanted, scrollY))
        return;

    scrollY = wanted;

    repaint();

    if (onScrollChanged != nullptr)
        onScrollChanged();
}


float PianoRollComponent::pitchToContentY (int pitch) noexcept
{
    return (float) (numPitches - 1 - pitch) * keyHeight;
}


int PianoRollComponent::contentYToPitch (float y) noexcept
{
    const int p = numPitches - 1 - (int) std::floor (y / keyHeight);
    return juce::jlimit (0, numPitches - 1, p);
}


float PianoRollComponent::screenYForPitch (int pitch) const noexcept
{
    return getGridArea().getY() + pitchToContentY (pitch) - scrollY;
}


int PianoRollComponent::pitchAtScreenY (float y) const noexcept
{
    return contentYToPitch (y - getGridArea().getY() + scrollY);
}


float PianoRollComponent::screenXForBeat (double beat) const noexcept
{
    return getGridArea().getX() + (float) ((beat - scrollBeats) * beatWidth);
}


double PianoRollComponent::beatAtScreenX (float x) const noexcept
{
    return scrollBeats + (double) (x - getGridArea().getX()) / beatWidth;
}


void PianoRollComponent::scrubRulerTo (float screenX)
{
    //-- Phase 67: the pointer may leave the window, and it takes the window --
    // A ruler drag used to be clamped to what was on screen: the playhead went
    // wherever the pointer's X mapped to, and once that X was past the edge the
    // head sat off screen while the picture stood still. Scrubbing to bar 40 was
    // therefore drop, wheel, drop, wheel. What a time axis should do is what the
    // transport itself does at the edge (Phase 66) — carry the view along — so
    // the two now share the same geometry:
    //
    //   past the right edge  ->  scroll so the beat lands ON that edge
    //   past the left  edge  ->  scroll so the beat lands ON that edge
    //
    // Both spellings are "pin the playhead to the edge being pushed on", which
    // is why they are two lines and not a mode. Inside the window neither fires,
    // so an ordinary scrub still scrolls nothing.
    const double raw     = beatAtScreenX (screenX);
    const double visible = getVisibleBeats();

    // Grow first, snap second. Clamping to the old length before the scroll ran
    // would stop the head at the old end of the ruler with the pointer still
    // moving — the extension is what "dragged past the end" means.
    if (raw > timelineBeats)
        ensureTimelineExtendsTo (raw);

    const double beat = juce::jlimit (0.0, timelineBeats, snapEnabled ? snapBeat (raw) : raw);

    if (visible > 1.0)
    {
        const double rightEdge = scrollBeats + visible;

        // Judged on the SNAPPED beat, not on the pointer: with snapping on, the
        // playhead is the thing the user watches, and pinning it half a cell off
        // the edge so that the pointer can be exactly on it would be backwards.
        if (beat > rightEdge + 1.0e-9)
            setScrollBeats (beat - visible);
        else if (beat < scrollBeats - 1.0e-9)
            setScrollBeats (beat);
    }

    if (onLocateRequested != nullptr)
        onLocateRequested (beat);
}


juce::Rectangle<float> PianoRollComponent::getNoteBounds (const NoteEvent& note) const
{
    const float x = screenXForBeat (note.startBeat);
    const float y = screenYForPitch (note.pitch);
    const float w = juce::jmax (5.0f, (float) (note.lengthBeats * beatWidth) - 1.5f);

    return { x, y + 1.0f, w, keyHeight - 2.0f };
}


//==============================================================================
double PianoRollComponent::visibleGridStep() const noexcept
{
    // The one number this whole phase is about: how many pixels sit between two
    // neighbouring grid lines. 5 design units is roughly the point where two
    // hairlines stop reading as separate lines and start reading as shading.
    const double minPixels = (double) ui::pf (5.0f);

    double step = finestGridStep;

    // ×4 each time, in both directions: 1/64 → 1/16 → beat → bar → 4 bars. ×4
    // and not ×2 because the levels the roll draws are already four apart, so a
    // half step would land between two levels that are never drawn. Starting at
    // the finest rung rather than at the 1/16 means the zoom-in side is the
    // same loop as the zoom-out side, and neither can forget the other.
    while (step * (double) beatWidth < minPixels && step < gridDivision * 1024.0)
        step *= 4.0;

    return step;
}


double PianoRollComponent::minimumNoteLength() const noexcept
{
    // Not `visibleGridStep()` alone: zoomed out that returns a beat or a bar,
    // and letting the picture raise the floor would mean a note dragged at 12
    // px/beat could never be shorter than a bar. The floor only ever moves DOWN
    // from the musical minimum.
    return juce::jmin (gridDivision, visibleGridStep());
}


float PianoRollComponent::gridLineAlpha (double beat, int beatsPerBar) noexcept
{
    // Asked of the BEAT, not of the loop index. While the step is 1/16 the two
    // happen to agree (`index % 4 == 0` is a beat); the moment it climbs to a
    // beat, index 4 is beat 4 and its own bar line — and the old test would have
    // painted it as a beat, or worse, as a subdivision.
    auto onUnit = [] (double value, double unit)
    {
        return std::abs (value - std::round (value / unit) * unit) < 1.0e-6;
    };

    if (onUnit (beat, (double) beatsPerBar))        // bar
        return 0.16f;

    if (onUnit (beat, 1.0))                         // beat
        return 0.075f;

    if (onUnit (beat, gridDivision))                // 1/16 note (1/4 beat)
        return 0.030f;

    // Phase 69: below the 1/16 there is now a fifth rung on the ladder, and it
    // must not borrow the 1/16's alpha — at 768 px/beat the two are both on
    // screen, and four levels of subdivision all drawn at 0.030 is the flat grey
    // texture the step ladder exists to prevent. Faint, but present: it is what
    // tells you a note is a 1/64 and not a 1/16.
    return 0.018f;
}


void PianoRollComponent::collectGridLines (double fromBeat, double toBeat,
                                           std::vector<double>& out) const
{
    out.clear();

    const double step = visibleGridStep();

    if (step <= 0.0 || toBeat < fromBeat)
        return;

    const int firstIndex = (int) std::floor (fromBeat / step);
    const int lastIndex  = (int) std::ceil  (toBeat / step);

    // One pass, no filtering by "is this line thin enough to skip": the step
    // already made that decision, which is why the skipped lines are not
    // something the drawing code can forget to check.
    for (int index = firstIndex; index <= lastIndex; ++index)
    {
        const double beat = (double) index * step;

        if (beat < 0.0 || beat > timelineBeats)
            continue;

        out.push_back (beat);
    }
}


double PianoRollComponent::snapBeat (double beats) const noexcept
{
    // The visible step, not the 1/16: zoomed out, snapping to a line that is not
    // drawn is the same as snapping to nothing — the note lands on a mark the
    // user cannot see and cannot aim at.
    const double step = visibleGridStep();

    return std::round (beats / step) * step;
}


bool PianoRollComponent::isBlackKey (int pitch) noexcept
{
    const int degree = pitch % 12;
    return degree == 1 || degree == 3 || degree == 6 || degree == 8 || degree == 10;
}


void PianoRollComponent::clampScroll()
{
    const auto area = getGridArea();

    const float contentHeight = (float) numPitches * keyHeight;
    const float maxScrollY = juce::jmax (0.0f, contentHeight - area.getHeight());
    scrollY = juce::jlimit (0.0f, maxScrollY, scrollY);

    const double visibleBeats = (double) area.getWidth() / beatWidth;

    // Phase 61: ask for the ruler the CLAMPED view needs. Zooming out pivots on a
    // beat, so `scrollBeats` is very often negative here — the anchor sits left of
    // beat 0 and the number is about to become 0 two lines down. The raw sum then
    // under-reports: at 14.8 px/beat the view wanted 74.5 beats while -16.6 + 74.5
    // = 57.9 said "64 is plenty". The roll only caught up on the NEXT notch, so
    // every zoom-out that crossed the end of the ruler showed a dead strip on the
    // right for one step.
    const double wanted = juce::jmax (0.0, scrollBeats) + visibleBeats;

    if (wanted > timelineBeats)
        ensureTimelineExtendsTo (wanted);

    const double maxBeats = juce::jmax (0.0, timelineBeats - visibleBeats);
    scrollBeats = juce::jlimit (0.0, maxBeats, scrollBeats);

    // Phase 60: and the other way. Every caller of clampScroll — resize, zoom,
    // wheel, a jumped-to track click — lands here, so "the roll is exactly as
    // long as it needs to be" is settled in one place instead of in each of them.
    trimTimelineToView();
}


//==============================================================================
void PianoRollComponent::ensureTimelineExtendsTo (double beats)
{
    if (beats <= timelineBeats)
        return;

    // At most four chunks per call: continuous scrolling advances smoothly,
    // while a single bogus request (a stale anchor, a huge drag) is capped.
    const double target = juce::jmin (beats, timelineBeats + beatChunk * 4.0);

    timelineBeats = juce::jlimit (totalBeats, maxTimelineBeats,
                                  std::ceil (target / beatChunk) * beatChunk);

    if (onTimelineChanged != nullptr)
        onTimelineChanged();
}


void PianoRollComponent::trimTimelineToView()
{
    const double visible = getVisibleBeats();

    // What the roll must still be able to show. Three things, and all three are
    // somebody's anchor:
    //
    //   * the music, plus a little air past the last note;
    //   * the window the user is looking through — trimming under it would drag
    //     the view left while the user was scrolling;
    //   * the playhead. A ruler that stops before the playhead is a ruler you
    //     cannot click on the beat you are listening to.
    //
    // Rounded UP to a chunk, which is the same grid ensureTimelineExtendsTo
    // grows on: a trim that landed on a non-chunk boundary would make the next
    // scroll grow again immediately, and the ruler would breathe on every wheel
    // tick.
    const double needed = juce::jmax (juce::jmax (getContentEndBeat() + 4.0,
                                                  scrollBeats + visible),
                                      playheadBeat + 4.0);

    const double length = juce::jlimit (totalBeats, maxTimelineBeats,
                                        std::ceil (needed / beatChunk) * beatChunk);

    if (length >= timelineBeats)
        return;

    timelineBeats = length;

    // `needed` already covers scrollBeats + visible, so this cannot move the
    // view; it is here so the invariant holds even if one of the three anchors
    // is ever changed above without the others.
    const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
    scrollBeats = juce::jlimit (0.0, maxBeats, scrollBeats);

    if (onTimelineChanged != nullptr)
        onTimelineChanged();
}


void PianoRollComponent::resetTimeline()
{
    scrollBeats   = 0.0;
    timelineBeats = totalBeats;

    // A loaded project brings its own length: the roll can never be shorter than
    // the music inside it, or the notes past the end would be unreachable.
    const double needed = getContentEndBeat() + 4.0;

    if (needed > timelineBeats)
        timelineBeats = juce::jlimit (totalBeats, maxTimelineBeats,
                                      std::ceil (needed / beatChunk) * beatChunk);

    // ...and never shorter than the view either, which clampScroll() handles.
    clampScroll();

    if (onTimelineChanged != nullptr)
        onTimelineChanged();
}


double PianoRollComponent::getContentEndBeat() const
{
    double end = 0.0;


    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i))
            end = juce::jmax (end, layer->getEndBeat());

    return end;
}


void PianoRollComponent::setBeatWidth (float pixels)
{
    // Wheel and menu zoom keep the middle of the view where it is: the pointer
    // is usually near the middle, and pinning beat 0 would throw the music away
    // from it on every notch. The ruler's right-drag is the other case — it
    // pins the column the button went down on — which is why the anchor is a
    // parameter and not a property of this function.
    const auto grid = getGridArea();

    setBeatWidthAround (pixels, beatAtScreenX ((float) grid.getCentreX()), (float) grid.getCentreX());
}


void PianoRollComponent::setBeatWidthAround (float pixels, double anchorBeat, float anchorX)
{
    const float clamped = juce::jlimit (minBeatWidth, maxBeatWidth, pixels);

    if (std::abs (clamped - beatWidth) < 0.01f)
        return;

    const float gridX = (float) getGridArea().getX();

    beatWidth = clamped;

    // The whole of "the zoom pivots on that point": after the width changes,
    // solve the scroll so `anchorBeat` lands back under `anchorX` again.
    scrollBeats = anchorBeat - (double) (anchorX - gridX) / (double) beatWidth;

    // Clamping at the ends of the timeline is the one case where the anchor
    // cannot be honoured — there is no music left of beat 0 to scroll into.
    clampScroll();
    noteUserScroll();          // Phase 58: zooming is the user's own move

    if (onScrollChanged != nullptr)
        onScrollChanged();

    repaint();
}


//==============================================================================
//  Painting
//==============================================================================
void PianoRollComponent::resized()
{
    clampScroll();
}


void PianoRollComponent::flashStatus (const juce::String& text)
{
    statusText     = text;
    statusExpiryMs = juce::Time::getMillisecondCounter() + 2200u;

    if (text.isEmpty())
        stopTimer();
    else
        startTimerHz (6);

    repaint (getToolbarArea());
}


void PianoRollComponent::timerCallback()
{
    if (juce::Time::getMillisecondCounter() < statusExpiryMs)
        return;

    stopTimer();
    statusText.clear();
    repaint (getToolbarArea());
}
} // namespace atomic

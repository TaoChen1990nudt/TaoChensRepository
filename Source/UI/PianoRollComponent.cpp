#include "PianoRollComponent.h"

#include "Util/Metrics.h"
#include "Util/SharedRules.h"     // Phase 52: the sampler drag payload

#include "Media/SampleLibrary.h"
#include "Util/Text.h"
#include "Util/Theme.h"

#include <limits>
#include <map>
#include <set>

namespace atomic
{

namespace
{
    const juce::Colour& kRollBackground = theme::surface0;
    const juce::Colour kGridBackground  = theme::surface2;
    const juce::Colour kBlackRowColour  = theme::bgApp;
    const juce::Colour kRulerBackground = theme::surface2;
    const juce::Colour kKeyWhite        = theme::ink;
    const juce::Colour kKeyBlack        = theme::surface4;
    const juce::Colour& kOutline        = theme::surface4;
    const juce::Colour& kText           = theme::inkMuted;
    const juce::Colour& kPlayheadColour = theme::record;

    /** DESIGN pixels: a note has one of these bands at each end, and dragging
        inside it changes the note's length instead of moving it. Both ends,
        because a note you cannot shorten from the front is a note you have to
        delete and redraw. */
    constexpr float kResizeHandleWidth = 6.0f;

    /** The tail's automation button. Phase 19: it moved inboard — it used to
        cover the right resize band, so the only way to lengthen a note was to
        miss the very thing the note offers. Now it stops short of the band with
        clear air on both sides. */
    constexpr float kNoteHandleWidth = 11.0f;

    /** Phase 25: the air between the button and the resize band on the TAIL
        side only. It used to be the full gap, which left the button sitting
        oddly far from the end it belongs to; 1 px still separates the two so a
        click on the band is never a click on the button, and the band keeps its
        whole width for stretching. */
    constexpr float kNoteHandleTailAir = 1.0f;

    /** Body left over once both resize bands and the button are subtracted.
        Under this the button is not drawn at all: a note that short has no room
        for a button that is not also a resize edge. */
    constexpr float kNoteHandleMinBody = 6.0f;

    //-- Phase 27: the clip menu's id ranges ----------------------------------
    /** One menu, three kinds of answer. Ranges instead of "result - 100", so a
        click that lands in the wrong kind cannot silently become a route. */
    constexpr int kClipMenuUnmount = 6;
    constexpr int kClipMenuNotices = 2999;   // disabled "there are none" rows
    constexpr int kClipMountBase   = 3000;   // + sampler slot id
    constexpr int kClipMenuUngroup = 3999;
    constexpr int kClipMenuNewGroup = 3998;   // Phase 35: make one, containing this
    constexpr int kClipGroupBase   = 4000;   // + group id
    /** Phase 31: + (int) StretchMode. A block of four, well clear of the mount
        and group ranges above it. */
    constexpr int kClipStretchBase = 5000;
    /** Phase 50: reverse. Its own id band — the stretch range is
        checked as `>= 5000 && < 5008`, so anything appended to that family has
        to start far enough away that the range test cannot swallow it. */
    constexpr int kClipMenuReverse = 6001;

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
    }

    /** Phase 35: the words on a group's pill, built once.

        Two functions draw or MEASURE this pill (getGroupLabelBounds and
        drawGroupLabels), and they had already drifted: the measurement counted
        a group's clips ("+ 2") while the drawing omitted them, so a group of
        clips got a pill wider than its text for no visible reason. One builder
        means the mute marker below cannot repeat that mistake — a pill whose
        width was computed from different words than it draws is how a label
        ends up clipped at the right edge. */
    juce::String groupLabelText (const Layer& layer, const NoteGroup& group)
    {
        const int notes = layer.countNotesInGroup (group.getId());
        const int clips = layer.countSamplesInGroup (group.getId());

        juce::String text = group.getName() + utf8 (" · ") + juce::String (notes);

        // A count that only reported notes would read as "the clip is not
        // really in there" (Phase 27).
        if (clips > 0)
            text << " + " + juce::String (clips);

        if (group.isMuted())
            text << utf8 (" · 静音");

        return text;
    }

    /** The label the inline box's menu shows. The model stores ASCII keys, so
        this is the only place the Chinese names exist. */
    juce::String modTargetLabel (NoteModTarget target)
    {
        switch (target)
        {
            case NoteModTarget::pan:   return utf8 ("声相");
            case NoteModTarget::pitch: return utf8 ("音高");
            case NoteModTarget::level:
            default:                   return utf8 ("音量");
        }
    }

    juce::String modTargetHint (NoteModTarget target)
    {
        switch (target)
        {
            case NoteModTarget::pan:   return utf8 ("0.5 居中，两端为左右");
            case NoteModTarget::pitch: return utf8 ("1.0 不弯，0 低一个八度");
            case NoteModTarget::level:
            default:                   return utf8 ("0 静音，1 原始音量");
        }
    }

    /** Phase 24: menu ids at or above this one mean "parameter N of the hosted
        synth", with N = id - kPluginParamItemBase. Well clear of the handful of
        small fixed ids the built-in targets and the menu verbs use, so a growing
        parameter list can never collide with them. */
    constexpr int kPluginParamItemBase = 1000;

    /** How many of a plugin's parameters the menu will list. See
        addPluginParamSubmenu() for why this is not "all of them". */
    constexpr int kMaxListedPluginParams = 64;

    /** A knob: ring plus pointer. The universal "this is a synth parameter, not
        a fader of ours" glyph, used wherever a curve drives a plugin. */
    void drawKnobGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
    {
        g.setColour (colour);

        const auto ring = area.reduced (1.0f);
        g.drawEllipse (ring, 1.2f);

        const auto  centre = ring.getCentre();
        const float radius = juce::jmin (ring.getWidth(), ring.getHeight()) * 0.5f;

        juce::Path needle;
        needle.startNewSubPath (centre);
        needle.lineTo (centre.x + 0.18f * radius, centre.y - 0.80f * radius);

        g.strokePath (needle, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    /** The value a lane "resets to": unity for a level, centre for a pan, and
        centre for a plugin parameter — not zero. An emptied curve must not also
        mean "mute", or cutting a curve would silence the thing it belonged to;
        and 1.0 as a plugin default would slam a cutoff wide open the first time
        anyone cut its lane. */
    float neutralCurveValue (int target, bool groupTarget)
    {
        if (groupTarget)
            return (target == (int) GroupAutomationTarget::pan
                      || target == (int) GroupAutomationTarget::plugin) ? 0.5f : 1.0f;

        return target == (int) NoteModTarget::pan ? 0.5f : 1.0f;
    }

    /** Phase 24: "组 A" -> "组 A 2" (then 3, 4...) until it is free.

        A pasted group needs its own name because the group list is a list of
        names; two rows both called 组 A is a list you cannot read. Keeping the
        original as the prefix is the useful half of the information: it says
        where the copy came from. */
    juce::String uniqueGroupName (const Layer& layer, const juce::String& base)
    {
        const auto taken = [&layer] (const juce::String& candidate)
        {
            for (const auto& group : layer.getGroups())
                if (group.getName() == candidate)
                    return true;

            return false;
        };

        if (! taken (base))
            return base;

        for (int suffix = 2; suffix < 1000; ++suffix)
        {
            const auto candidate = base + " " + juce::String (suffix);

            if (! taken (candidate))
                return candidate;
        }

        return base;
    }

    // Note naming lives in Util/Text.h so the panel and the roll agree.
}

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

//==============================================================================
void PianoRollComponent::layersChanged()
{
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
    repaint();
}

void PianoRollComponent::rackChanged()
{
    // Synth colours drive the override badges, so a name/colour change matters.
    repaint();
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
             .withTrimmedBottom (scrollbarHeight)
             .toFloat();
}

juce::Rectangle<int> PianoRollComponent::getRulerArea() const
{
    return getLocalBounds().withTrimmedLeft (keyboardWidth)
                           .withTrimmedTop (toolbarHeight)
                           .withHeight (rulerHeight);
}

juce::Rectangle<int> PianoRollComponent::getKeyboardArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarHeight + rulerHeight)
                           .withTrimmedBottom (scrollbarHeight)
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

float PianoRollComponent::gridLineAlpha (double beat) noexcept
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

//==============================================================================
//  Selection
//==============================================================================
bool PianoRollComponent::isSelected (int noteId) const
{
    return std::find (selection.begin(), selection.end(), noteId) != selection.end();
}

void PianoRollComponent::setSelection (std::vector<int> newSelection)
{
    selection = std::move (newSelection);

    // Phase 28: one selection at a time. See setSampleSelection for why.
    if (! selection.empty())
        sampleSelection.clear();

    notifySelectionChanged();
    repaint();
}

void PianoRollComponent::setSampleSelection (std::vector<int> ids)
{
    // Drop ids that no longer exist: a delete, an undo, or a layer switch can
    // all leave the list pointing at nothing, and a selection of ghosts makes
    // Delete look broken.
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        ids.clear();
    else
        ids.erase (std::remove_if (ids.begin(), ids.end(),
                                   [layer] (int id) { return layer->findSample (id) == nullptr; }),
                   ids.end());

    sampleSelection = std::move (ids);

    // A block and a note cannot both be "the selection": Delete, Ctrl+C and the
    // next drag would each have to guess. Clicking a block therefore lets go of
    // the notes, and clicking a note lets go of the blocks.
    if (! sampleSelection.empty() && ! selection.empty())
        selection.clear();

    // Phase 32: always tell the host, not only when notes were dropped. The
    // rack lights the sampler a selected block is mounted on, and that answer
    // changes on every click that lands on a block.
    notifySelectionChanged();
    repaint();
}

void PianoRollComponent::setMixedSelection (std::vector<int> noteIds, std::vector<int> sampleIds)
{
    auto* layer = stack.getActiveLayer();

    // Same hygiene as setSampleSelection: an id that no longer resolves makes
    // Delete look broken, so it is dropped at the door.
    if (layer == nullptr)
    {
        noteIds.clear();
        sampleIds.clear();
    }
    else
    {
        sampleIds.erase (std::remove_if (sampleIds.begin(), sampleIds.end(),
                                         [layer] (int id) { return layer->findSample (id) == nullptr; }),
                         sampleIds.end());
        noteIds.erase (std::remove_if (noteIds.begin(), noteIds.end(),
                                       [layer] (int id) { return layer->findNote (id) == nullptr; }),
                       noteIds.end());
    }

    selection       = std::move (noteIds);
    sampleSelection = std::move (sampleIds);

    notifySelectionChanged();
    repaint();
}

bool PianoRollComponent::isSampleSelected (int sampleId) const
{
    return std::find (sampleSelection.begin(), sampleSelection.end(), sampleId)
             != sampleSelection.end();
}

void PianoRollComponent::toggleSampleSelection (int sampleId)
{
    if (const auto found = std::find (sampleSelection.begin(), sampleSelection.end(), sampleId);
        found != sampleSelection.end())
    {
        sampleSelection.erase (found);
    }
    else
    {
        sampleSelection.push_back (sampleId);
    }

    if (! sampleSelection.empty() && ! selection.empty())
    {
        selection.clear();
        notifySelectionChanged();
    }

    repaint();
}

void PianoRollComponent::notifySelectionChanged()
{
    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

int PianoRollComponent::getSelectedRoutingSlotId() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return -1;

    int slotId = -1;
    bool first = true;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr)
            continue;

        const auto resolution = routing::resolve (*layer, *note, rack);

        if (! resolution.isValid())
            return -1;

        if (first)
        {
            slotId = resolution.slotId;
            first = false;
        }
        else if (slotId != resolution.slotId)
        {
            return -1;      // the selection is split: nothing single to highlight
        }
    }

    return slotId;
}

bool PianoRollComponent::selectionUsesOverride() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    for (int id : selection)
        if (const auto* note = layer->findNote (id))
            if (note->outputSlotOverride >= 0)
                return true;

    return false;
}

void PianoRollComponent::routeSelectionTo (int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return;

    // Phase 27: see mountSelectionTo — a sampler cannot play a note, so it is
    // not a destination for one.
    if (slotId >= 0)
        if (const auto* slot = rack.findSlot (slotId); slot != nullptr && slot->isSampler())
            return;

    for (int id : selection)
        if (auto* note = layer->findNote (id))
            note->outputSlotOverride = slotId < 0 ? -1 : slotId;

    stack.notifyNotesChanged();
    notifySelectionChanged();
    repaint();
}

bool PianoRollComponent::mountSelectionTo (int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return false;

    // Phase 27: clicking a sampler card while notes are selected used to mean
    // "route these into that slot", which is now a silent mute — a sampler holds
    // a file and has no oscillator. Refusing outright is better than doing it
    // and leaving the user to work out why the notes stopped.
    if (slotId >= 0)
        if (const auto* slot = rack.findSlot (slotId); slot != nullptr && slot->isSampler())
            return false;

    const int groupId = getSelectedGroupId();

    // Phase 24: a group action only when the WHOLE group is selected.
    //
    // Phase 23 mounted the group as soon as the selection happened to lie
    // inside one, which made "this note should use the other synth" impossible:
    // clicking a single note and then a synth card re-mounted all twelve. The
    // note-level override is the tier that means "just this one", and it can
    // only reach the router if we do not overwrite the group instead.
    if (groupId < 0 || ! isWholeGroupSelected (groupId))
    {
        // A partial (or loose) selection: the per-note override is the honest
        // answer — one exception should stay one exception.
        routeSelectionTo (slotId);
        return true;
    }

    // A group. Mount the group itself, then clear the members' individual
    // overrides: an override outranks the group in routing::resolve(), so
    // leaving one behind would make the group's chip claim a synth the note is
    // not actually playing on.
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* target = stack.getLayer (i);

        if (target == nullptr)
            continue;

        if (auto* group = target->findGroup (groupId))
        {
            group->setOutputSlotId (slotId < 0 ? -1 : slotId);

            for (const auto& note : target->getNotes())
                if (note.groupId == groupId)
                    if (auto* mutableNote = target->findNote (note.id))
                        mutableNote->outputSlotOverride = -1;

            stack.notifyNotesChanged();
            notifySelectionChanged();
            repaint();
            return true;
        }
    }

    return false;
}

//==============================================================================
//  Phase 24: which hosted parameter a curve can drive
//==============================================================================
int PianoRollComponent::resolvedSlotForNote (const NoteEvent& note) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    return routing::resolve (*layer, note, rack).slotId;
}

int PianoRollComponent::resolvedSlotForGroup (int layerIndex, int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return -1;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return -1;

    // Two tiers, exactly like routing::resolve() one level down: the group's
    // own mount when it has a live one, otherwise the layer default.
    if (! group->followsLayerOutput() && rack.findSlot (group->getOutputSlotId()) != nullptr)
        return group->getOutputSlotId();

    return routing::layerSlotId (*layer, rack);
}

std::vector<PluginParamInfo> PianoRollComponent::pluginParamsForSlot (int slotId) const
{
    if (listPluginParams == nullptr || slotId < 0)
        return {};

    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr || ! slot->isPlugin())
        return {};      // an internal synth has three targets, and they are built in

    return listPluginParams (slotId);
}

juce::String PianoRollComponent::pluginParamName (int slotId, int paramIndex) const
{
    if (paramIndex < 0)
        return {};

    for (const auto& info : pluginParamsForSlot (slotId))
        if (info.index == paramIndex)
            return info.name;

    return {};
}

juce::String PianoRollComponent::noteTargetLabel (const NoteEvent& note) const
{
    if (! note.drivesPluginParam())
        return modTargetLabel (note.modTarget);

    const auto name = pluginParamName (resolvedSlotForNote (note), note.modPluginParam);

    // The plugin is gone, or its parameter list changed under the project. The
    // curve is still there and still says what it says; it just cannot be
    // named. Saying so beats an empty cell.
    return name.isNotEmpty() ? name : utf8 ("音源参数");
}

juce::String PianoRollComponent::groupTargetLabel (const NoteGroup& group) const
{
    if (expandedGroupTarget != (int) GroupAutomationTarget::plugin)
        return expandedGroupTarget == (int) GroupAutomationTarget::pan ? utf8 ("组声相")
                                                                      : utf8 ("组音量");

    const auto name = pluginParamName (resolvedSlotForGroup (expandedGroupLayerIndex, group.getId()),
                                       group.getPluginParam());

    return name.isNotEmpty() ? name : utf8 ("音源参数");
}

bool PianoRollComponent::addPluginParamSubmenu (juce::PopupMenu& menu, int slotId,
                                                int currentIndex, int firstItemId)
{
    const auto params = pluginParamsForSlot (slotId);

    if (params.empty())
        return false;

    juce::PopupMenu sub;

    // A hosted VST3 can expose two thousand parameters (JUCE's host-side
    // wrapper lists a block of them for MIDI-style control on top of whatever
    // the vendor wrote). A menu that long is unusable and takes a visible
    // moment to lay out, so the list is capped: the first 64 are the ones a
    // vendor parameter list starts with, and the rest stay reachable through
    // the plugin's own editor window.
    const int listed = juce::jmin ((int) params.size(), kMaxListedPluginParams);

    for (int i = 0; i < listed; ++i)
        sub.addItem (firstItemId + i, params[(size_t) i].name,
                     true, params[(size_t) i].index == currentIndex);

    if ((int) params.size() > listed)
        sub.addItem (-1, utf8 ("另有 ") + juce::String ((int) params.size() - listed)
                            + utf8 (" 个参数 · 用音源自己的窗口"), false, false);

    menu.addSubMenu (utf8 ("音源参数") + "  ·  " + rack.getSlotName (slotId), sub, true);
    return true;
}

void PianoRollComponent::toggleSelection (int noteId)
{    if (isSelected (noteId))
        selection.erase (std::remove (selection.begin(), selection.end(), noteId), selection.end());
    else
        selection.push_back (noteId);

    // Phase 28: touching a note lets go of the clips — see setSampleSelection.
    sampleSelection.clear();

    notifySelectionChanged();
    repaint();
}

//==============================================================================
//  Phase 16: note groups
//==============================================================================
int PianoRollComponent::groupSelection()
{
    auto* layer = stack.getActiveLayer();

    // Phase 35: a group is "the members", not "the notes". A selection holding
    // only sample blocks used to do nothing here — G pressed and no group
    // appeared, because the button was asking a question about notes.
    if (layer == nullptr || (selection.empty() && sampleSelection.empty()))
        return -1;

    // The name/colour cycle follows the number of groups that already exist, so
    // a session reads A, B, C… even after a few have been dissolved.
    const int ordinal = (int) layer->getGroups().size();
    const int groupId = layer->addGroup (groupPalette::defaultName (ordinal),
                                          groupPalette::nextGroupColour (ordinal));

    layer->assignNotesToGroup (selection, groupId);

    // Both kinds, one id. The second line is the whole of "采样建组": membership
    // already lived on the clip (Phase 27 saved it, Phase 27's menu set it), the
    // only thing missing was a way to say it without finding an existing group.
    layer->assignSamplesToGroup (sampleSelection, groupId);

    // Phase 62: pin where this appearance sits NOW, while "the earliest member"
    // still means what it says. Deriving it later would let the first deleted
    // note take the whole appearance with it.
    layer->occurrenceStart (groupId, 0);

    // Re-grouping may have emptied the group these notes came from. Prune it
    // here as well as on delete: an empty group is a row in the automation
    // window that nobody can find in the roll.
    pruneEmptyGroups();

    // One notification for the whole gesture: the engine republishes once, the
    // undo history takes one step, and no view sees a half-built group.
    stack.notifyNotesChanged();
    repaint();
    return groupId;
}

int PianoRollComponent::getSelectedGroupId() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    // Phase 35: samples are members too, so a selection of blocks can be "the
    // selection's group" just as well as a selection of notes. A MIXED selection
    // has to answer with the group both halves belong to; if one half is loose
    // the honest answer is -1, because "group this" would then mean two things.
    int groupId = -1;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr)
            continue;

        if (note->groupId < 0)
            return -1;

        if (groupId < 0)
            groupId = note->groupId;
        else if (groupId != note->groupId)
            return -1;
    }

    for (int id : sampleSelection)
    {
        const int clipGroup = layer->getGroupIdOfSample (id);

        if (clipGroup < 0)
            return -1;

        if (groupId < 0)
            groupId = clipGroup;
        else if (groupId != clipGroup)
            return -1;
    }

    return groupId;
}

int PianoRollComponent::getSelectedGroupOccurrence() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    // Phase 58: the selection also answers "WHICH copy", because copying a
    // group has to copy the copy the user selected — not all four of them. A
    // selection that straddles two appearances is not one object, so it answers
    // -1 and the copy falls back to the loose-notes rule.
    int occurrence = -1;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr || note->groupId < 0)
            return -1;

        if (occurrence < 0)
            occurrence = note->groupOccurrence;
        else if (occurrence != note->groupOccurrence)
            return -1;
    }

    for (int id : sampleSelection)
    {
        const auto* clip = layer->findSample (id);

        if (clip == nullptr || clip->groupId < 0)
            return -1;

        if (occurrence < 0)
            occurrence = clip->groupOccurrence;
        else if (occurrence != clip->groupOccurrence)
            return -1;
    }

    return occurrence;
}

bool PianoRollComponent::isWholeGroupSelected (int groupId, bool includeSamples,
                                               int occurrence) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0)
        return false;

    // Every member has to be in the selection, and nothing else may be — a
    // selection that also holds a loose note is two actions, not one.
    //
    // Phase 58: "every member" means every member of ONE appearance when
    // `occurrence` is given. Selecting the chorus copy alone is a whole object,
    // even though the group it belongs to has three more copies elsewhere.
    const int notes   = occurrence < 0 ? layer->countNotesInGroup (groupId)
                                       : layer->countNotesInOccurrence (groupId, occurrence);
    const int clips   = ! includeSamples ? 0
                        : (occurrence < 0 ? layer->countSamplesInGroup (groupId)
                                          : layer->countSamplesInOccurrence (groupId, occurrence));
    const int members = notes + clips;
    const int chosen  = (int) selection.size() + (includeSamples ? (int) sampleSelection.size() : 0);

    if (selection.empty() && (! includeSamples || sampleSelection.empty()))
        return false;

    if (members <= 0 || chosen != members)
        return false;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr || note->groupId != groupId)
            return false;

        if (occurrence >= 0 && note->groupOccurrence != occurrence)
            return false;
    }

    if (includeSamples)
        for (int id : sampleSelection)
        {
            const auto* clip = layer->findSample (id);

            if (clip == nullptr || clip->groupId != groupId)
                return false;

            if (occurrence >= 0 && clip->groupOccurrence != occurrence)
                return false;
        }

    return true;
}

std::vector<std::pair<int, int>> PianoRollComponent::getSelectedGroups() const
{
    std::vector<std::pair<int, int>> groups;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return groups;

    // The selection only ever holds ids of the ACTIVE layer (switching sheets
    // clears it), so this is one layer's worth of groups, not the session's.
    for (const auto& group : layer->getGroups())
    {
        for (int occurrence : layer->getGroupOccurrences (group.getId()))
        {
            // Blocks count: the user selects a group by clicking its label or by
            // dragging it, and a group of samples has no notes to select.
            //
            // Phase 63: per appearance. Asking `isWholeGroupSelected` here made
            // "select two groups" light up NEITHER row in the group panel — the
            // same whole-selection-vs-one-appearance confusion as P63-A.
            if (isWholeOccurrenceSelected (group.getId(), occurrence))
            {
                groups.push_back ({ stack.getActiveLayerIndex(), group.getId() });
                break;
            }
        }
    }

    return groups;
}

void PianoRollComponent::selectGroup (int groupId, int occurrence)
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        if (layer->findGroup (groupId) == nullptr)
            continue;

        // A group belongs to exactly one layer, and only the active layer is
        // clickable, so selecting it also means bringing that sheet forward.
        if (i != stack.getActiveLayerIndex())
            stack.setActiveLayerIndex (i);

        // Phase 35: BOTH kinds, in one write.
        //
        // "Select the whole group" has to mean the whole group. Setting only the
        // notes left the group's blocks dark while the bracket said they were
        // in it, and — worse — the next Delete then removed half of what the
        // highlight appeared to cover. setMixedSelection exists for exactly this
        // (Phase 29 built it for mixed copy), and it is the only way to write
        // both lists without one clearing the other.
        // Phase 58: with an occurrence, only that copy; without one, the whole
        // group (every copy) — which is what a caller holding just an id means.
        if (occurrence < 0)
            setMixedSelection (layer->getGroupNoteIds (groupId),
                              layer->getGroupSampleIds (groupId));
        else
            setMixedSelection (layer->getOccurrenceNoteIds (groupId, occurrence),
                              layer->getOccurrenceSampleIds (groupId, occurrence));

        return;
    }
}

bool PianoRollComponent::ungroupSelection()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || (selection.empty() && sampleSelection.empty()))
        return false;

    bool changed = false;

    // Phase 62: taking a member out of the shape takes its twin out too, or the
    // next edit would read the difference as "this member was deleted" and
    // remove it from the other appearance behind the user's back.
    std::vector<int> twinNotes, twinClips;
    collectTwinsForEdit (twinNotes, twinClips);

    const auto detach = [layer, &changed] (int id, bool isNote)
    {
        if (isNote)
        {
            if (auto* note = layer->findNote (id); note != nullptr && note->groupId >= 0)
            {
                note->groupId         = -1;
                note->groupOccurrence = 0;      // Phase 58: leave no stale appearance
                changed = true;
            }
        }
        else if (auto* clip = layer->findSample (id); clip != nullptr && clip->groupId >= 0)
        {
            clip->groupId         = -1;
            clip->groupOccurrence = 0;
            changed = true;
        }
    };

    for (int id : selection)
        detach (id, true);

    for (int id : twinNotes)
        detach (id, true);

    // Phase 35: Shift+G on a mixed selection has to take the blocks out too, or
    // the group would still exist — held open by members the user just asked to
    // leave, and still counted in its bracket.
    for (int id : sampleSelection)
        detach (id, false);

    for (int id : twinClips)
        detach (id, false);

    if (changed)
    {
        pruneEmptyGroups();
        stack.notifyNotesChanged();
        repaint();
    }

    return changed;
}

bool PianoRollComponent::disbandGroup (int layerIndex, int groupId, int occurrence)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || layer->findGroup (groupId) == nullptr)
        return false;

    //-- Phase 58: one copy, or the whole definition? ------------------------
    // With several appearances on the sheet, "解散编组" on the chorus copy must
    // not take the verse copy's group away with it — the user pointed at one
    // object. So a single occurrence is DETACHED, and the definition is only
    // dropped when this was the last thing holding it (or when no occurrence
    // was named at all, which is the old whole-group verb).
    if (occurrence >= 0 && layer->getGroupOccurrences (groupId).size() > 1)
    {
        const auto noteIds = layer->getOccurrenceNoteIds (groupId, occurrence);
        const auto clipIds = layer->getOccurrenceSampleIds (groupId, occurrence);

        layer->assignNotesToGroup (noteIds, -1);
        layer->assignSamplesToGroup (clipIds, -1);

        stack.notifyNotesChanged();
        repaint();
        return true;
    }

    // Tell the host FIRST: it owns the automation window and has to drop the
    // group's track before the id stops resolving. Doing it afterwards leaves a
    // row pointing at nothing for one repaint.
    if (onGroupRemoved != nullptr)
        onGroupRemoved (groupId);

    layer->removeGroup (groupId);

    // Notes the group used to hold are still selected; that is deliberate —
    // dissolving a group should not also lose your place.
    stack.notifyNotesChanged();
    repaint();
    return true;
}

bool PianoRollComponent::setGroupOutputForSelection (int slotId)
{
    const int groupId = getSelectedGroupId();

    if (groupId < 0)
        return false;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        if (auto* group = layer->findGroup (groupId))
        {
            group->setOutputSlotId (slotId < 0 ? -1 : slotId);
            stack.notifyNotesChanged();
            repaint();
            return true;
        }
    }

    return false;
}

int PianoRollComponent::getGroupLabelAt (juce::Point<float> position, int* occurrence) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& group : layer->getGroups())
        {
            // Phase 58: every appearance has its own pill, and the one the
            // pointer is over is the one the click is about.
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                // Phase 21: the pill now ends in a handle, and that handle is
                // its own hit target. Without this exclusion clicking it would
                // select the group instead of opening the curve — which is
                // exactly the bug the note's tail button already taught us to
                // avoid.
                if (getGroupAutomationHandleBounds (i, group.getId(), which).contains (position))
                    continue;

                if (getGroupLabelBounds (i, group.getId(), which).contains (position))
                {
                    if (occurrence != nullptr)
                        *occurrence = which;

                    return group.getId();
                }
            }
        }
    }

    return -1;
}

int PianoRollComponent::getGroupFrameAt (juce::Point<float> position, int* occurrence) const
{
    // Only the active layer: a ghost group is not editable, and a click on one
    // that picked the group up would drag notes the user cannot even select.
    const int  activeIndex = stack.getActiveLayerIndex();
    const auto* layer      = stack.getLayer (activeIndex);

    if (layer == nullptr)
        return -1;

    for (const auto& group : layer->getGroups())
    {
        // Phase 58: one box per appearance. The pointer is inside at most one of
        // them, so "which group" is no longer a good enough answer — the caller
        // also needs "which copy of it".
        for (int which : layer->getGroupOccurrences (group.getId()))
        {
            // Phase 57: the same box the frame is drawn with (groupFrameOf), so
            // the pointer agrees with the line — the old 3px expansion would
            // have the group grabbed from outside its own border.
            const auto content = groupFrameOf (activeIndex, group.getId(), which);

            if (content.isEmpty())
                continue;

            if (content.contains (position))
            {
                if (occurrence != nullptr)
                    *occurrence = which;

                return group.getId();
            }
        }
    }

    return -1;
}

bool PianoRollComponent::isGroupFocused (int layerIndex, int groupId, int occurrence) const
{
    if (focusSlotId < 0)
        return false;

    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId
             && (occurrence < 0 || note.groupOccurrence == occurrence)
             && routing::resolve (*layer, note, rack).slotId == focusSlotId)
            return true;

    return false;
}

juce::Rectangle<float> PianoRollComponent::getGroupContentBounds (const Layer& layer,
                                                                 const NoteGroup& group,
                                                                 int occurrence) const
{
    const auto area = getGridArea();
    juce::Rectangle<float> unionBounds;

    // Phase 58: -1 means "the whole group" (every appearance at once), which is
    // what a caller that only knows the group id wants; anything else picks ONE
    // appearance out, and those are the rectangles the user sees.
    const auto belongs = [&group, occurrence] (int groupId, int memberOccurrence)
    {
        return groupId == group.getId() && (occurrence < 0 || memberOccurrence == occurrence);
    };

    for (const auto& note : layer.getNotes())
    {
        if (! belongs (note.groupId, note.groupOccurrence))
            continue;

        const auto bounds = getNoteBounds (note);

        if (! bounds.intersects (area))
            continue;

        unionBounds = unionBounds.isEmpty() ? bounds : unionBounds.getUnion (bounds);
    }

    // Phase 27: clips are members too, so the bracket has to reach them.
    // Otherwise "group the clip with these notes" would draw a frame around
    // the notes with the clip sticking out of it, and dragging that frame would
    // leave the clip behind.
    for (const auto& sample : layer.getSamples())
    {
        if (! belongs (sample.groupId, sample.groupOccurrence))
            continue;

        const auto bounds = getSampleScreenBounds (layerIndexOf (&layer), sample.id);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        unionBounds = unionBounds.isEmpty() ? bounds : unionBounds.getUnion (bounds);
    }

    return unionBounds;
}

int PianoRollComponent::layerIndexOf (const Layer* layer) const
{
    if (layer == nullptr)
        return -1;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (stack.getLayer (i) == layer)
            return i;

    return -1;
}

juce::Rectangle<float> PianoRollComponent::getGroupLabelBounds (int layerIndex, int groupId,
                                                              int occurrence) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return {};

    // Phase 58: a pill per appearance. Two copies of "副歌" on the sheet are two
    // objects with two grab areas, not one label floating over both.
    const auto content = getGroupContentBounds (*layer, *group, occurrence);

    if (content.isEmpty())
        return {};

    const auto area = getGridArea();
    const float height = ui::pf (14);

    const juce::String text = groupLabelText (*layer, *group);

    juce::Font font (juce::FontOptions (atomic::ui::sp (9.5f)));

    // Measured with GlyphArrangement rather than Font::getStringWidthFloat —
    // that one is deprecated in JUCE 8 (it cannot account for the fallback that
    // draws CJK) and this project builds warning-free.
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);

    // Phase 66: the pill may not be wider than the group it names. A group
    // 0.2 beats long is about 8 px at the default zoom, while the minimum pill
    // is 34 px — the name tag then hangs out over whatever sits to the right,
    // and which group it belongs to stops being readable. Clamped to the
    // group's own width the tag is exactly as wide as the bracket under it;
    // `drawText` with useEllipses = false trims the text for us.
    const float width = juce::jmin (juce::jlimit (ui::pf (34), ui::pf (168),
                                                  glyphs.getBoundingBox (0, -1, true).getWidth()
                                                    + ui::pf (16) + ui::pf (13)),
                                    content.getWidth());

    // Above the topmost member, and inside the grid when the group sits against
    // the very top row — a label half off the canvas would be unclickable.
    float labelY = content.getY() - height - ui::pf (2);

    if (labelY < area.getY())
        labelY = juce::jmin (area.getBottom() - height, content.getY() + ui::pf (2));

    float labelX = content.getX();

    if (labelX + width > area.getRight())
        labelX = juce::jmax (area.getX(), area.getRight() - width);

    return { labelX, labelY, width, height };
}

juce::Rectangle<float> PianoRollComponent::getGroupAutomationHandleBounds (int layerIndex,
                                                                          int groupId,
                                                                          int occurrence) const
{
    const auto pill = getGroupLabelBounds (layerIndex, groupId, occurrence);

    if (pill.isEmpty())
        return {};

    // The right end of the pill, inset so the pill's rounded corner still
    // reads: a handle flush with the edge looks like a clipped button.
    const float w = ui::pf (12);

    // Phase 66: a pill narrower than the group now can be narrower than this
    // handle too — placing it anyway puts a 12 px button outside the tag it
    // belongs to. Below that width there is no handle: the tag is a name, and
    // a group that narrow is not a place where two controls fit anyway.
    if (pill.getWidth() < w + ui::pf (6))
    {
        return {};
    }
    return { pill.getRight() - w - ui::pf (1.5f), pill.getY() + ui::pf (1.5f),
             w, juce::jmax (ui::pf (8), pill.getHeight() - ui::pf (3)) };
}

int PianoRollComponent::getFocusedNoteCount() const
{
    if (focusSlotId < 0)
        return 0;

    int count = 0;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
            if (routing::resolve (*layer, note, rack).slotId == focusSlotId)
                ++count;
    }

    return count;
}

void PianoRollComponent::setFocusSlotId (int slotId)
{
    if (focusSlotId == slotId)
        return;

    focusSlotId = slotId;
    repaint();
}

bool PianoRollComponent::dismissSpotlight()
{
    if (focusSlotId < 0)
        return false;

    // The host owns the rack's own highlight indicator, so it gets told first;
    // without a host (or if it declines) the view still has to recover, because
    // a mode the user cannot leave is a bug no matter who else is asleep.
    if (onClearSpotlight != nullptr)
        onClearSpotlight();

    setFocusSlotId (-1);
    return true;
}

int PianoRollComponent::layerIndexOfGroup (int groupId) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i);
            layer != nullptr && layer->findGroup (groupId) != nullptr)
            return i;

    return -1;
}

void PianoRollComponent::pruneEmptyGroups()
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr || layer->getGroups().empty())
            continue;

        // Collected first: removeGroup() rewrites the vector this loop reads.
        std::vector<int> empty;

        for (const auto& group : layer->getGroups())
            if (layer->countNotesInGroup (group.getId()) == 0
                 && layer->countSamplesInGroup (group.getId()) == 0)   // Phase 27: clips count
                empty.push_back (group.getId());

        for (int groupId : empty)
        {
            if (onGroupRemoved != nullptr)
                onGroupRemoved (groupId);

            layer->removeGroup (groupId);
        }

        // Phase 62: an appearance that just lost its last member must not leave
        // its origin behind — the next appearance to take that number would
        // inherit a position that was never its own.
        layer->pruneOccurrenceStarts();
    }
}

void PianoRollComponent::fillGroupMenu (juce::PopupMenu& menu, int layerIndex, int groupId,
                                        int occurrence) const
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return;

    // Phase 58: the counts belong to the copy under the pointer. With one
    // appearance they are the group's counts, exactly as before; with several,
    // "12 音符" for a 3-note copy would be a lie about what the verbs below do.
    const bool perCopy = occurrence >= 0;
    const int  members = perCopy ? layer->countNotesInOccurrence (groupId, occurrence)
                                 : layer->countNotesInGroup (groupId);
    const int  clips   = perCopy ? layer->countSamplesInOccurrence (groupId, occurrence)
                                 : layer->countSamplesInGroup (groupId);   // Phase 27

    const int copies = (int) layer->getGroupOccurrences (groupId).size();

    menu.addSectionHeader (group->getName() + utf8 (" · ")
                             + juce::String (members) + utf8 (" 音符")
                             + (clips > 0 ? utf8 (" · ") + juce::String (clips) + utf8 (" 采样")
                                          : juce::String())
                             + (copies > 1 && perCopy
                                  ? utf8 (" · 第 ") + juce::String (occurrence + 1) + utf8 (" 处")
                                  : juce::String()));
    menu.addItem (1, utf8 ("选中整组"));
    // Phase 56: the group as one object, moved one bar on. Not "复制" + "粘贴":
    // that spends the clipboard and lands wherever the playhead is, and the
    // point of this item is that the answer is already known.
    menu.addItem (8, utf8 ("复制到后一小节"));
    menu.addItem (2, utf8 ("编组挂载音源…"));
    // Phase 20: two parameters, and the tick means "this curve already has a
    // row in the automation window", so the menu doubles as the index.
    // Phase 42: one verb, one entry — the two parameters are the submenu.
    {
        juce::PopupMenu autoMenu;
        autoMenu.addItem (3, utf8 ("组音量"), true, group->getLevelAutomation().isEnabled());
        autoMenu.addItem (4, utf8 ("组声相"), true, group->getPanAutomation().isEnabled());
        menu.addSubMenu (utf8 ("创建自动化"), autoMenu);
    }
    // Phase 35: the tick is the state, so the menu is also the readout — the
    // same convention the two automation rows above already use.
    menu.addItem (6, utf8 ("静音（音符与采样一起）"), true, group->isMuted());
    // Phase 50: freeze the group into one block. The render keeps the group's
    // own curves, its mount and its trim, because the host publishes the same
    // schedule the sound card would hear — this is not a second mixer.
    menu.addItem (7, utf8 ("渲染为采样…"));
    menu.addSeparator();
    menu.addItem (5, utf8 ("解散编组（音符保留）"));
}

void PianoRollComponent::showGroupMenu (int layerIndex, int groupId, int occurrence,
                                        juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    fillGroupMenu (menu, layerIndex, groupId, occurrence);

    // Nothing to show means the group went away between the click and here.
    if (menu.getNumItems() == 0)
        return;

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, layerIndex, groupId, occurrence, screenPos] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            switch (result)
                            {
                                case 1: safe->selectGroup (groupId, occurrence); break;

                                case 8:
                                    safe->duplicateGroupToNextBar (layerIndex, groupId, occurrence);
                                    break;

                                case 2:
                                    if (safe->onGroupMountRequested != nullptr)
                                        safe->onGroupMountRequested (layerIndex, groupId, screenPos);
                                    break;

                                case 3:
                                case 4:
                                    if (safe->onGroupAutomationRequested != nullptr)
                                        safe->onGroupAutomationRequested (layerIndex, groupId,
                                                                          result == 4 ? 1 : 0);
                                    break;

                                case 5:
                                    safe->disbandGroup (layerIndex, groupId, occurrence);
                                    break;

                                case 6:
                                    // Re-resolved: the menu can outlive the group
                                    // it was opened on (undo, another window).
                                    if (auto* owner = safe->stack.getLayer (layerIndex))
                                        if (auto* live = owner->findGroup (groupId))
                                        {
                                            live->setMuted (! live->isMuted());
                                            safe->stack.notifyNotesChanged();
                                            safe->repaint();
                                        }
                                    break;

                                case 7:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex      = layerIndex;
                                        request.groupId         = groupId;
                                        request.groupOccurrence = occurrence;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                default: break;
                            }
                        });
}

void PianoRollComponent::addRenderScopeItems (juce::PopupMenu& menu, bool hasSelection)
{
    menu.addItem (7, utf8 ("渲染选中内容为采样…"), hasSelection);
    menu.addItem (8, utf8 ("渲染整个图层为采样…"), true);
}

void PianoRollComponent::showCanvasMenu (juce::Point<int> screenPos)
{
    const int count    = (int) selection.size();
    const int clips    = (int) sampleSelection.size();   // Phase 35
    const int groupId  = getSelectedGroupId();
    const int ownerLayer = groupId >= 0 ? layerIndexOfGroup (groupId) : -1;

    juce::PopupMenu menu;
    menu.addSectionHeader (count > 0 || clips > 0
                             ? juce::String (count) + utf8 (" 个音符")
                                 + (clips > 0 ? utf8 (" · ") + juce::String (clips) + utf8 (" 个采样")
                                              : juce::String())
                             : juce::String (utf8 ("钢琴卷帘")));

    menu.addItem (1, utf8 ("编组选中内容"), count > 0 || clips > 0);
    menu.addItem (2, utf8 ("选中整组"), groupId >= 0);
    menu.addItem (3, utf8 ("取消编组"), groupId >= 0);
    menu.addSeparator();
    menu.addItem (4, utf8 ("编组挂载音源…"), groupId >= 0);
    {
        // Phase 42: the same verb / parameters split as everywhere else.
        juce::PopupMenu autoMenu;
        autoMenu.addItem (5, utf8 ("组音量"), groupId >= 0);
        autoMenu.addItem (6, utf8 ("组声相"), groupId >= 0);
        menu.addSubMenu (utf8 ("创建自动化"), autoMenu, groupId >= 0);
    }

    //-- Phase 50: bounce to a sample block ----------------------------------
    // The selection and the whole layer are here; a group has its own menu
    // entry (right-clicking its pill), which is where "this group" is obvious.
    // Built by a named function so the self-test can read the menu instead of
    // trusting a screenshot of a popup (P43's rule).
    menu.addSeparator();
    addRenderScopeItems (menu, count > 0 || clips > 0);

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, groupId, ownerLayer, screenPos] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            switch (result)
                            {
                                case 1: safe->groupSelection(); break;
                                case 2: safe->selectGroup (groupId); break;
                                case 3: safe->ungroupSelection(); break;

                                case 4:
                                    if (safe->onGroupMountRequested != nullptr)
                                        safe->onGroupMountRequested (ownerLayer, groupId, screenPos);
                                    break;

                                case 5:
                                case 6:
                                    if (safe->onGroupAutomationRequested != nullptr)
                                        safe->onGroupAutomationRequested (ownerLayer, groupId,
                                                                          result == 6 ? 1 : 0);
                                    break;

                                // Phase 50: the id list is COPIED here. The
                                // render runs on its own thread, and the
                                // selection is free to change while it does.
                                case 7:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex = safe->stack.getActiveLayerIndex();
                                        request.noteIds    = safe->selection;
                                        request.sampleIds  = safe->sampleSelection;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                case 8:
                                    if (safe->onRenderScopeRequested != nullptr)
                                    {
                                        RenderScopeRequest request;
                                        request.layerIndex = safe->stack.getActiveLayerIndex();
                                        request.wholeLayer = true;
                                        safe->onRenderScopeRequested (request);
                                    }
                                    break;

                                default: break;
                            }
                        });
}

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
    grabKeyboardFocus();

    if (onCanvasMouseDown != nullptr)
        onCanvasMouseDown();

    const auto pos = e.getPosition();

    // ---- Tool strip ---------------------------------------------------------
    if (getToolbarArea().contains (pos))
    {
        static const Tool tools[5] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand,
                                     Tool::knife };

        for (int i = 0; i < 5; ++i)
            if (getToolButtonBounds (i).contains (pos))
            {
                setTool (tools[i]);
                return;
            }

        if (getToolButtonBounds (5).contains (pos))
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

        if (getToolButtonBounds (6).contains (pos))
        {
            // Phase 16. With nothing selected there is nothing to group, and
            // silently doing nothing is worse than saying so in the strip.
            // Phase 38: "something" means either kind — a clip-only selection
            // used to reach this line and get nothing.
            if (hasAnySelection())
                groupSelection();

            return;
        }

        if (getToolButtonBounds (7).contains (pos))
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

        if (getToolButtonBounds (8).contains (pos))
        {
            setSnapEnabled (! snapEnabled);
            repaint (getToolbarArea());
            return;
        }

        return;
    }

    // ---- Horizontal scrollbar ----------------------------------------------
    if (getScrollBarArea().contains (pos))
    {
        const auto thumb = getScrollThumb();

        if (thumb.expanded (4.0f, 2.0f).contains (e.position))
        {
            dragMode = DragMode::scrollThumb;
            scrollDragAnchorX = e.position.x;
            scrollDragAnchorBeats = scrollBeats;
        }
        else
        {
            // Clicking the empty track jumps there, centring the thumb.
            const auto track = getScrollBarArea().reduced (2).toFloat();
            const double visible = getVisibleBeats();
            const float span = juce::jmax (1.0f, track.getWidth() - thumb.getWidth());
            const float t = juce::jlimit (0.0f, 1.0f,
                                          (e.position.x - track.getX() - thumb.getWidth() * 0.5f) / span);
            setScrollBeats ((double) t * juce::jmax (0.0, timelineBeats - visible));
        }

        repaint();
        return;
    }

    // ---- Phase 22: the piano column is a keyboard, not a graphic -----------
    // Before the ruler and the grid: the keys are outside both, and the old
    // code simply fell through to "not in the grid, ignore". A pitch grid you
    // cannot hear while pointing at it is a picture of a keyboard.
    if (getKeyboardArea().contains (pos))
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
    if (getRulerArea().contains (pos))
    {
        if (e.mods.isPopupMenu())
        {
            // Right button: zoom. Anchored on the value the drag started with,
            // so the scale cannot wind up or down as the mouse jitters.
            //
            // Phase 31: and anchored on the BEAT UNDER THE POINTER, which is the
            // point of the gesture. A right-drag on a ruler is "pull this moment
            // apart" — the moment under the mouse is the one that must not move.
            dragMode = DragMode::rulerZoom;
            rulerDragAnchorY      = e.position.y;
            rulerDragAnchorBeatWidth = beatWidth;
            rulerZoomAnchorX      = e.position.x;
            rulerZoomAnchorBeat   = beatAtScreenX (e.position.x);
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        dragMode = DragMode::rulerScrub;
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

        // Phase 67: one function, so a click and a drag cannot disagree about
        // where the playhead is.
        scrubRulerTo (e.position.x);

        return;
    }

    if (! getGridArea().contains (e.position))
        return;

    // ---- Phase 29: the way out of the spotlight ----------------------------
    // Checked before everything else in the grid: the chip is an override, and
    // a click that lands on it must not also move something out from under it.
    if (focusSlotId >= 0 && getSpotlightExitChipBounds().toFloat().contains (e.position))
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
            if (getNoteAutomationBoxBounds (*expanded).contains (e.position))
            {
                // Phase 22: a click inside a box (re)selects its curve, whether
                // that click lands on a point, the parameter cell or the frame.
                curveSelection = CurveSelection::noteEnvelope;
                handleAutomationMouseDown (e, *expanded);
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
                  .contains (e.position))
            {
                curveSelection = CurveSelection::groupLane;
                handleGroupAutomationMouseDown (e);
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
                  .contains (e.position))
            {
                handleSampleAutomationMouseDown (e, expandedSampleLayerIndex, expandedSampleId);
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
            if (! getGroupAutomationHandleBounds (i, group.getId()).contains (e.position))
                continue;

            if (expandedGroupId == group.getId() && expandedGroupLayerIndex == i)
                closeGroupAutomation();
            else
                openGroupAutomation (i, group.getId(), expandedGroupTarget);

            return;
        }
    }

    // ---- Phase 16: a group's label is a click target of its own ------------
    // Checked before the notes because the pill sits above them: clicking the
    // name is the obvious way to say "the whole group", and it costs one hit
    // test rather than a modifier key nobody would guess.
    int labelOccurrence = 0;

    if (const int groupId = getGroupLabelAt (e.position, &labelOccurrence); groupId >= 0)
    {
        const int layerIndex = layerIndexOfGroup (groupId);

        if (e.mods.isPopupMenu())
        {
            showGroupMenu (layerIndex, groupId, labelOccurrence, e.getScreenPosition());
            return;
        }

        armGroupMove (groupId, labelOccurrence, e.position);
        return;
    }

    // ---- Phase 17: an audio clip under the cursor --------------------------
    // Before the notes: a block is the size of several lanes, so it is the
    // thing you are aiming at when both are under the pointer.
    if (auto* clipUnder = hitTestSample (e.position))
    {
        const int clipId = clipUnder->id;

        //-- Phase 58: a SELECTED block's edge outranks the hit --------------
        // The hit test can only answer with ONE block, and with two blocks
        // overlapping the pixel you aimed at may belong to a stranger that is
        // drawn on top. "I selected three and dragged one edge" then became
        // "I dragged the stranger" — the trim looked like it failed.
        // The pointer's own cursor already says "trim band", so ask that
        // question first: if the edge belongs to something selected, that edge
        // is what the gesture is about, and the selection must survive.
        const auto edge     = selectedTrimEdgeAt (e.position);
        const int  edgeId   = edge.first;
        const bool edgeTail = edge.second;
        const int  grabbedId = edgeId >= 0 ? edgeId : clipId;

        // Phase 28: the click is also the selection — except on a trim grab
        // that belongs to an already-selected block, where rewriting the
        // selection would throw away exactly the blocks the trim is for.
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
            toggleSampleSelection (clipId);
        else if (edgeId < 0 && ! isSampleSelected (clipId))
            setSampleSelection ({ clipId });

        auto* grabbed = stack.getActiveLayer() != nullptr
                          ? stack.getActiveLayer()->findSample (grabbedId)
                          : nullptr;

        if (grabbed == nullptr)
            return;

        if (e.mods.isPopupMenu())
        {
            showSampleMenu (grabbedId, e.getScreenPosition());
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
            const double at = beatAtScreenX (e.position.x);

            splitSamplesAt ({ grabbedId }, snapEnabled ? snapBeat (at) : at);
            return;
        }

        const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), grabbedId);

        // Phase 49: the chevron on the block opens its curve box. Checked
        // before the trim bands because it sits between them — the same order
        // the note's tail button answers in.
        const auto handle = getSampleAutomationHandleBounds (*grabbed);

        if (! handle.isEmpty() && handle.contains (e.position))
        {
            if (expandedSampleId == grabbedId && expandedSampleLayerIndex == stack.getActiveLayerIndex())
                closeSampleAutomation();
            else
                openSampleAutomation (stack.getActiveLayerIndex(), grabbedId);

            return;
        }

        const float band   = ui::pf (kResizeHandleWidth);
        const bool  onTail = edgeId >= 0 ? edgeTail : (e.position.x > bounds.getRight() - band);
        const bool  onHead = edgeId >= 0 ? ! edgeTail
                                         : (! onTail && e.position.x < bounds.getX() + band);

        // Phase 28/29: a grab on a block's edge is a trim of the SELECTION
        // (Phase 56 widened it from "the grabbed block only"); a grab anywhere
        // else arms a RIGID move of the whole selection, clips and notes
        // together.
        if ((onTail || onHead) && isSampleSelected (grabbedId))
        {
            dragSampleId       = grabbedId;
            dragSampleOriginal = *grabbed;
            dragAnchorBeat     = beatAtScreenX (e.position.x);
            dragMode           = onTail ? DragMode::resizeSample : DragMode::resizeSampleLeft;

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
        else
        {
            armSampleMove (e.position);
        }

        repaint();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        // Right-click anywhere else: the group actions for whatever the
        // selection currently is. Before Phase 16 this click was simply ignored.
        showCanvasMenu (e.getScreenPosition());
        return;
    }

    // ---- Phase 8: the "v" handle at the note's tail -------------------------
    if (auto* noteUnderMouse = hitTestActiveLayer (e.position))
    {
        const auto handle = getNoteAutomationHandleBounds (*noteUnderMouse);

        if (! handle.isEmpty() && handle.contains (e.position))
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
        panAnchor = e.position;
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
        if (auto* hit = hitTestActiveLayer (e.position))
        {
            const double at = beatAtScreenX (e.position.x);

            splitNotesAt ({ hit->id }, snapEnabled ? snapBeat (at) : at);
            return;
        }

        return;     // a knife click on empty canvas cuts nothing
    }

    // ---- Eraser: one click, one note ---------------------------------------
    if (tool == Tool::eraser)
    {
        if (auto* hit = hitTestActiveLayer (e.position))
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
    if (tool == Tool::pencil && hitTestActiveLayer (e.position) == nullptr)
    {
        // Phase 11: the note is stamped, not invented — if a state was copied
        // off another note it carries that length, velocity, curve and synth.
        if (const int newId = addNoteAt (beatAtScreenX (e.position.x),
                                          pitchAtScreenY (e.position.y));
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
    if (auto* hit = hitTestActiveLayer (e.position))
    {
        const int  hitId = hit->id;
        const int  hitPitch = hit->pitch;
        const auto noteBounds = getNoteBounds (*hit);
        const float band = ui::pf (kResizeHandleWidth);

        // Phase 19: both ends resize. The right end keeps priority when the note
        // is short enough that the two bands would overlap.
        const bool onRightEdge = e.position.x > noteBounds.getRight() - band;
        const bool onLeftEdge  = ! onRightEdge && e.position.x < noteBounds.getX() + band;

        if (e.mods.isShiftDown() || e.mods.isCommandDown())
            toggleSelection (hitId);
        else if (! isSelected (hitId))
            setSelection ({ hitId });

        // Alt-drag: leave the original where it is and drag a copy — which
        // carries the envelope, because the copy is a whole NoteEvent.
        if (e.mods.isAltDown() && ! onRightEdge && ! onLeftEdge)
        {
            const auto copies = duplicateSelectedNotes (0.0);

            if (! copies.empty())
                setSelection (copies);
        }

        if (isSelected (hitId))
        {
            dragMode = onRightEdge ? DragMode::resizeNote
                       : (onLeftEdge ? DragMode::resizeNoteLeft : DragMode::moveNote);
            dragAnchorBeat = beatAtScreenX (e.position.x);
            dragAnchorPitch = pitchAtScreenY (e.position.y);
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
    if (! e.mods.isShiftDown())
    {
        int frameOccurrence = 0;

        if (const int frameGroup = getGroupFrameAt (e.position, &frameOccurrence); frameGroup >= 0)
        {
            armGroupMove (frameGroup, frameOccurrence, e.position);
            return;
        }
    }

    // ---- Empty grid: marquee over the ACTIVE layer only ----------------------
    // Phase 30: and the grid itself ends the spotlight. "Click somewhere that is
    // not a note" is the gesture everyone tries first. Checked HERE, after every
    // hit test, so clicking a note or a clip inside the highlight never
    // dismisses it.
    dismissSpotlight();

    if (! e.mods.isShiftDown())
    {
        selection.clear();
        sampleSelection.clear();     // Phase 28: clicking the grid lets go of both
        notifySelectionChanged();
    }

    dragMode = DragMode::marquee;
    marqueeStart = marqueeCurrent = e.position;
    repaint();
}

void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! getGridArea().contains (e.position))
        return;

    auto* hit = hitTestActiveLayer (e.position);

    if (hit == nullptr)      // double-clicking empty space does nothing
        return;

    dragMode = DragMode::none;
    dragOriginals.clear();
    dragResizeNoteId = -1;

    if (onNoteDoubleClicked != nullptr)
        onNoteDoubleClicked (stack.getActiveLayerIndex(), hit->id);
}

//==============================================================================
int PianoRollComponent::addNoteAt (double beat, int pitch)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return -1;

    // The template is a whole NoteEvent, so the copy carries everything: the
    // length, the velocity, the curve inside it, the parameter that curve drives
    // and the synth override. Pitch and position are the only things the click
    // decides, because those are what the user is pointing at.
    NoteEvent note = hasPaintState ? paintState : NoteEvent {};
    note.pitch       = juce::jlimit (0, numPitches - 1, pitch);
    note.lengthBeats = juce::jmax (gridDivision, note.lengthBeats);
    note.velocity    = juce::jlimit (0.0f, 1.0f, note.velocity);

    const double raw = snapEnabled ? snapBeat (beat) : beat;
    note.startBeat = juce::jmax (0.0, raw);

    // Landing past the end is how the timeline grows while drawing.
    ensureTimelineExtendsTo (note.startBeat + note.lengthBeats + 4.0);
    note.startBeat = juce::jmin (note.startBeat,
                                 juce::jmax (0.0, timelineBeats - gridDivision));

    return active->addNote (note);
}

//==============================================================================
bool PianoRollComponent::copyNoteStateFromSelection()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // First selected note in model order, not in click order: "the state I
    // copied" must not depend on which note happened to be clicked last.
    for (const auto& note : active->getNotes())
    {
        if (! isSelected (note.id))
            continue;

        paintState = note;
        hasPaintState = true;

        // Select/tool switch is the caller's business, but the strip has to
        // repaint either way: the button lights up and the hint text changes.
        repaint();
        return true;
    }

    return false;
}

void PianoRollComponent::clearPencilState()
{
    if (! hasPaintState)
        return;

    hasPaintState = false;
    paintState = NoteEvent {};
    repaint();
}

std::vector<int> PianoRollComponent::duplicateSelectedNotes (double offsetBeats)
{
    std::vector<int> newIds;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return newIds;

    // Snapshot before mutating: addNote() push_backs into the same vector we
    // would otherwise be iterating.
    std::vector<NoteEvent> originals;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            originals.push_back (note);

    for (auto copy : originals)
    {
        copy.startBeat = juce::jlimit (0.0, juce::jmax (0.0, timelineBeats - copy.lengthBeats),
                                       copy.startBeat + offsetBeats);

        // Whole-struct copy: geometry, velocity AND the envelope. This one
        // line is why automation cannot get detached from its note.
        newIds.push_back (active->addNote (copy));
    }

    if (! newIds.empty())
    {
        setSelection (newIds);
        stack.notifyNotesChanged();
    }

    return newIds;
}

PianoRollComponent::MoveClamp PianoRollComponent::clampMove (const std::vector<NoteEvent>& originals,
                                                            double rawBeats, int rawPitch,
                                                            bool snap,
                                                            const std::vector<SampleEvent>& clips) const
{
    MoveClamp move;
    move.beats = rawBeats;
    move.pitch = rawPitch;

    if (originals.empty() && clips.empty())
        return move;

    // Phase 27: the block is notes AND clips. Both are measured into the same
    // min/max, because a group that clamps against its notes alone would let a
    // clip slide off the top of the grid while its bracket stopped moving.
    bool   haveAny  = false;
    double minStart = 0.0;
    int    minPitch = 0;
    int    maxPitch = 0;

    const auto measure = [&] (double start, int lowPitch, int highPitch)
    {
        if (! haveAny)
        {
            haveAny  = true;
            minStart = start;
            minPitch = lowPitch;
            maxPitch = highPitch;
            return;
        }

        minStart = juce::jmin (minStart, start);
        minPitch = juce::jmin (minPitch, lowPitch);
        maxPitch = juce::jmax (maxPitch, highPitch);
    };

    for (const auto& origin : originals)
        measure (origin.startBeat, origin.pitch, origin.pitch);

    for (const auto& clip : clips)
        measure (clip.startBeat, clip.pitch, clip.topPitch());

    // Snapping applies to the BLOCK, anchored on its leftmost member. Snapping
    // each one separately would round a deliberately off-grid member into place
    // and quietly rewrite the spacing of the thing being dragged.
    if (snap)
        move.beats = snapBeat (minStart + move.beats) - minStart;

    // The leftmost member is the one that decides: it stops at beat 0 and takes
    // everyone with it. Same on the way right, and same vertically — the block
    // keeps its shape and stops.
    move.beats = juce::jmax (move.beats, -minStart);

    const double latestStart = juce::jmax (0.0, timelineBeats - gridDivision);

    if (latestStart > minStart)
        move.beats = juce::jmin (move.beats, latestStart - minStart);

    move.pitch = juce::jlimit (-minPitch, numPitches - 1 - maxPitch, move.pitch);

    return move;
}

void PianoRollComponent::captureDragOriginals()
{
    dragOriginals.clear();
    dragClipOriginals.clear();
    dragAppliedMoveBeats = 0.0;      // Phase 62: a new gesture measures from zero

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            dragOriginals.push_back (note);
}

void PianoRollComponent::captureGroupClips (int groupId, int occurrence)
{
    // Phase 65: kept as the "one appearance" spelling of the capture, but the
    // gesture now arms EVERY selected block. A click on a group label is a
    // click on one group, yet what moves is the selection — narrowing it to the
    // group under the pointer is exactly what made "select two groups, drag one"
    // drop the other.
    juce::ignoreUnused (groupId, occurrence);

    captureSampleDragOriginals();
}

void PianoRollComponent::applyMove (const MoveClamp& move)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& origin : dragOriginals)
        if (auto* note = active->findNote (origin.id))
        {
            note->startBeat = origin.startBeat + move.beats;
            note->pitch     = origin.pitch + move.pitch;
        }

    for (const auto& origin : dragClipOriginals)
        if (auto* clip = active->findSample (origin.id))
        {
            clip->startBeat = juce::jmax (0.0, origin.startBeat + move.beats);

            // A block keeps its whole lane span, so its BOTTOM lane is what is
            // clamped — the same rule the single-clip drag uses. Clamping the
            // top instead would let a tall block hang off the bottom row.
            clip->pitch = juce::jlimit (0, numPitches - juce::jmax (1, clip->laneSpan),
                                        origin.pitch + move.pitch);

            ensureTimelineExtendsTo (clip->endBeat());
        }

    applyOccurrenceOriginsForMove (move.beats, active);

    // ...and the shape half, live: dragging one member of an appearance is an
    // edit of the group, so the other appearances move with the pointer instead
    // of snapping into place on mouse-up.
    syncDragShapes();
}

bool PianoRollComponent::syncDragShapes()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || (dragOriginals.empty() && dragClipOriginals.empty()))
        return false;

    std::vector<int> noteIds, clipIds;

    for (const auto& note : dragOriginals)
        noteIds.push_back (note.id);

    for (const auto& clip : dragClipOriginals)
        clipIds.push_back (clip.id);

    const auto whole    = wholeGroupOccurrencesInDrag();
    const auto touched  = groupOccurrencesOf (noteIds, clipIds);

    bool changed = false;

    for (const auto& entry : touched)
    {
        // A carry of the whole appearance is a change of position, not of shape.
        if (std::find (whole.begin(), whole.end(), entry) != whole.end())
            continue;

        changed = layer->syncGroupShape (entry.first, entry.second) || changed;
    }

    if (changed)
        stack.notifyNotesChanged();

    return changed;
}

void PianoRollComponent::applyOccurrenceOriginsForMove (double movedBeats, Layer* layer)
{
    // `move.beats` is measured from the ORIGINALS, not from the last mouse-move,
    // so the origin has to be moved by the difference — adding the whole delta
    // on every mouse-move would walk the appearance off the timeline.
    const double step = movedBeats - dragAppliedMoveBeats;

    dragAppliedMoveBeats = movedBeats;

    if (layer == nullptr || std::abs (step) < 1.0e-12)
        return;

    //-- Only an appearance carried WHOLE keeps its shape ---------------------
    // Dragging all of it is a change of POSITION: the origin travels with it and
    // the shape (member offsets from that origin) is untouched. Dragging part of
    // it is a change of SHAPE, which is Phase 62's other half — the siblings get
    // rebuilt from it at the end of the gesture, and this origin must stay put
    // so that "the shape moved inside its own box" is what gets copied.
    for (const auto& entry : wholeGroupOccurrencesInDrag())
    {
        const double origin = layer->occurrenceStart (entry.first, entry.second);
        layer->setOccurrenceStart (entry.first, entry.second, origin + step);
    }
}

std::vector<std::pair<int, int>> PianoRollComponent::wholeGroupOccurrencesInDrag() const
{
    std::vector<std::pair<int, int>> whole;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return whole;

    const auto addIfWhole = [&] (int groupId, int occurrence)
    {
        if (groupId < 0)
            return;

        int carried = 0;

        for (const auto& note : dragOriginals)
            if (note.groupId == groupId && note.groupOccurrence == occurrence)
                ++carried;

        for (const auto& clip : dragClipOriginals)
            if (clip.groupId == groupId && clip.groupOccurrence == occurrence)
                ++carried;

        const int members = layer->countNotesInOccurrence (groupId, occurrence)
                              + layer->countSamplesInOccurrence (groupId, occurrence);

        if (members > 0 && carried == members
              && std::find (whole.begin(), whole.end(),
                            std::make_pair (groupId, occurrence)) == whole.end())
            whole.push_back ({ groupId, occurrence });
    };

    for (const auto& note : dragOriginals)
        addIfWhole (note.groupId, note.groupOccurrence);

    for (const auto& clip : dragClipOriginals)
        addIfWhole (clip.groupId, clip.groupOccurrence);

    return whole;
}

std::vector<std::pair<int, int>> PianoRollComponent::groupOccurrencesOf (const std::vector<int>& noteIds,
                                                                        const std::vector<int>& clipIds) const
{
    std::vector<std::pair<int, int>> occurrences;

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return occurrences;

    const auto add = [&occurrences] (int groupId, int occurrence)
    {
        if (groupId < 0)
            return;

        const auto entry = std::make_pair (groupId, occurrence);

        if (std::find (occurrences.begin(), occurrences.end(), entry) == occurrences.end())
            occurrences.push_back (entry);
    };

    for (int id : noteIds)
        if (const auto* note = layer->findNote (id))
            add (note->groupId, note->groupOccurrence);

    for (int id : clipIds)
        if (const auto* clip = layer->findSample (id))
            add (clip->groupId, clip->groupOccurrence);

    return occurrences;
}

bool PianoRollComponent::syncGroupShapesAfterEdit (const std::vector<std::pair<int, int>>& touched)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || touched.empty())
        return false;

    bool changed = false;

    for (const auto& entry : touched)
        changed = layer->syncGroupShape (entry.first, entry.second) || changed;

    if (changed)
        stack.notifyNotesChanged();

    return changed;
}

void PianoRollComponent::collectTwinsForEdit (std::vector<int>& twinNotes,
                                              std::vector<int>& twinClips) const
{
    twinNotes.clear();
    twinClips.clear();

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    for (const auto& entry : groupOccurrencesOf (selection, sampleSelection))
    {
        const int groupId    = entry.first;
        const int occurrence = entry.second;

        // An appearance the selection carries WHOLE is a per-copy decision:
        // deleting the chorus copy must not delete the verse. Only a PART of an
        // appearance is a change of shape.
        //
        // Phase 63: asked per appearance. `isWholeGroupSelected` answers "no"
        // as soon as a second group is in the selection, which turned every
        // multi-group edit into "sync to all my twins".
        if (isWholeOccurrenceSelected (groupId, occurrence))
            continue;

        // Ask per appearance: `collectTwinMembers` pairs by ordinal, and the
        // ordinal only means something inside one appearance.
        std::vector<int> sourceNotes, sourceClips;

        for (int id : selection)
            if (const auto* note = layer->findNote (id); note != nullptr
                  && note->groupId == groupId && note->groupOccurrence == occurrence)
                sourceNotes.push_back (id);

        for (int id : sampleSelection)
            if (const auto* clip = layer->findSample (id); clip != nullptr
                  && clip->groupId == groupId && clip->groupOccurrence == occurrence)
                sourceClips.push_back (id);

        std::vector<int> twinsOfNotes, twinsOfClips;
        layer->collectTwinMembers (sourceNotes, sourceClips, twinsOfNotes, twinsOfClips);

        for (int id : twinsOfNotes)
            if (std::find (twinNotes.begin(), twinNotes.end(), id) == twinNotes.end())
                twinNotes.push_back (id);

        for (int id : twinsOfClips)
            if (std::find (twinClips.begin(), twinClips.end(), id) == twinClips.end())
                twinClips.push_back (id);
    }
}

bool PianoRollComponent::simulateGroupMove (int layerIndex, int groupId,
                                            double deltaBeats, int deltaPitch,
                                            int occurrence)
{    if (stack.getActiveLayerIndex() != layerIndex)
        return false;
    auto* active = stack.getActiveLayer();

    if (active == nullptr || active->findGroup (groupId) == nullptr)
        return false;

    // Same rule as armGroupMove: a selection that already holds this appearance
    // whole is kept, so the self-test drives the gesture the user gets.
    if (! isWholeOccurrenceSelected (groupId, occurrence))
        selectGroup (groupId, occurrence);

    captureDragOriginals();
    captureGroupClips (groupId, occurrence);

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return false;

    const auto move = clampMove (dragOriginals, deltaBeats, deltaPitch, false, dragClipOriginals);
    applyMove (move);
    stack.notifyNotesChanged();

    return true;
}

juce::Rectangle<float> PianoRollComponent::groupFrameOf (int layerIndex, int groupId,
                                                         int occurrence) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    return group != nullptr ? getGroupContentBounds (*layer, *group, occurrence)
                            : juce::Rectangle<float>();
}

void PianoRollComponent::armGroupMove (int groupId, int occurrence, juce::Point<float> position)
{
    // Selecting is not moving: a group whose only gesture is "become the
    // selection" reads as broken the moment you try to drag it. So the same
    // click arms a rigid move of every member — the rules are unchanged, it is
    // still clampMove() over dragOriginals, exactly like dragging one of its
    // notes.
    //
    // Phase 58: ONE appearance, not the group. Grabbing the chorus copy must
    // leave the verse copy standing — that is the whole point of a reusable
    // group, and it is why the occurrence travels down into the capture.
    //
    // Phase 65: ...unless this appearance is ALREADY selected whole, which is
    // what a multi-selection looks like. Then grabbing its label means "drag
    // what I have selected", and replacing the selection with this one group
    // would drop the other groups on the floor at the moment the pointer moves.
    // "Select two groups, drag one" moves both; a click on a group that is not
    // part of the selection still narrows to it, exactly as before.
    if (! isWholeOccurrenceSelected (groupId, occurrence))
        selectGroup (groupId, occurrence);

    captureDragOriginals();
    captureGroupClips (groupId, occurrence);

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return;      // the group has no member on this sheet

    dragMode         = DragMode::moveNote;
    dragAnchorBeat   = beatAtScreenX (position.x);
    dragAnchorPitch  = pitchAtScreenY (position.y);

    if (! dragOriginals.empty())
        dragAuditionPitch = dragOriginals.front().pitch;

    dragResizeNoteId = -1;
}

int PianoRollComponent::resizeTargetId() const
{
    // The note whose end band was grabbed. Falls back to the first captured
    // note so a programmatic resize (a test arming the mode by hand) still
    // resizes something instead of silently doing nothing.
    for (const auto& origin : dragOriginals)
        if (origin.id == dragResizeNoteId)
            return dragResizeNoteId;

    return dragOriginals.empty() ? -1 : dragOriginals.front().id;
}

bool PianoRollComponent::applyNoteResize (const NoteEvent& origin, double wantedBeat, bool fromTail)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* note = active->findNote (origin.id);

    if (note == nullptr)
        return false;

    if (fromTail)
    {
        // The head does not move, so this is one subtraction with a floor — and
        // the floor is a grid step, not zero: a note that can be shrunk to
        // nothing is a note you can lose with a twitch. Phase 69: that step is
        // the VISIBLE one when it is finer than the 1/16, so a 1/64 grid can
        // produce a 1/64 note.
        note->lengthBeats = juce::jmax (minimumNoteLength(), wantedBeat - origin.startBeat);
        syncShapeOfMember (origin.id);
        return true;
    }

    // Trim the front: the tail stays where it was, which is what makes "make
    // this start later" not also mean "make this end later".
    const double end      = origin.startBeat + origin.lengthBeats;
    const double newStart = juce::jlimit (0.0,
                                          juce::jmax (0.0, end - minimumNoteLength()),
                                          wantedBeat);

    note->startBeat   = newStart;
    note->lengthBeats = juce::jmax (minimumNoteLength(), end - newStart);
    syncShapeOfMember (origin.id);
    return true;
}

bool PianoRollComponent::syncShapeOfMember (int noteId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    const auto* note = layer->findNote (noteId);

    if (note == nullptr || note->groupId < 0)
        return false;

    const int groupId = note->groupId;
    const int occurrence = note->groupOccurrence;

    if (! layer->syncGroupShape (groupId, occurrence))
        return false;

    stack.notifyNotesChanged();
    return true;
}

bool PianoRollComponent::syncShapesOfMembers (const std::vector<int>& noteIds,
                                             const std::vector<int>& clipIds)
{
    return syncGroupShapesAfterEdit (groupOccurrencesOf (noteIds, clipIds));
}

bool PianoRollComponent::syncShapeOfClip (int clipId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    const auto* clip = layer->findSample (clipId);

    if (clip == nullptr || clip->groupId < 0)
        return false;

    const int groupId = clip->groupId;
    const int occurrence = clip->groupOccurrence;

    if (! layer->syncGroupShape (groupId, occurrence))
        return false;

    stack.notifyNotesChanged();
    return true;
}

bool PianoRollComponent::simulateNoteResize (int noteId, double wantedBeat, bool fromTail)
{
    captureDragOriginals();
    dragResizeNoteId = noteId;

    const int target = resizeTargetId();
    bool changed = false;

    for (const auto& origin : dragOriginals)
        if (origin.id == target)
        {
            changed = applyNoteResize (origin, wantedBeat, fromTail);
            break;
        }

    dragOriginals.clear();
    dragResizeNoteId = -1;
    stack.notifyNotesChanged();
    return changed;
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

        case DragMode::marquee:
            marqueeCurrent = e.position;
            repaint();
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
    }

    dragMode = DragMode::none;
    dragOriginals.clear();
    dragClipOriginals.clear();     // Phase 28: armed by a clip move as well
    dragTrimOriginals.clear();     // Phase 56: armed by a clip trim as well
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
    if (wasRulerDrag)
        updateCursor();

    repaint();
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
                const bool overMenu = getAutomationMenuArea (*expanded).contains (e.position);
                const int  overPoint = getAutomationCurveArea (*expanded).contains (e.position)
                                         ? findEnvelopePointAt (*expanded, e.position) : -1;

                if (overMenu != hoveredAutomationMenu || overPoint != hoveredEnvelopePoint)
                {
                    hoveredAutomationMenu  = overMenu;
                    hoveredEnvelopePoint   = overPoint;
                    repaint (box.expanded (6.0f).getSmallestIntegerContainer());
                }

                setMouseCursor (overMenu ? juce::MouseCursor::PointingHandCursor
                                         : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                           : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredAutomationMenu || hoveredEnvelopePoint != -1)
    {
        hoveredAutomationMenu = false;
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
                const bool overMenu = getGroupMenuArea().contains (e.position);
                const int  overPoint = getGroupCurveBounds().contains (e.position)
                                         ? findGroupCurvePointAt (e.position) : -1;

                if (overMenu != hoveredGroupMenu)
                {
                    hoveredGroupMenu = overMenu;
                    repaint (box.expanded (6.0f).getSmallestIntegerContainer());
                }

                setMouseCursor (overMenu ? juce::MouseCursor::PointingHandCursor
                                         : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                           : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredGroupMenu)
    {
        hoveredGroupMenu = false;
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
                const bool  overMenu  = getSampleMenuArea (expandedSampleLayerIndex,
                                                           expandedSampleId).contains (e.position);
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

                setMouseCursor (overMenu ? juce::MouseCursor::PointingHandCursor
                                         : (overPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                           : juce::MouseCursor::CrosshairCursor));
                return;
            }
        }
    }

    if (hoveredSampleCurveMenu || hoveredSampleCurvePoint != -1)
    {
        hoveredSampleCurveMenu  = false;
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
        if (selectedTrimEdgeAt (e.position).first >= 0)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), clip->id);
        const float band  = ui::pf (kResizeHandleWidth);
        const bool  onEnd = e.position.x > bounds.getRight() - band
                              || e.position.x < bounds.getX() + band;

        setMouseCursor (onEnd ? juce::MouseCursor::LeftRightResizeCursor
                              : juce::MouseCursor::PointingHandCursor);

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
         || hoveredAutomationMenu || hoveredEnvelopePoint != -1
         || hoveredGroupId != -1 || hoveredGroupHandle || hoveredGroupMenu
         || hoveredSampleHandle || hoveredSampleCurveMenu
         || hoveredSampleCurvePoint != -1)
    {
        hoveredNoteId = -1;
        hoveredTool = -1;
        hoveredAutomationMenu = false;
        hoveredEnvelopePoint  = -1;
        hoveredGroupId        = -1;
        hoveredGroupLayer     = -1;
        hoveredGroupHandle    = false;
        hoveredGroupMenu      = false;
        hoveredSampleHandle     = false;
        hoveredSampleHandleId   = -1;
        hoveredSampleCurveMenu  = false;
        hoveredSampleCurvePoint = -1;
        repaint();
    }
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

//==============================================================================
//  Painting
//==============================================================================
void PianoRollComponent::resized()
{
    clampScroll();
}

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (kRollBackground);

    const auto area = getGridArea();

    g.setColour (kGridBackground);
    g.fillRect (area);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());

        drawPitchRows (g);
        drawTimeGrid (g);
        drawGridLines (g);
        drawGroupBands (g);       // Phase 16: a frame around each group
        drawSamples (g);          // Phase 17: audio blocks sit under the notes
        drawNotes (g);
        drawGroupFrames (g);      // Phase 57: the frame's outline, ON the members
        drawGroupLabels (g);      // ... and their name pills in front
        drawFocusHighlights (g);  // the focused synth's notes, on top of all
        drawSpotlightExitChip (g); // Phase 29: and the way back out of the veil
        drawMarquee (g);
        drawDropPreview (g);      // Phase 17: where a dragged sample would land
        drawPlayhead (g);
    }

    drawToolbar (g);
    drawRuler (g);
    drawKeyboard (g);
    drawScrollBar (g);

    // The inline note automation box floats above the notes and is clipped to
    // the grid: it is anchored under a note, so it must not creep over the
    // keyboard or the ruler when that note sits near an edge.
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        drawNoteAutomationBox (g);
        drawGroupAutomationBox (g);   // Phase 21: the same, for a group
        drawSampleAutomationBox (g);  // Phase 49: and for a clip's own curve
    }

    // Seams between the panes.
    g.setColour (kOutline);
    g.drawVerticalLine (keyboardWidth, 0.0f, (float) getHeight());
    g.drawHorizontalLine (toolbarHeight, 0.0f, (float) getWidth());
    g.drawHorizontalLine (toolbarHeight + rulerHeight, 0.0f, (float) getWidth());
    g.drawHorizontalLine (getHeight() - scrollbarHeight, 0.0f, (float) getWidth());
}

void PianoRollComponent::drawToolbar (juce::Graphics& g)
{
    auto bar = getToolbarArea();

    g.setColour (kRulerBackground);
    g.fillRect (bar);

    // Phase 8: icons instead of words. A DAW tool strip is read at a glance
    // while the eye is on the notes, and four two-character labels need a
    // second look every time.
    // Phase 51: the knife is the fifth tool — a mode, exactly like the pencil.
    const Tool tools[5] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand, Tool::knife };

    const auto tint = theme::accent;

    for (int i = 0; i < 5; ++i)
    {
        const bool active  = tool == tools[i];
        const bool hovered = hoveredTool == i;
        const auto bounds  = getToolButtonBounds (i).toFloat();

        g.setColour (active ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        if (active)
        {
            g.setColour (tint.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);
        }

        drawToolIcon (g, i, bounds, active ? juce::Colours::white : kText);
    }

    //-- Copy-state button -----------------------------------------------------
    // Not a mode: an action with a lit "loaded" state. It says "画笔状态" on it
    // because a dropper icon alone reads as "pick colour" to everyone who has
    // used a paint program.
    {
        const auto bounds  = getToolButtonBounds (5).toFloat();
        const bool hovered = hoveredTool == 5;
        const bool loaded  = hasPaintState;

        g.setColour (loaded ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (loaded ? tint.withAlpha (0.85f)
                            : juce::Colours::white.withAlpha (hovered ? 0.20f : 0.10f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 5, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      loaded ? juce::Colours::white : kText);

        g.setColour (loaded ? juce::Colours::white : kText);
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("状态"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 16: group button ----------------------------------------------
    // An action, not a mode, exactly like the dropper above it. It stays lit
    // while the selection is inside one group, because that is when clicking it
    // again is the "pick the whole group up" gesture.
    {
        const auto bounds  = getToolButtonBounds (6).toFloat();
        const bool hovered = hoveredTool == 6;
        const bool inGroup = getSelectedGroupId() >= 0;
        const bool enabled = hasAnySelection();

        g.setColour (inGroup ? tint.withAlpha (0.22f)
                             : juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (inGroup ? tint.withAlpha (0.85f)
                             : juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 6, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      inGroup ? juce::Colours::white : kText.withAlpha (enabled ? 1.0f : 0.4f));

        g.setColour (inGroup ? juce::Colours::white : kText.withAlpha (enabled ? 1.0f : 0.45f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("编组"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 20: ungroup ----------------------------------------------------
    // Same shape as 编组, dimmed until there is a selection — the pair reads as
    // one control with two directions, which is exactly what it is. Phase 21
    // put it directly right of 编组, ahead of the magnet.
    {
        const auto bounds  = getToolButtonBounds (7).toFloat();
        const bool hovered = hoveredTool == 7;
        const bool enabled = hasAnySelection();
        const bool grouped = getSelectedGroupId() >= 0;

        g.setColour (juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        drawToolIcon (g, 7, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      kText.withAlpha (enabled ? 1.0f : 0.4f));

        g.setColour (kText.withAlpha (enabled && grouped ? 1.0f : (enabled ? 0.72f : 0.45f)));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("解组"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
    }

    //-- Phase 18: snap toggle ------------------------------------------------
    // A toggle, not an action: it stays lit for as long as the grid is on, and
    // it switches the AUTOMATION window's grid too. One switch, one answer —
    // two independent snap flags is how a project ends up with notes on the
    // grid and curves half a tick off it.
    {
        const auto bounds  = getToolButtonBounds (8).toFloat();
        const bool hovered = hoveredTool == 8;
        const bool on      = snapEnabled;

        g.setColour (on ? tint.withAlpha (0.22f)
                        : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (4));

        g.setColour (on ? tint.withAlpha (0.85f)
                        : juce::Colours::white.withAlpha (0.20f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (4), 1.0f);

        // Phase 20: a magnet instead of the 3x3 dot grid. "Snap" is a thing
        // every DAW draws as a magnet, and a dot grid reads as "grid view".
        drawToolIcon (g, 7, bounds.withTrimmedLeft (ui::pf (7)).withWidth (ui::pf (18)),
                      on ? juce::Colours::white : kText.withAlpha (0.75f));

        g.setColour (on ? juce::Colours::white : kText.withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("吸附"),
                    bounds.withTrimmedLeft (ui::pf (26)), juce::Justification::centredLeft, false);
    }

    // A short reminder of what the current tool does, so the mode is never a
    // mystery after a click. With a paint state loaded, saying so matters more
    // than describing the tool: every click from now on stamps that note.
    const char* hints[4] { "单击空白处画音符 · 双击音符编辑包络",
                           "单击音符删除",
                           "拖动音符移动 · 空白处框选",
                           "拖动平移视图 · 滚轮上下 · Ctrl+滚轮或拖标尺缩放" };

    const auto hintArea = bar.withTrimmedLeft (getToolButtonBounds (numToolButtons - 1).getRight()
                                                 + ui::px (14));

    if (statusText.isNotEmpty())
    {
        // Phase 22: copy/paste are silent verbs. The tool hint steps aside for
        // a moment so the strip can say what actually happened.
        g.setColour (tint);
        g.setFont (boldFont (10.0f));
        g.drawText (statusText, hintArea, juce::Justification::centredLeft, true);
    }
    else if (hasPaintState)
    {
        g.setColour (tint.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
        g.drawText (utf8 ("画笔状态：")
                      + juce::String (paintState.lengthBeats, 2) + utf8 (" 拍 · vel ")
                      + juce::String (paintState.velocity, 2)
                      + (paintState.envelope.isEnabled() ? utf8 (" · 带包络") : juce::String())
                      + (paintState.outputSlotOverride >= 0
                           ? utf8 (" · 越级 ") + rack.getSlotName (paintState.outputSlotOverride)
                           : juce::String())
                      + utf8 ("   （再点一次清除）"),
                    hintArea, juce::Justification::centredLeft, true);
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            if (tools[i] == tool)
            {
                // Phase 16: with a selection, the strip also says what it is —
                // the count and whether it is already one group. That is the
                // readout that tells you whether the 编组 button will do
                // anything at all.
                juce::String text (utf8 (hints[i]));

                if (! selection.empty())
                {
                    const int groupId = getSelectedGroupId();

                    text << utf8 ("   ·   ") << juce::String ((int) selection.size())
                         << utf8 (" 个音符");

                    if (groupId >= 0)
                        text << utf8 (" · 已编组");
                }

                g.setColour (juce::Colours::white.withAlpha (0.38f));
                g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
                g.drawText (text, hintArea, juce::Justification::centredLeft, true);
            }
    }

    // Scroll position readout: the timeline now grows, so "how long is it" is a
    // number that changes while you work and has to be readable.
    g.setColour (kText.withAlpha (0.7f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f))));
    g.drawText (juce::String (scrollBeats, 1) + " / " + juce::String (timelineBeats, 0) + " beat",
                bar.withTrimmedRight (12), juce::Justification::centredRight, false);
}

void PianoRollComponent::drawScrollBar (juce::Graphics& g)
{
    auto area = getScrollBarArea();

    g.setColour (kRulerBackground);
    g.fillRect (area);

    const auto track = area.reduced (2).toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (track, track.getHeight() * 0.5f);

    const auto thumb = getScrollThumb();
    g.setColour (theme::accent.withAlpha (dragMode == DragMode::scrollThumb ? 0.75f : 0.45f));
    g.fillRoundedRectangle (thumb, thumb.getHeight() * 0.5f);
}

void PianoRollComponent::drawPitchRows (juce::Graphics& g)
{
    const auto area = getGridArea();

    const int topPitch    = contentYToPitch (scrollY);
    const int bottomPitch = contentYToPitch (scrollY + area.getHeight());

    for (int pitch = bottomPitch; pitch <= topPitch; ++pitch)
    {
        if (! isBlackKey (pitch))
            continue;

        g.setColour (kBlackRowColour);
        g.fillRect (area.getX(), screenYForPitch (pitch), area.getWidth(), keyHeight);
    }
}

void PianoRollComponent::drawTimeGrid (juce::Graphics& g)
{
    // A second is a musical quantity once a tempo exists: at 120 BPM it is two
    // beats, at 60 BPM one, at 180 BPM three. Drawing it means a tempo change
    // is immediately visible in the grid instead of only being audible.
    const double beatsPerSecond = bpm / 60.0;
    const double secondsPerBeat = 1.0 / beatsPerSecond;

    if (secondsPerBeat <= 1.0e-6)
        return;

    // Below about 20 px a second the lines would just muddy the 1/16 grid.
    const double pixelsPerSecond = secondsPerBeat * (double) beatWidth;

    if (pixelsPerSecond < 20.0)
        return;

    const auto area = getGridArea();
    const double visibleBeats = (double) area.getWidth() / beatWidth;

    const int firstSecond = (int) std::floor (scrollBeats / beatsPerSecond);
    const int lastSecond  = (int) std::ceil ((scrollBeats + visibleBeats) / beatsPerSecond);

    g.setColour (theme::accent.withAlpha (0.075f));

    for (int second = firstSecond; second <= lastSecond; ++second)
    {
        const double beat = (double) second * beatsPerSecond;

        if (beat < 0.0 || beat > timelineBeats)
            continue;

        g.drawVerticalLine ((int) std::floor (screenXForBeat (beat)),
                            area.getY(), area.getBottom());
    }
}

void PianoRollComponent::drawGridLines (juce::Graphics& g)
{
    const auto area = getGridArea();

    // Phase 68: the list already reflects the current step, so "skip the thin
    // lines when they are too close together" is a property of the geometry
    // rather than a check the painter has to remember.
    std::vector<double> lines;
    collectGridLines (scrollBeats, scrollBeats + (double) area.getWidth() / beatWidth, lines);

    for (double beat : lines)
    {
        const float x = std::floor (screenXForBeat (beat)) + 0.5f;

        g.setColour (juce::Colours::white.withAlpha (gridLineAlpha (beat)));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
    }
}

void PianoRollComponent::drawNotes (juce::Graphics& g)
{
    const auto area = getGridArea();
    const int activeIndex = stack.getActiveLayerIndex();

    // The playhead's position through a note is the same everywhere; computing
    // it here means the drawing code has no transport state of its own.
    auto progressOf = [this] (const NoteEvent& note)
    {
        if (! transportActive)
            return -1.0;

        return envelopeProgress (note, playheadBeat);
    };

    // --- Pass 1: every visible NON-active layer, at 30% ----------------------
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        if (i == activeIndex)
            continue;

        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
        {
            if (! getNoteBounds (note).intersects (area))
                continue;

            drawNote (g, note, layer->getColour(), ghostAlphaValue, false, false, progressOf (note));
        }
    }

    // --- Pass 2: the active layer, fully opaque, drawn last so it sits on top -
    // The eye button does not hide this one: it is the sheet being edited, and
    // an editor that can hide its own contents is a bug, not a feature.
    if (auto* active = stack.getActiveLayer())
    {
        for (const auto& note : active->getNotes())
        {
            if (! getNoteBounds (note).intersects (area))
                continue;

            drawNote (g, note, active->getColour(), solidAlpha,
                      isSelected (note.id), note.id == hoveredNoteId, progressOf (note));
        }
    }

    // --- Pass 3: there isn't one any more -----------------------------------
    // Phase 30 removed the spotlight's 46% black veil. Dimming the whole sheet
    // to answer "which notes are on this synth" made the sheet unusable for
    // everything else — you could not read, select or edit anything until you
    // found a way out. The focused notes are now marked POSITIVELY instead
    // (a tinted wash, a ring and a tab in the slot's colour, drawn in
    // drawFocusHighlights); nothing on the grid gets darker, so nothing becomes
    // unreadable.
}

//==============================================================================
//  Phase 16: group marks
//==============================================================================
void PianoRollComponent::drawGroupBands (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool  onActive = (i == activeIndex);
        const float baseAlpha = onActive ? 1.0f : ghostAlphaValue + 0.25f;

        // Phase 58: one band per APPEARANCE. A group reused four times is four
        // rectangles on the sheet and one definition behind them.
        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                // groupFrameOf, not getGroupContentBounds: the box the self-test
                // reads and the box that is painted have to be the same call, or
                // "the frame sits on the block" becomes a claim about a function
                // nobody draws with.
                const auto content = groupFrameOf (i, group.getId(), which);

                if (content.isEmpty())
                    continue;

                const bool focused = isGroupFocused (i, group.getId(), which);
                const auto colour  = group.getColour();

                // Phase 57: the band is the members' box, not the members' box
                // plus a margin — see drawGroupFrames for the outline it shares.
                const auto frame   = content;

                g.setColour (colour.withAlpha (baseAlpha * (focused ? 0.10f : 0.05f)));
                g.fillRoundedRectangle (frame, ui::pf (4));
            }
    }
}

void PianoRollComponent::drawGroupFrames (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool  onActive = (i == activeIndex);
        const float baseAlpha = onActive ? 1.0f : ghostAlphaValue + 0.25f;

        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
            {
                const auto content = groupFrameOf (i, group.getId(), which);

                if (content.isEmpty())
                    continue;

                const bool focused = isGroupFocused (i, group.getId(), which);
                const auto colour  = group.getColour();

                g.setColour (colour.withAlpha (baseAlpha * (focused ? 0.85f : 0.34f)));

                // Half a pixel in: a 1px stroke on the exact edge is drawn ON the
                // clip's own border and reads as "the clip got thicker" rather
                // than "this is the group". Reduced by the stroke's own
                // half-width, the two lines touch without merging — no gap, no
                // double line.
                const float stroke = focused ? ui::pf (1.4f) : ui::pf (1.0f);
                auto box = content.reduced (stroke * 0.5f);

                if (box.getWidth() <= 0.0f || box.getHeight() <= 0.0f)
                    box = content;      // one-pixel member: the frame is all there is

                g.drawRoundedRectangle (box, ui::pf (4), stroke);
            }
    }
}

void PianoRollComponent::drawGroupLabels (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getGroups().empty())
            continue;

        const bool onActive = (i == activeIndex);

        // Phase 58: a pill per appearance — each copy is its own grab area, so
        // each copy has its own name tag.
        for (const auto& group : layer->getGroups())
            for (int which : layer->getGroupOccurrences (group.getId()))
        {
            const auto pill = getGroupLabelBounds (i, group.getId(), which);

            if (pill.isEmpty())
                continue;

            const bool focused = isGroupFocused (i, group.getId(), which);
            const bool hovered = (hoveredGroupId == group.getId() && hoveredGroupLayer == i
                                    && hoveredGroupOccurrence == which);
            const auto colour  = group.getColour();

            const float alpha = onActive ? 1.0f : 0.55f;

            // Phase 35: a muted group is drawn muted. The pill keeps its colour
            // but loses most of its fill, because "this group is silent" has to
            // be visible on the thing that names it — a mute whose only witness
            // is a row in a side panel is a mute you will debug twice.
            const float pillAlpha = group.isMuted() ? alpha * 0.45f : alpha;

            // Focused and hovered pills are filled; the rest are outlined so a
            // screen with six groups is not six shouting tabs.
            if (focused || hovered)
            {
                g.setColour (colour.withAlpha (pillAlpha * (focused ? 0.92f : 0.75f)));
                g.fillRoundedRectangle (pill, ui::pf (4));
            }
            else
            {
                g.setColour (colour.withAlpha (pillAlpha * 0.14f));
                g.fillRoundedRectangle (pill, ui::pf (4));
                g.setColour (colour.withAlpha (pillAlpha * 0.55f));
                g.drawRoundedRectangle (pill, ui::pf (4), 1.0f);
            }

            const juce::String text = groupLabelText (*layer, group);

            g.setColour (focused || hovered ? theme::surface1
                                            : colour.withAlpha (pillAlpha));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (text, pill.reduced (ui::pf (5), 0.0f).withTrimmedRight (ui::pf (13)),
                        juce::Justification::centredLeft, false);

            //-- Phase 21: the curve handle ----------------------------------
            // The note's tail button is a small chevron on a dark square; this
            // is the same glyph in the same place, so "the thing at the right
            // end opens a curve" is learned once. The first version drew a tiny
            // curve icon here and at 12 px it read as the letter A.
            {
                const auto handle = getGroupAutomationHandleBounds (i, group.getId(), which);
                const bool open   = (expandedGroupId == group.getId()
                                       && expandedGroupLayerIndex == i
                                       && expandedGroupOccurrence == which);
                const bool hot    = hoveredGroupHandle
                                      && hoveredHandleGroupId == group.getId()
                                      && hoveredHandleLayer == i;

                // Phase 66: absent when the tag is too narrow to hold it.
                if (! handle.isEmpty())
                {
                    g.setColour (open ? colour.brighter (0.8f).withAlpha (alpha * 0.55f)
                                      : juce::Colours::black.withAlpha (alpha * 0.30f));
                    g.fillRoundedRectangle (handle, ui::pf (2.5f));

                    juce::Path chevron;
                    const float cx = handle.getCentreX();
                    const float cy = handle.getCentreY() + (open ? ui::pf (1.2f) : 0.0f);

                    chevron.startNewSubPath (cx - ui::pf (2.8f), cy - ui::pf (1.4f));
                    chevron.lineTo (cx, cy + ui::pf (1.8f));
                    chevron.lineTo (cx + ui::pf (2.8f), cy - ui::pf (1.4f));

                    g.setColour (juce::Colours::white.withAlpha (open || hot ? 0.95f : 0.7f));
                    g.strokePath (chevron, juce::PathStrokeType (ui::pf (1.2f),
                                                                 juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));
                }
            }

            // A mount marker: the pill's bottom edge is underlined in the
            // synth's own colour, so "this group plays on B" is visible without
            // opening anything. An underline rather than a dot because a filled
            // pill has no room for a dot that reads.
            if (! group.followsLayerOutput())
            {
                const auto* slot = rack.findSlot (group.getOutputSlotId());

                g.setColour ((slot != nullptr ? slot->colour : theme::record)
                                .withAlpha (alpha * 0.95f));
                g.fillRoundedRectangle (pill.withTrimmedTop (pill.getHeight() - ui::pf (2.5f))
                                            .withTrimmedLeft (ui::pf (4))
                                            .withTrimmedRight (ui::pf (15)),
                                        ui::pf (1.2));
            }
        }
    }
}

void PianoRollComponent::drawFocusHighlights (juce::Graphics& g)
{
    if (focusSlotId < 0)
        return;

    const auto* slot = rack.findSlot (focusSlotId);

    if (slot == nullptr)
        return;

    const auto ring = slot->colour;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        const bool onActive = (i == stack.getActiveLayerIndex());

        for (const auto& note : layer->getNotes())
        {
            if (routing::resolve (*layer, note, rack).slotId != focusSlotId)
                continue;

            const auto bounds = getNoteBounds (note);

            if (! bounds.intersects (getGridArea()))
                continue;

            // Phase 30: the emphasis is POSITIVE now that the veil is gone — a
            // tinted wash in the slot's colour instead of "everything else got
            // darker". A wash is read against the note's own colour, so it works
            // on all three layers and at any velocity, which a brightness tweak
            // on the body did not.
            g.setColour (ring.withAlpha (onActive ? 0.34f : 0.20f));
            g.fillRoundedRectangle (bounds.reduced (0.5f), 3.0f);

            // A soft halo, then a crisp edge. Two rings rather than a fatter
            // one: a single 4 px outline eats a 14 px note.
            g.setColour (ring.withAlpha (onActive ? 0.22f : 0.12f));
            g.drawRoundedRectangle (bounds.expanded (ui::pf (1.6)), ui::pf (4), ui::pf (2.0));

            // Phase 29: a SELECTED note keeps its white ring. The crisp edge
            // here sits exactly on top of it, and in a spotlit view that meant
            // "selected" was invisible — which is the one thing you are about to
            // act on. The halo and the tab still say which synth it plays on.
            const bool selectedNote = onActive && isSelected (note.id);

            if (! selectedNote)
            {
                g.setColour (ring.withAlpha (onActive ? 0.92f : 0.50f));
                g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, ui::pf (1.2));
            }

            // A 2 px tab on the left edge: the same mark at any zoom level, and
            // it survives the note being narrower than its own outline.
            g.fillRoundedRectangle (juce::Rectangle<float> (bounds.getX() - ui::pf (1),
                                                            bounds.getY() + ui::pf (1),
                                                            ui::pf (2.5),
                                                            juce::jmax (ui::pf (3), bounds.getHeight() - ui::pf (2))),
                                    ui::pf (1.2));
        }
    }
}

//==============================================================================
//  Phase 29: the way out of the spotlight
//==============================================================================
juce::Rectangle<int> PianoRollComponent::getSpotlightExitChipBounds() const
{
    if (focusSlotId < 0)
        return {};

    const auto area = getGridArea().toNearestInt().reduced (ui::px (8), ui::px (8));
    const int  w    = juce::jmin (area.getWidth(), ui::px (176));
    const int  h    = ui::px (24);

    // Bottom-right, not top-right (Phase 30). The top-right corner is where
    // clips and the higher melody notes live — the chip was covering the thing
    // it was meant to help you look at, and eating the clicks that landed on it.
    return juce::Rectangle<int> (juce::jmax (area.getX(), area.getRight() - w),
                                 juce::jmax (area.getY(), area.getBottom() - h), w, h);
}

void PianoRollComponent::drawSpotlightExitChip (juce::Graphics& g)
{
    const auto chip = getSpotlightExitChipBounds();

    if (chip.isEmpty())
        return;

    const auto box = chip.toFloat();

    // Dark and opaque: it is drawn on top of a 46% black veil, and a translucent
    // chip on a veil is exactly the thing the user cannot find.
    g.setColour (theme::surface3);
    g.fillRoundedRectangle (box, ui::pf (4));

    g.setColour (kPlayheadColour.withAlpha (0.80f));
    g.drawRoundedRectangle (box.reduced (0.5f), ui::pf (4), 1.0f);

    const juce::String name = rack.findSlot (focusSlotId) != nullptr
                                ? rack.getSlotName (focusSlotId)
                                : juce::String (focusSlotId);

    g.setColour (juce::Colours::white.withAlpha (0.94f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));

    // Phase 30: the chip no longer sits on a veil, so it has to name the exit
    // gestures that exist — the empty grid is one of them now.
    g.drawText (utf8 ("高亮 ") + name + utf8 ("   ·   点空白或 Esc 退出"),
                chip.withTrimmedLeft (ui::px (9)).withTrimmedRight (ui::px (6)),
                juce::Justification::centredLeft, true);
}

void PianoRollComponent::drawNote (juce::Graphics& g, const NoteEvent& note, juce::Colour colour,
                                   float alpha, bool selected, bool hovered, double progress)
{
    auto bounds = getNoteBounds (note);

    // Velocity modulates brightness, not opacity — opacity is reserved for
    // the ghost / solid distinction and must stay unambiguous.
    const auto tinted = colour.withMultipliedBrightness (0.78f + 0.35f * note.velocity);
    const bool sounding = progress >= 0.0;

    // Phase 22: a selected note is a brighter note. The outline alone was too
    // quiet a difference on a sheet with twenty notes on it.
    g.setColour ((selected ? tinted.brighter (0.30f) : tinted).withAlpha (alpha * 0.88f));
    g.fillRoundedRectangle (bounds, 3.0f);

    // Top highlight edge: makes stacked sheets readable even at 30%.
    g.setColour (tinted.brighter (0.55f).withAlpha (alpha * 0.95f));
    g.fillRoundedRectangle (bounds.withHeight (2.5f), 1.4f);

    g.setColour (tinted.darker (0.7f).withAlpha (alpha));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    // A notch in the target synth's colour: this note is bypassing its layer.
    // Same colours as the rack cards, so the destination is identifiable at a
    // glance without opening the inspector.
    if (note.outputSlotOverride >= 0 && bounds.getWidth() >= 14.0f)
    {
        const auto* target = rack.findSlot (note.outputSlotOverride);
        const auto badgeColour = target != nullptr ? target->colour : theme::record;

        juce::Path notch;
        notch.addTriangle (bounds.getRight() - 10.0f, bounds.getY() + 1.0f,
                           bounds.getRight() - 1.5f, bounds.getY() + 1.0f,
                           bounds.getRight() - 1.5f, bounds.getY() + 9.5f);

        g.setColour (badgeColour.withAlpha (alpha));
        g.fillPath (notch);
    }

    //-- The note's own automation, inside the block -------------------------
    // This is the visual proof of the binding: move or copy the block and the
    // curve moves with it. While the playhead is inside the note, the curve is
    // the thing being *played*, so it shows how far it has got.
    if (note.envelope.isEnabled() && note.envelope.getPoints().size() >= 2
         && bounds.getWidth() >= 10.0f)
    {
        const auto inner = bounds.reduced (1.5f, 2.0f);

        auto xForTime   = [&inner] (float t) { return inner.getX() + t * inner.getWidth(); };
        auto yForValue  = [&inner] (float v) { return inner.getBottom() - v * inner.getHeight(); };

        auto buildCurve = [&] (float from, float to)
        {
            juce::Path path;

            const int steps = juce::jlimit (2, 96, (int) (inner.getWidth() * (to - from) / 2.5f));

            for (int i = 0; i < steps; ++i)
            {
                const float t = from + (to - from) * (float) i / (float) (steps - 1);
                const float x = xForTime (t);
                const float y = yForValue (note.envelope.valueAt (t));

                if (i == 0) path.startNewSubPath (x, y);
                else        path.lineTo (x, y);
            }

            return path;
        };

        // Whole curve, dim: what the note will do.
        g.setColour (juce::Colours::white.withAlpha (alpha * (sounding ? 0.30f : 0.55f)));
        g.strokePath (buildCurve (0.0f, 1.0f), juce::PathStrokeType (1.0f));

        if (sounding)
        {
            const float p = (float) progress;

            // Area already covered by the envelope, filled at low alpha: the
            // "progress bar" of a single note.
            if (p > 0.001f)
            {
                auto filled = buildCurve (0.0f, p);
                filled.lineTo (xForTime (p), inner.getBottom());
                filled.lineTo (xForTime (0.0f), inner.getBottom());
                filled.closeSubPath();

                g.setColour (tinted.brighter (0.85f).withAlpha (alpha * 0.22f));
                g.fillPath (filled);

                // The part of the curve that has already played, at full
                // strength — this is the line "moving" under the playhead.
                g.setColour (juce::Colours::white.withAlpha (alpha * 0.95f));
                g.strokePath (buildCurve (0.0f, p), juce::PathStrokeType (1.6f));
            }

            // The head: a dot riding the curve at exactly the level the engine
            // is applying to this note right now.
            const float v  = note.envelope.valueAt (p);
            const float cx = xForTime (p);
            const float cy = yForValue (v);

            g.setColour (tinted.brighter (0.9f).withAlpha (alpha * 0.35f));
            g.fillEllipse (cx - 4.5f, cy - 4.5f, 9.0f, 9.0f);

            g.setColour (juce::Colours::white.withAlpha (alpha * 0.95f));
            g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        }
    }

    if (sounding)
    {
        // A lit edge marks the note as currently sounding, so its outline reads
        // at a glance even on a busy sheet.
        g.setColour (tinted.brighter (0.9f).withAlpha (alpha * 0.75f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.4f);
    }

    //-- Phase 8: the "v" handle at the tail ---------------------------------
    // Only the active layer gets one. A ghost note is click-through by design,
    // so offering it a button would break the one rule the roll never breaks.
    if (alpha >= 0.9f)
    {
        const auto handle = getNoteAutomationHandleBounds (note);

        if (! handle.isEmpty())
        {
            const bool open = note.id == expandedNoteId && expandedLayerIndex == stack.getActiveLayerIndex();

            g.setColour (open ? tinted.brighter (0.8f).withAlpha (0.55f)
                              : juce::Colours::black.withAlpha (0.30f));
            g.fillRoundedRectangle (handle, 2.5f);

            juce::Path chevron;
            const float cx = handle.getCentreX();
            const float cy = handle.getCentreY() + (open ? 1.5f : 0.0f);

            chevron.startNewSubPath (cx - 2.8f, cy - 1.4f);
            chevron.lineTo (cx, cy + 1.8f);
            chevron.lineTo (cx + 2.8f, cy - 1.4f);

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (chevron, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
        }
    }

    if (hovered && ! selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.2f);
    }

    //-- Phase 22: selection has to win at a glance --------------------------
    // Phase 8 marked a selected note with a 1.6 px outline, which on a busy
    // sheet was indistinguishable from the hover ring — "did I select that
    // one?" is not a question the editor should make you ask twice, because
    // the answer decides what the next drag moves. Three signals now: a
    // brighter body (above), a 2 px white ring, and four corner brackets
    // outside the block.
    if (selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.96f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 2.0f);

        if (bounds.getWidth() >= 12.0f && bounds.getHeight() >= 6.0f)
        {
            const float arm = 3.0f;
            const auto  box = bounds.expanded (1.5f);
            juce::Path brackets;

            brackets.startNewSubPath (box.getX(), box.getY() + arm);
            brackets.lineTo (box.getX(), box.getY());
            brackets.lineTo (box.getX() + arm, box.getY());

            brackets.startNewSubPath (box.getRight() - arm, box.getY());
            brackets.lineTo (box.getRight(), box.getY());
            brackets.lineTo (box.getRight(), box.getY() + arm);

            brackets.startNewSubPath (box.getRight(), box.getBottom() - arm);
            brackets.lineTo (box.getRight(), box.getBottom());
            brackets.lineTo (box.getRight() - arm, box.getBottom());

            brackets.startNewSubPath (box.getX() + arm, box.getBottom());
            brackets.lineTo (box.getX(), box.getBottom());
            brackets.lineTo (box.getX(), box.getY() + box.getHeight() - arm);

            g.setColour (juce::Colours::white.withAlpha (0.82f));
            g.strokePath (brackets, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered,
                                                          juce::PathStrokeType::butt));
        }
    }
}

void PianoRollComponent::drawMarquee (juce::Graphics& g)
{
    if (dragMode != DragMode::marquee)
        return;

    auto r = juce::Rectangle<float> (marqueeStart, marqueeCurrent);

    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillRect (r);

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.drawRect (r, 1.0f);
}

void PianoRollComponent::drawPlayhead (juce::Graphics& g)
{
    if (playheadBeat < 0.0)
        return;

    const auto area = getGridArea();
    const float x = screenXForBeat (playheadBeat);

    if (x < area.getX() || x > area.getRight())
        return;

    // Drawn as a path rather than drawVerticalLine: at 60 fps the playhead
    // moves about 1.3 px per frame, and an integer-snapped line makes that
    // read as a series of jumps. A 1.2 px path lands on sub-pixels, so the
    // motion looks continuous.
    juce::Path glow;
    glow.addRectangle (x - 2.5f, area.getY(), 5.0f, area.getHeight());
    g.setColour (kPlayheadColour.withAlpha (0.12f));
    g.fillPath (glow);

    juce::Path body;
    body.addRectangle (x - 0.6f, area.getY(), 1.2f, area.getHeight());
    g.setColour (kPlayheadColour.withAlpha (0.90f));
    g.fillPath (body);
}

void PianoRollComponent::drawRuler (juce::Graphics& g)
{
    auto ruler = getRulerArea();

    g.setColour (kRulerBackground);
    g.fillRect (ruler);

    //-- Corner readout: how long a bar is at the current tempo ---------------
    // The beat grid deliberately does not move when the tempo changes, so this
    // is the number that shows the timeline has been re-timed.
    {
        // The corner cell sits beside the ruler, not above it: drawing it at 0,0
        // painted over the tool strip and hid the first tool button.
        const auto corner = juce::Rectangle<int> (0, ruler.getY(), keyboardWidth, rulerHeight);

        g.setColour (kRulerBackground);
        g.fillRect (corner);

        const double barSeconds = 4.0 * (60.0 / juce::jmax (1.0, bpm));

        g.setColour (juce::Colours::white.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (7.5f))));
        g.drawText (utf8 ("1 小节"), ui::px (8), corner.getY() + ui::px (3),
                    ui::px (60), ui::px (14), juce::Justification::centredLeft, false);

        g.setColour (theme::accent.withAlpha (0.90f));
        g.setFont (boldFont (9.5f));
        g.drawText (juce::String (barSeconds, 2) + " s", ui::px (8), corner.getY() + ui::px (18),
                    ui::px (70), ui::px (16), juce::Justification::centredLeft, false);
    }

    auto formatTime = [] (double seconds)
    {
        const int    minutes = (int) (seconds / 60.0);
        const double rest    = seconds - (double) minutes * 60.0;

        return juce::String (minutes) + ":"
                 + juce::String (rest, 1).paddedLeft ('0', 4);
    };

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (ruler);

        //-- One-second ticks: the wall-clock scale under the musical one -----
        const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
        const double beatsPerSecond = 1.0 / secondsPerBeat;
        const double pixelsPerSecond = secondsPerBeat * (double) beatWidth;

        if (pixelsPerSecond >= 20.0)
        {
            const int firstSecond = (int) std::floor (scrollBeats / beatsPerSecond);
            const int lastSecond  = (int) std::ceil ((scrollBeats + (double) ruler.getWidth() / beatWidth)
                                                       / beatsPerSecond);

            g.setColour (theme::accent.withAlpha (0.30f));

            for (int second = firstSecond; second <= lastSecond; ++second)
            {
                const double beat = (double) second * beatsPerSecond;

                if (beat < 0.0 || beat > timelineBeats)
                    continue;

                const int x = (int) std::floor (screenXForBeat (beat));

                g.drawVerticalLine (x, (float) ruler.getBottom() - ui::pf (5),
                                    (float) ruler.getBottom());
            }
        }

        //-- Bars, with their wall-clock position underneath -------------------
        g.setFont (boldFont (10.5f));

        const int firstBar = (int) std::floor (scrollBeats / 4.0);
        const int lastBar  = (int) std::ceil ((scrollBeats + (double) ruler.getWidth() / beatWidth) / 4.0);

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            const double beat = (double) bar * 4.0;

            if (beat < 0.0 || beat > timelineBeats)
                continue;

            const float x = std::floor (screenXForBeat (beat)) + 0.5f;

            g.setColour (juce::Colours::white.withAlpha (0.22f));
            g.drawVerticalLine ((int) x, (float) ruler.getBottom() - ui::pf (9),
                                (float) ruler.getBottom());

            g.setColour (kText);
            g.drawText (juce::String (bar + 1),
                        juce::Rectangle<int> ((int) x + ui::px (4), ruler.getY() + ui::px (2),
                                              ui::px (46), ui::px (16)),
                        juce::Justification::centredLeft, false);

            // Changing the tempo rewrites this row — the beat grid itself must
            // not move, so the seconds are what tells you the tempo changed.
            g.setColour (theme::accent.withAlpha (0.45f));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (8.5f))));
            g.drawText (formatTime (beat * secondsPerBeat),
                        juce::Rectangle<int> ((int) x + ui::px (4), ruler.getY() + ui::px (19),
                                              ui::px (64), ui::px (15)),
                        juce::Justification::centredLeft, false);
            g.setFont (boldFont (10.5f));
        }
    }

    // Playhead marker in the ruler, drawn on top of the bar numbers.
    if (playheadBeat >= 0.0)
    {
        const float x = screenXForBeat (playheadBeat);

        if (x >= ruler.getX() && x <= ruler.getRight())
        {
            juce::Path body;
            body.addRectangle (x - 0.6f, (float) ruler.getY(), 1.2f, (float) ruler.getHeight());

            g.setColour (kPlayheadColour);
            g.fillPath (body);

            juce::Path head;
            head.addTriangle (x - ui::pf (6), (float) ruler.getY(),
                              x + ui::pf (6), (float) ruler.getY(),
                              x, (float) ruler.getY() + ui::pf (9));
            g.fillPath (head);
        }
    }

    g.setColour (kOutline);
    g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());
}

void PianoRollComponent::drawKeyboard (juce::Graphics& g)
{
    auto keys = getKeyboardArea();

    g.setColour (theme::surface2);
    g.fillRect (keys);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (keys);

        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));

        const int topPitch    = contentYToPitch (scrollY);
        const int bottomPitch = contentYToPitch (scrollY + (float) keys.getHeight());

        for (int pitch = bottomPitch; pitch <= topPitch; ++pitch)
        {
            const float y = screenYForPitch (pitch);
            const bool  black = isBlackKey (pitch);

            auto row = juce::Rectangle<float> ((float) keys.getX(), y,
                                               black ? (float) keys.getWidth() * 0.58f : (float) keys.getWidth(),
                                               keyHeight);

            g.setColour (black ? kKeyBlack : kKeyWhite);
            g.fillRect (row);

            // Phase 22: the key under the pointer is lit the same colour the
            // notes use for "you are working on this", so a key press and a
            // note selection read as the same kind of thing.
            if (pitch == pressedKeyPitch)
            {
                g.setColour (theme::accent.withAlpha (black ? 0.55f : 0.40f));
                g.fillRect (row);

                g.setColour (theme::accent);
                g.drawRect (row, 1.2f);
            }

            if (! black)
            {
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.drawText (noteName (pitch),
                            keys.withY ((int) y).withHeight ((int) keyHeight).reduced (ui::px (5), 0),
                            juce::Justification::centredRight, false);
            }

            g.setColour (juce::Colours::black.withAlpha (black ? 0.35f : 0.12f));
            g.drawHorizontalLine ((int) (y + keyHeight) - 1, (float) keys.getX(), (float) keys.getRight());
        }
    }

    g.setColour (kOutline);
    g.drawVerticalLine (keys.getRight() - 1, (float) keys.getY(), (float) keys.getBottom());
}

//==============================================================================
//  Phase 8: tool icons
//==============================================================================
void PianoRollComponent::drawToolIcon (juce::Graphics& g, int toolIndex,
                                       juce::Rectangle<float> area, juce::Colour colour)
{
    // Every icon is drawn inside a 16x16 box centred on the button. (Callers
    // used to pre-shrink the button with reduced(), which turns inside out once
    // the inset passes half the height — the icons ended up hanging off the
    // bottom edge.)
    const auto box = juce::Rectangle<float> (16.0f, 16.0f).withCentre (area.getCentre());
    const auto c   = box.getCentre();

    g.setColour (colour);

    switch (toolIndex)
    {
        case 0:     // pencil
        {
            juce::Path body;
            body.startNewSubPath (box.getX() + 5.0f, box.getBottom() - 5.0f);
            body.lineTo (box.getRight() - 3.0f, box.getY() + 3.0f);

            g.strokePath (body, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

            // The sharpened tip, meeting the body at 45 degrees.
            juce::Path tip;
            tip.addTriangle (box.getX() + 1.8f, box.getBottom() - 1.8f,
                             box.getX() + 3.2f, box.getBottom() - 6.6f,
                             box.getX() + 6.6f, box.getBottom() - 3.2f);
            g.fillPath (tip);
            break;
        }

        case 1:     // eraser: a tilted block with the rubber end marked
        {
            juce::Path block;
            block.addRoundedRectangle (-5.5f, -3.2f, 11.0f, 6.4f, 1.6f);
            block.applyTransform (juce::AffineTransform::rotation (-0.55f).translated (c));

            g.strokePath (block, juce::PathStrokeType (1.6f));

            juce::Path divider;
            divider.startNewSubPath (-1.2f, -3.2f);
            divider.lineTo (-1.2f, 3.2f);
            divider.applyTransform (juce::AffineTransform::rotation (-0.55f).translated (c));

            g.strokePath (divider, juce::PathStrokeType (1.2f));
            break;
        }

        case 2:     // select: the classic arrow cursor
        {
            juce::Path arrow;
            arrow.startNewSubPath (box.getX() + 4.0f, box.getY() + 2.0f);
            arrow.lineTo (box.getX() + 4.0f, box.getBottom() - 3.0f);
            arrow.lineTo (box.getX() + 7.0f, box.getBottom() - 5.6f);
            arrow.lineTo (box.getX() + 9.0f, box.getBottom() - 0.5f);
            arrow.lineTo (box.getX() + 11.2f, box.getBottom() - 1.6f);
            arrow.lineTo (box.getX() + 9.1f, box.getBottom() - 6.4f);
            arrow.lineTo (box.getX() + 12.5f, box.getBottom() - 6.6f);
            arrow.closeSubPath();

            g.fillPath (arrow);
            break;
        }

        case 4:     // Phase 51: knife — a utility blade: body, snap grooves, tip
        {
            // First version was a slim filled triangle plus a handle stroke,
            // and at 16 px it read as one thin diagonal line — indistinguishable
            // from noise, let alone from the pencil. A utility knife reads
            // instead: the eraser's rounded-rect body (one family look), a
            // triangular blade sticking out of the working end, and the snap
            // grooves across the blade. The eraser has no tip; the pencil has
            // no grooves; this has both.
            const auto knife = juce::AffineTransform::rotation (-0.55f).translated (c);

            juce::Path body;
            body.addRoundedRectangle (-5.6f, -2.2f, 9.2f, 4.4f, 1.4f);
            body.applyTransform (knife);

            g.strokePath (body, juce::PathStrokeType (1.6f));

            juce::Path blade;
            blade.addTriangle (11.0f, 0.0f, 4.6f, -2.2f, 4.6f, 2.2f);
            blade.applyTransform (knife);

            g.fillPath (blade);

            juce::Path grooves;
            grooves.startNewSubPath (6.4f, -1.5f);
            grooves.lineTo (6.4f, 1.5f);
            grooves.startNewSubPath (8.1f, -1.1f);
            grooves.lineTo (8.1f, 1.9f);
            grooves.applyTransform (knife);

            g.strokePath (grooves, juce::PathStrokeType (1.0f));
            break;
        }

        case 5:     // copy note state: a dropper, with a drop of what it picked up
        {
            juce::Path pipette;
            pipette.startNewSubPath (box.getRight() - 3.0f, box.getY() + 2.0f);
            pipette.lineTo (box.getX() + 5.0f, box.getBottom() - 8.0f);
            pipette.lineTo (box.getX() + 2.0f, box.getBottom() - 5.0f);
            pipette.closeSubPath();

            g.fillPath (pipette);

            // The bulb at the top, and the drop beneath — the two things that
            // tell a dropper apart from a pencil at 16 px.
            g.fillEllipse (box.getRight() - 6.0f, box.getY() + 0.5f, 5.5f, 5.5f);

            juce::Path drop;
            drop.startNewSubPath (box.getX() + 2.5f, box.getBottom() - 4.5f);
            drop.quadraticTo (box.getX() - 1.5f, box.getBottom() - 1.0f,
                              box.getX() + 2.5f, box.getBottom() - 0.5f);
            drop.quadraticTo (box.getX() + 6.5f, box.getBottom() - 1.0f,
                              box.getX() + 2.5f, box.getBottom() - 4.5f);
            g.fillPath (drop);
            break;
        }

        case 6:     // group: three blocks behind a bracket
        {
            g.fillRoundedRectangle (box.getX() + 4.5f, box.getY() + 3.5f, 7.5f, 3.0f, 1.0f);
            g.fillRoundedRectangle (box.getX() + 4.5f, box.getY() + 8.0f, 7.5f, 3.0f, 1.0f);

            // The bracket says "these are one object"; without it the two bars
            // read as a pause icon, which is the one thing it must not be.
            juce::Path bracket;
            bracket.startNewSubPath (box.getX() + 2.5f, box.getY() + 2.0f);
            bracket.lineTo (box.getX() + 1.0f, box.getY() + 2.0f);
            bracket.lineTo (box.getX() + 1.0f, box.getBottom() - 2.0f);
            bracket.lineTo (box.getX() + 2.5f, box.getBottom() - 2.0f);
            bracket.startNewSubPath (box.getRight() - 2.5f, box.getY() + 2.0f);
            bracket.lineTo (box.getRight() - 1.0f, box.getY() + 2.0f);
            bracket.lineTo (box.getRight() - 1.0f, box.getBottom() - 2.0f);
            bracket.lineTo (box.getRight() - 2.5f, box.getBottom() - 2.0f);

            g.strokePath (bracket, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            break;
        }

        case 7:     // ungroup: the group mark, with the brackets coming off
        {
            g.fillRoundedRectangle (box.getX() + 5.0f, box.getY() + 3.5f, 6.5f, 3.0f, 1.0f);
            g.fillRoundedRectangle (box.getX() + 5.0f, box.getY() + 9.5f, 6.5f, 3.0f, 1.0f);

            // Broken brackets, open at the middle and at both ends: the pair
            // that held these two bars together is being taken away, which is
            // the one thing that distinguishes this from the 编组 button.
            juce::Path left;
            left.startNewSubPath (box.getX() + 3.2f, box.getY() + 2.0f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 2.0f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 6.4f);
            left.startNewSubPath (box.getX() + 1.4f, box.getY() + 9.6f);
            left.lineTo (box.getX() + 1.4f, box.getY() + 14.0f);
            left.lineTo (box.getX() + 3.2f, box.getY() + 14.0f);

            juce::Path right;
            right.startNewSubPath (box.getRight() - 3.2f, box.getY() + 2.0f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 2.0f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 6.4f);
            right.startNewSubPath (box.getRight() - 1.4f, box.getY() + 9.6f);
            right.lineTo (box.getRight() - 1.4f, box.getY() + 14.0f);
            right.lineTo (box.getRight() - 3.2f, box.getY() + 14.0f);

            g.strokePath (left, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::butt));
            g.strokePath (right, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::butt));
            break;
        }

        case 8:     // snap: a small horseshoe magnet, poles up
        {
            // Drawn as a path, not as a filled shape: the U is one stroke of
            // constant width, so it stays a magnet at 16 px instead of turning
            // into a blob. Right leg -> bottom arc -> left leg, because JUCE
            // measures arc angles clockwise from 12 o'clock and the path has to
            // travel the way the stroke expects.
            //
            // The first version drew the legs at 3.0 px with pole faces only
            // one pixel wider, and the whole thing read as the letter U. The
            // legs are now thin (2.6) and the pole faces flare well past them
            // with a gap down the middle, which is the silhouette everyone
            // recognises. The legs start below the poles rather than behind
            // them, so neither shape eats the other.
            const float leftLeg  = box.getX() + 4.5f;
            const float rightLeg = box.getX() + 11.5f;
            const float poleTop  = box.getY() + 1.3f;
            const float poleH    = 3.4f;

            juce::Path horseshoe;
            horseshoe.startNewSubPath (rightLeg, poleTop + poleH);
            horseshoe.lineTo (rightLeg, box.getY() + 9.2f);
            horseshoe.addCentredArc (box.getX() + 8.0f, box.getY() + 9.2f, 3.5f, 3.5f, 0.0f,
                                     juce::MathConstants<float>::pi * 0.5f,
                                     juce::MathConstants<float>::pi * 1.5f, false);
            horseshoe.lineTo (leftLeg, poleTop + poleH);

            g.strokePath (horseshoe, juce::PathStrokeType (2.6f, juce::PathStrokeType::mitered,
                                                           juce::PathStrokeType::butt));

            g.fillRoundedRectangle (juce::Rectangle<float> (leftLeg - 2.6f, poleTop, 5.2f, poleH), 0.8f);
            g.fillRoundedRectangle (juce::Rectangle<float> (rightLeg - 2.6f, poleTop, 5.2f, poleH), 0.8f);
            break;
        }

        case 3:     // move view: a four-way cross
        default:
        {
            const float arm = 5.0f;

            g.drawLine (c.x - arm, c.y, c.x + arm, c.y, 1.5f);
            g.drawLine (c.x, c.y - arm, c.x, c.y + arm, 1.5f);

            auto head = [&g, c] (float dx, float dy)
            {
                juce::Path p;
                p.addTriangle (c.x + dx * 7.5f, c.y + dy * 7.5f,
                               c.x + dx * 4.0f - dy * 2.6f, c.y + dy * 4.0f - dx * 2.6f,
                               c.x + dx * 4.0f + dy * 2.6f, c.y + dy * 4.0f + dx * 2.6f);
                g.fillPath (p);
            };

            head (0.0f, -1.0f);
            head (0.0f,  1.0f);
            head (-1.0f, 0.0f);
            head ( 1.0f, 0.0f);
            break;
        }
    }
}

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
    hoveredAutomationMenu = false;

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

juce::Rectangle<float> PianoRollComponent::getAutomationCloseArea (const NoteEvent& note) const
{
    const auto  box  = getNoteAutomationBoxBounds (note);
    const float size = juce::jlimit (ui::pf (9), ui::pf (17), box.getWidth() - ui::pf (7));

    return { box.getRight() - size - ui::pf (3), box.getY() + ui::pf (3), size, size };
}

int PianoRollComponent::findEnvelopePointAt (const NoteEvent& note, juce::Point<float> position) const
{
    const auto inner = getAutomationCurveArea (note);
    const auto& points = note.envelope.getPoints();

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

    note.envelope.movePoint (dragEnvelopePoint, t, v);

    // Phase 62: same rule for a note's own curve — one shape, every place.
    syncShapeOfMember (note.id);

    if (onNoteEdited != nullptr)
        onNoteEdited();

    repaint();
}

bool PianoRollComponent::handleAutomationMouseDown (const juce::MouseEvent& e, NoteEvent& note)
{
    const auto pos = e.position;

    if (getAutomationCloseArea (note).contains (pos))
    {
        closeNoteAutomation();
        return true;
    }

    if (getAutomationMenuArea (note).contains (pos))
    {
        showModTargetMenu (note, e.getScreenPosition());
        return true;
    }

    const auto inner = getAutomationCurveArea (note);

    if (! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    const int hit = findEnvelopePointAt (note, pos);

    if (e.mods.isPopupMenu())
    {
        // Right-click removes a point; the curve always keeps two.
        if (hit >= 0)
        {
            note.envelope.removePoint (hit);

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

        const int added = note.envelope.addPoint (t, v);

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
    const int current    = (int) note.modTarget;

    // Phase 24: the synth this note plays through decides whether there is a
    // second menu half at all. A hosted instrument has a parameter list; the
    // internal oscillator has the three built-in targets and nothing else.
    const int slotId = resolvedSlotForNote (note);

    std::vector<int> paramIndices;

    for (const auto& info : pluginParamsForSlot (slotId))
        paramIndices.push_back (info.index);

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("调制的参数"));

    for (auto target : { NoteModTarget::level, NoteModTarget::pan, NoteModTarget::pitch })
        menu.addItem ((int) target + 1, modTargetLabel (target) + "  ·  " + modTargetHint (target),
                      true, ! note.drivesPluginParam() && (int) target == current);

    if (! paramIndices.empty())
    {
        menu.addSeparator();
        addPluginParamSubmenu (menu, slotId, note.modPluginParam, kPluginParamItemBase);
    }
    else if (resolvedSlotForNote (note) >= 0)
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

        if (result >= kPluginParamItemBase)
        {
            const int which = result - kPluginParamItemBase;

            if (! juce::isPositiveAndBelow (which, (int) paramIndices.size()))
                return;

            target->modPluginParam = paramIndices[(size_t) which];

            // Switching to a plugin parameter has to make the curve audible
            // for the same reason opening a box does: a curve you can see and
            // cannot hear is the classic way automation gets taught wrong.
            target->envelope.setEnabled (true);
        }
        else
        {
            target->modPluginParam = -1;          // back to the built-in target
            target->modTarget = (NoteModTarget) (result - 1);
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

    //-- Parameter cell ------------------------------------------------------
    // Wide box: the parameter's name and a chevron. Narrow box: just the
    // parameter's icon. "音量" needs 24 px and a 1/16 note is 12 px wide, so
    // text would be clipped into a single unreadable glyph anyway — but the
    // cell still opens the same menu.
    const auto menuArea = getAutomationMenuArea (*note);
    const bool menuHot  = hoveredAutomationMenu;
    const bool iconOnly = menuArea.getWidth() < 52.0f;

    g.setColour (colour.withAlpha (menuHot ? 0.26f : 0.13f));
    g.fillRoundedRectangle (menuArea, 3.0f);

    if (iconOnly)
    {
        const float side  = juce::jmin (12.0f, juce::jmin (menuArea.getWidth(), menuArea.getHeight()));
        const auto  glyph = juce::Rectangle<float> (side, side).withCentre (menuArea.getCentre());

        if (note->drivesPluginParam())
            drawKnobGlyph (g, glyph, juce::Colours::white.withAlpha (0.92f));
        else
            drawModTargetIcon (g, note->modTarget, glyph, juce::Colours::white.withAlpha (0.92f));
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f)).withStyle ("Bold")));
        g.drawText (noteTargetLabel (*note),
                    menuArea.reduced (6.0f, 0.0f).withTrimmedRight (12.0f),
                    juce::Justification::centredLeft, true);

        // The "v" that says "this opens a menu".
        juce::Path chevron;
        const float cx = menuArea.getRight() - 9.0f;
        const float cy = menuArea.getCentreY();

        chevron.startNewSubPath (cx - 3.5f, cy - 1.8f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 3.5f, cy - 1.8f);

        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
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
    const auto& envelope = note.envelope;
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

void PianoRollComponent::openGroupAutomation (int layerIndex, int groupId, int target,
                                              int occurrence)
{
    // One floating editor at a time, same rule as the note box.
    closeNoteAutomation();
    closeSampleAutomation();

    expandedGroupLayerIndex = layerIndex;
    expandedGroupId         = groupId;
    expandedGroupOccurrence = juce::jmax (0, occurrence);
    expandedGroupTarget     = juce::jlimit (0, numGroupAutomationTargets - 1, target);
    dragGroupPoint          = -1;

    // A group that was never given a plugin parameter cannot be edited on the
    // plugin lane: opening the box there would show a curve with nothing behind
    // it (the publisher skips a lane with `pluginParam < 0`). Falling back to
    // the level lane is the honest default — the menu is one click away.
    if (auto* group = getExpandedGroup())
        if (expandedGroupTarget == (int) GroupAutomationTarget::plugin
              && ! group->drivesPluginParam())
            expandedGroupTarget = (int) GroupAutomationTarget::level;

    // Opening the box means "I am editing this curve", so the lane goes on with
    // it. A curve you can see and cannot hear is the single most common way a
    // DAW teaches the wrong thing about automation. The notification only fires
    // when the flag really flips: opening a box on a live curve must not push an
    // undo step for doing nothing.
    if (auto* group = getExpandedGroup())
    {
        auto& lane = group->automationFor ((GroupAutomationTarget) expandedGroupTarget);

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
    hoveredGroupMenu        = false;

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

AutomationLane& PianoRollComponent::groupLaneFor (NoteGroup& group, int target)
{
    return group.automationFor ((GroupAutomationTarget) juce::jlimit (0, numGroupAutomationTargets - 1,
                                                                    target));
}

const AutomationLane& PianoRollComponent::groupLaneFor (const NoteGroup& group, int target) const
{
    return group.automationFor ((GroupAutomationTarget) juce::jlimit (0, numGroupAutomationTargets - 1,
                                                                    target));
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
    const auto& points = groupLaneFor (*group, expandedGroupTarget).getPoints();

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

    // Phase 24: the third lane drives a parameter of the synth the group is
    // mounted on. Which parameters exist is a question only the plugin can
    // answer, so the list is built here, on the message thread, once.
    const int slotId = resolvedSlotForGroup (layerIndex, groupId);

    std::vector<int> paramIndices;

    for (const auto& info : pluginParamsForSlot (slotId))
        paramIndices.push_back (info.index);

    const bool onPluginLane = expandedGroupTarget == (int) GroupAutomationTarget::plugin;

    juce::PopupMenu menu;
    menu.addSectionHeader (group->getName() + utf8 (" · 自动化参数"));

    menu.addItem (1, utf8 ("组音量") + "  ·  " + utf8 ("整组的推子，乘进每个成员"),
                  true, expandedGroupTarget == (int) GroupAutomationTarget::level);
    menu.addItem (2, utf8 ("组声相") + "  ·  " + utf8 ("整组的声相，加到每个成员"),
                  true, expandedGroupTarget == (int) GroupAutomationTarget::pan);

    if (! paramIndices.empty())
    {
        addPluginParamSubmenu (menu, slotId, group->getPluginParam(), kPluginParamItemBase);
    }
    else
    {
        // Saying why beats a menu that silently lists two thirds of itself.
        menu.addItem (-1, group->getPluginParam() >= 0 ? utf8 ("音源参数：音源已不在，曲线保留")
                                                       : utf8 ("挂到外部音源后可选它的参数"),
                      false, onPluginLane);
    }

    menu.addSeparator();
    menu.addItem (3, utf8 ("从曲线中移除…"), true);

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

        if (result == 3)
        {
            // "Remove" means the lane is switched off AND flattened: an emptied
            // curve that still carried old breakpoints would come back the
            // moment the lane was switched on again.
            target->automationFor ((GroupAutomationTarget) safe->expandedGroupTarget)
                  .resetFlat (neutralCurveValue (safe->expandedGroupTarget, true),
                              safe->timelineBeats);
            target->automationFor ((GroupAutomationTarget) safe->expandedGroupTarget)
                  .setEnabled (false);
        }
        else if (result >= kPluginParamItemBase)
        {
            const int which = result - kPluginParamItemBase;

            if (! juce::isPositiveAndBelow (which, (int) paramIndices.size()))
                return;

            target->setPluginParam (paramIndices[(size_t) which]);
            safe->expandedGroupTarget = (int) GroupAutomationTarget::plugin;

            auto& lane = target->getPluginAutomation();

            // A lane that has never been drawn starts flat at the centre:
            // 1.0 would slam a cutoff wide open the first time you look at it.
            if (! lane.isEnabled() && lane.getPoints().size() <= 2)
                lane.resetFlat (0.5f, safe->timelineBeats);

            lane.setEnabled (true);
        }
        else
        {
            safe->expandedGroupTarget = juce::jlimit (0, 1, result - 1);
            target->automationFor ((GroupAutomationTarget) safe->expandedGroupTarget)
                  .setEnabled (true);
        }

        safe->stack.notifyNotesChanged();
        safe->repaint();
    });
}

bool PianoRollComponent::handleGroupAutomationMouseDown (const juce::MouseEvent& e)
{
    auto* group = getExpandedGroup();

    if (group == nullptr)
        return true;

    const auto pos = e.position;

    if (getGroupCloseArea().contains (pos))
    {
        closeGroupAutomation();
        return true;
    }

    if (getGroupMenuArea().contains (pos))
    {
        showGroupTargetMenu (e.getScreenPosition());
        return true;
    }

    const auto inner = getGroupCurveBounds();

    if (inner.isEmpty() || ! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    auto& lane = groupLaneFor (*group, expandedGroupTarget);

    auto* layer = stack.getLayer (expandedGroupLayerIndex);

    if (layer == nullptr)
        return true;

    const auto span = getGroupBeatSpan (*layer, *group);

    const int hit = findGroupCurvePointAt (pos);

    if (e.mods.isPopupMenu())
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

    auto& lane = groupLaneFor (*group, expandedGroupTarget);

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

    const auto& lane = groupLaneFor (*group, expandedGroupTarget);
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

    //-- Parameter cell ------------------------------------------------------
    const auto menuArea = getGroupMenuArea();
    const bool iconOnly = menuArea.getWidth() < 62.0f;

    g.setColour (colour.withAlpha (hoveredGroupMenu ? 0.26f : 0.13f));
    g.fillRoundedRectangle (menuArea, 3.0f);

    const juce::String targetName = groupTargetLabel (*group);
    const bool pluginTarget = expandedGroupTarget == (int) GroupAutomationTarget::plugin;

    if (iconOnly)
    {
        const auto glyph = juce::Rectangle<float> (ui::pf (11), ui::pf (11))
                             .withCentre (menuArea.getCentre());

        if (pluginTarget)
            drawKnobGlyph (g, glyph, juce::Colours::white.withAlpha (0.92f));
        else
            drawModTargetIcon (g, expandedGroupTarget == 1 ? NoteModTarget::pan : NoteModTarget::level,
                               glyph, juce::Colours::white.withAlpha (0.92f));
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f)).withStyle ("Bold")));
        g.drawText (targetName, menuArea.reduced (6.0f, 0.0f).withTrimmedRight (12.0f),
                    juce::Justification::centredLeft, true);

        juce::Path chevron;
        const float cx = menuArea.getRight() - 9.0f;
        const float cy = menuArea.getCentreY();

        chevron.startNewSubPath (cx - 3.5f, cy - 1.8f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 3.5f, cy - 1.8f);

        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
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

        for (const auto& p : note->envelope.getPoints())
            out.push_back ({ (double) p.time * juce::jmax (gridDivision, note->lengthBeats),
                             p.value });

        lengthBeats = juce::jmax (gridDivision, note->lengthBeats);
        target      = (int) note->modTarget;

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

        for (const auto& p : groupLaneFor (*group, expandedGroupTarget).getPoints())
            if (p.beat >= span.getStart() - 1.0e-9 && p.beat <= span.getEnd() + 1.0e-9)
                out.push_back ({ p.beat - span.getStart(), p.value });

        lengthBeats = juce::jmax (1.0e-6, span.getLength());
        target      = expandedGroupTarget;

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
            const float flat = neutralCurveValue ((int) note->modTarget, false);
            note->envelope.loadPoints ({ { 0.0f, flat }, { 1.0f, flat } });
            note->envelope.setEnabled (true);
        }
    }
    else if (curveSelection == CurveSelection::groupLane)
    {
        if (auto* group = getExpandedGroup())
        {
            auto& lane = groupLaneFor (*group, expandedGroupTarget);
            lane.resetFlat (neutralCurveValue (expandedGroupTarget, true), timelineBeats);
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

        note->envelope.loadPoints (std::move (out));
        note->envelope.setEnabled (true);
        note->modTarget = (NoteModTarget) juce::jlimit (0, 2, target);
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
        const int laneTarget = juce::jlimit (0, 1, target);

        auto& lane = groupLaneFor (*group, laneTarget);
        lane.loadPoints (std::move (out));
        lane.setEnabled (true);

        // Show the lane that was just written, or the box would keep displaying
        // a curve the paste did not touch.
        expandedGroupTarget = laneTarget;
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

//==============================================================================
//  Phase 22: the note clipboard
//==============================================================================
namespace
{
    /** " · 3 个音符 · 2 个采样" — never a count of zero, because the numbers are
        what tell the user whether this clipboard is going to paste a group or a
        handful of notes. */
    juce::String membersDescription (int notes, int clips)
    {
        juce::String out;

        if (notes > 0)
            out = utf8 (" · ") + juce::String (notes) + utf8 (" 个音符");

        if (clips > 0)
            out += utf8 (" · ") + juce::String (clips) + utf8 (" 个采样");

        return out;
    }
}

bool PianoRollComponent::copyNotesToClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    if (selection.empty() && sampleSelection.empty())
        return false;

    // Phase 63: this fills ALL THREE clipboards — the objects, the loose notes
    // and the loose blocks — in one pass, because "which member of this
    // selection travels inside a copied object, and which travels on its own"
    // is one question with one answer. Filling them in separate passes (this
    // function, then copySamplesToClipboard) meant the block clipboard got a
    // list that included the blocks the group clipboard had already claimed:
    // one copy, two descriptions of it, and a paste that wrote both.
    noteClip.clear();
    noteClipFirstBeat = std::numeric_limits<double>::max();
    noteClipEndBeat   = 0.0;

    sampleClip.clear();
    sampleClipFirstBeat  = std::numeric_limits<double>::max();
    sampleClipLengthBeat = 1.0;

    // A fresh copy starts a fresh streak: keeping the old next-beat would send
    // the first paste of a new clipboard to the tail of the previous one.
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    groupClips.clear();

    //-- 1. Sort the selection into slices, one per appearance ----------------
    // Phase 63: the selection is split by (groupId, occurrence), not by kind.
    // Two groups selected together used to be one undifferentiated pile of
    // notes, which is why the copy could only be "a group" or "some notes" —
    // never "these two groups".
    using SliceKey = std::pair<int, int>;

    std::map<SliceKey, std::vector<NoteEvent>>   noteSlices;
    std::map<SliceKey, std::vector<SampleEvent>> clipSlices;
    std::vector<NoteEvent>   looseNotes;
    std::vector<SampleEvent> looseClips;

    for (const auto& note : active->getNotes())
    {
        if (! isSelected (note.id))
            continue;

        // A member of a group that is gone is a loose note; the dangling id is
        // the model's business, not the clipboard's.
        if (note.groupId < 0 || active->findGroup (note.groupId) == nullptr)
        {
            looseNotes.push_back (note);
            continue;
        }

        noteSlices[{ note.groupId, note.groupOccurrence }].push_back (note);
    }

    for (const auto& clip : active->getSamples())
    {
        if (! isSampleSelected (clip.id))
            continue;

        if (clip.groupId < 0 || active->findGroup (clip.groupId) == nullptr)
        {
            looseClips.push_back (clip);
            continue;
        }

        clipSlices[{ clip.groupId, clip.groupOccurrence }].push_back (clip);
    }

    //-- 2. A slice that IS a whole appearance becomes one clipboard object ---
    std::set<SliceKey> keys;

    for (const auto& entry : noteSlices)
        keys.insert (entry.first);

    for (const auto& entry : clipSlices)
        keys.insert (entry.first);

    for (const auto& key : keys)
    {
        const auto noteIt = noteSlices.find (key);
        const auto clipIt = clipSlices.find (key);

        auto* group = active->findGroup (key.first);
        const bool whole = group != nullptr && isWholeOccurrenceSelected (key.first, key.second);

        if (! whole)
        {
            // Part of an appearance, or no appearance at all: these travel as
            // members of nothing. Half a group is not a copyable object — the
            // paste would have to invent where the other half was.
            if (noteIt != noteSlices.end())
                for (const auto& note : noteIt->second)
                    looseNotes.push_back (note);

            if (clipIt != clipSlices.end())
                for (const auto& clip : clipIt->second)
                    looseClips.push_back (clip);

            continue;
        }

        // Phase 62: the appearance's PINNED origin, not the earliest member —
        // an appearance that lost its earliest note to an earlier edit still
        // sits where it was told to sit, and its curves are absolute to that.
        const double start = active->occurrenceStart (key.first, key.second);

        auto object = captureGroupClip (*group, start, key.second);

        if (noteIt != noteSlices.end())
            object.notes = noteIt->second;

        if (clipIt != clipSlices.end())
            object.clips = clipIt->second;

        groupClips.push_back (std::move (object));
    }

    //-- 3. The leftovers are the loose clipboard ----------------------------
    noteClip   = std::move (looseNotes);
    sampleClip = std::move (looseClips);

    for (const auto& note : noteClip)
    {
        noteClipFirstBeat = juce::jmin (noteClipFirstBeat, note.startBeat);
        noteClipEndBeat   = juce::jmax (noteClipEndBeat, note.startBeat + note.lengthBeats);
    }

    if (noteClip.empty())
        noteClipFirstBeat = 0.0;

    double clipEnd = 0.0;

    for (const auto& clip : sampleClip)
    {
        sampleClipFirstBeat = juce::jmin (sampleClipFirstBeat, clip.startBeat);
        clipEnd             = juce::jmax (clipEnd, clip.endBeat());
    }

    if (sampleClip.empty())
        sampleClipFirstBeat = 0.0;
    else
        sampleClipLengthBeat = juce::jmax (gridDivision, clipEnd - sampleClipFirstBeat);

    //-- 4. Say what happened ------------------------------------------------
    if (! groupClips.empty())
    {
        int noteCount = 0;
        int clipCount = 0;

        for (const auto& object : groupClips)
        {
            noteCount += (int) object.notes.size();
            clipCount += (int) object.clips.size();
        }

        flashStatus (utf8 ("已复制 ") + getGroupClipboardName()
                       + membersDescription (noteCount, clipCount));
        return true;
    }

    if (noteClip.empty() && sampleClip.empty())
        return false;

    if (noteClip.empty())
    {
        flashStatus (utf8 ("已复制 ") + juce::String ((int) sampleClip.size()) + utf8 (" 个采样"));
        return true;
    }

    flashStatus (utf8 ("已复制 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符"));
    return true;
}

juce::String PianoRollComponent::getGroupClipboardName() const
{
    if (groupClips.empty())
        return {};

    // One object answers with its name; two or more answer with a count. Naming
    // the first one would read as "you copied 组 A" when the clipboard also
    // holds 组 B.
    return groupClips.size() == 1 ? groupClips.front().name
                                  : juce::String ((int) groupClips.size()) + utf8 (" 个组");
}

bool PianoRollComponent::isWholeOccurrenceSelected (int groupId, int occurrence) const
{

    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0 || occurrence < 0)
        return false;

    // Counted off the MODEL, not off the selection: the question is "did the
    // user get all of it", and a selection that also holds half of the next
    // group must not change the answer.
    int members = 0;
    int chosen  = 0;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId && note.groupOccurrence == occurrence)
        {
            ++members;

            if (isSelected (note.id))
                ++chosen;
        }

    for (const auto& clip : layer->getSamples())
        if (clip.groupId == groupId && clip.groupOccurrence == occurrence)
        {
            ++members;

            if (isSampleSelected (clip.id))
                ++chosen;
        }

    return members > 0 && chosen == members;
}

double PianoRollComponent::clipboardFirstBeat() const
{
    double first = std::numeric_limits<double>::max();

    for (const auto& object : groupClips)
        first = juce::jmin (first, object.startBeat);

    if (! noteClip.empty())
        first = juce::jmin (first, noteClipFirstBeat);

    if (! sampleClip.empty())
        first = juce::jmin (first, sampleClipFirstBeat);

    if (first == std::numeric_limits<double>::max())
        return 0.0;

    return juce::jmax (0.0, first);
}

double PianoRollComponent::clipboardLastBeat() const
{
    double last = 0.0;

    for (const auto& object : groupClips)
    {
        for (const auto& note : object.notes)
            last = juce::jmax (last, note.startBeat + note.lengthBeats);

        for (const auto& clip : object.clips)
            last = juce::jmax (last, clip.endBeat());
    }

    if (! noteClip.empty())
        last = juce::jmax (last, noteClipEndBeat);

    for (const auto& clip : sampleClip)
        last = juce::jmax (last, clip.endBeat());

    return last;
}

void PianoRollComponent::pasteLooseNotesInto (Layer& layer, double delta,
                                              std::vector<int>& newIds)
{
    // Loose notes arrive loose. The NoteEvent they were copied from still
    // carries its groupId, and pasting it unchanged would quietly drop the copy
    // back into the appearance the user copied it out of.
    for (auto note : noteClip)
    {
        note.startBeat       = juce::jmax (0.0, note.startBeat + delta);
        note.id              = 0;
        note.groupId         = -1;
        note.groupOccurrence = 0;

        if (const int newId = layer.addNote (note); newId > 0)
            newIds.push_back (newId);
    }
}

void PianoRollComponent::pasteLooseSamplesInto (Layer& layer, double delta,
                                                std::vector<int>& newIds)
{
    for (auto clip : sampleClip)
    {
        clip.startBeat = juce::jmax (0.0, clip.startBeat + delta);
        clip.id        = 0;

        clip.groupId         = -1;
        clip.groupOccurrence = 0;

        // A CUT takes its sampler with it, so the slot this clipboard holds may
        // already be gone.
        ensureClipSlot (clip);

        if (const int newId = layer.addSample (std::move (clip)); newId > 0)
        {
            newIds.push_back (newId);

            if (const auto* created = layer.findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }
}

bool PianoRollComponent::cutNotesToClipboard()
{
    if (! copyNotesToClipboard())
        return false;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // Phase 56: copying a GROUP now puts its blocks on the clipboard too, so a
    // cut of that group has to take them off the timeline as well — otherwise
    // "cut a group" leaves its audio behind and the paste creates a second copy
    // of blocks nobody moved.
    //
    // Phase 63: every selected block leaves, not just the ones the block
    // clipboard happens to describe. Since the copy now splits its blocks
    // between "member of a copied object" and "loose", reading the cut list off
    // one of the two clipboards left the other half on the timeline.
    for (int id : selection)
        active->removeNote (id);

    for (int id : sampleSelection)
        active->removeSample (id);

    selection.clear();
    sampleSelection.clear();

    hoveredNoteId = -1;
    notifySelectionChanged();
    pruneEmptyGroups();
    stack.notifyNotesChanged();

    flashStatus ((! groupClips.empty()
                    ? utf8 ("已剪切 ") + getGroupClipboardName()
                    : utf8 ("已剪切 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符")));
    return true;
}

PianoRollComponent::GroupClip PianoRollComponent::captureGroupClip (const NoteGroup& group,
                                                                     double startBeat,
                                                                     int occurrence) const
{
    GroupClip clip;

    clip.sourceGroupId    = group.getId();
    clip.sourceOccurrence = juce::jmax (0, occurrence);
    clip.name             = group.getName();
    clip.colour       = group.getColour();
    clip.outputSlotId = group.getOutputSlotId();
    clip.levelDb      = group.getLevelDb();
    clip.pan          = group.getPan();
    clip.pluginParam  = group.getPluginParam();
    clip.startBeat    = startBeat;

    for (int i = 0; i < numGroupAutomationTargets; ++i)
    {
        const auto target = (GroupAutomationTarget) i;
        clip.lanes[(size_t) i]  = group.automationFor (target);
        clip.laneOn[(size_t) i] = group.automationFor (target).isEnabled();
    }

    return clip;
}

int PianoRollComponent::addGroupCopy (Layer& layer, const GroupClip& source, double deltaBeats)
{
    // Created before the members so they can be born inside it: a note whose
    // groupId points at a group that does not exist yet would be repaired to
    // "ungrouped" by the model, and the copy would paste as loose notes.
    //
    // Phase 57: a PASTE no longer comes through here when the source group is
    // still alive (it reuses that group instead). This is now the branch for
    // "the source is gone" — a cut swept it away, or the paste landed on
    // another sheet — plus the deliberate copy that "复制到后一小节" makes.
    const int newId = layer.addGroup (uniqueGroupName (layer, source.name), source.colour);

    auto* copy = layer.findGroup (newId);

    if (copy == nullptr)
        return -1;

    copy->setOutputSlotId (source.outputSlotId);
    copy->setLevelDb (source.levelDb);
    copy->setPan (source.pan);
    copy->setPluginParam (source.pluginParam);
    copy->setCollapsed (false);

    for (int i = 0; i < numGroupAutomationTargets; ++i)
    {
        auto& lane = copy->automationFor ((GroupAutomationTarget) i);

        // A group lane is ABSOLUTE time, so a copy that lands later has to move
        // its curve with it — otherwise pasting four bars later would keep the
        // shape at the old beats and the notes would arrive at a flat line.
        auto points = source.lanes[(size_t) i].getPoints();

        for (auto& point : points)
            point.beat = juce::jmax (0.0, point.beat + deltaBeats);

        lane.loadPoints (std::move (points));
        lane.setEnabled (source.laneOn[(size_t) i]);
    }

    return newId;
}

void PianoRollComponent::insertGroupMembers (Layer& layer, int groupId, int occurrence,
                                             double delta,
                                             const std::vector<NoteEvent>& notes,
                                             const std::vector<SampleEvent>& clips,
                                             std::vector<int>& newNotes,
                                             std::vector<int>& newClips)
{
    // Phase 62: where this appearance sits, measured on the members that are
    // about to land. Stored before anything can be deleted, because from now on
    // the origin is what "same shape, other place" is measured from — deriving
    // it later would follow the earliest surviving member and drift.
    double origin = std::numeric_limits<double>::max();

    for (const auto& note : notes)
        origin = juce::jmin (origin, juce::jmax (0.0, note.startBeat + delta));

    for (const auto& clip : clips)
        origin = juce::jmin (origin, juce::jmax (0.0, clip.startBeat + delta));

    if (origin < std::numeric_limits<double>::max())
        layer.setOccurrenceStart (groupId, occurrence, origin);

    for (auto note : notes)
    {
        note.startBeat       = juce::jmax (0.0, note.startBeat + delta);
        note.id              = 0;
        note.groupId         = groupId;
        note.groupOccurrence = occurrence;

        if (const int newId = layer.addNote (note); newId > 0)
            newNotes.push_back (newId);
    }

    for (auto clip : clips)
    {
        clip.startBeat = juce::jmax (0.0, clip.startBeat + delta);
        clip.id        = 0;

        // Phase 56: a pasted block belongs to the group the paste targets, never
        // -1 — that was the bug: a group with blocks in it pasted as a group
        // plus loose audio beside it. Phase 58: and to the same APPEARANCE as
        // the notes beside it, which is what keeps the copy independent.
        clip.groupId         = groupId;
        clip.groupOccurrence = occurrence;

        ensureClipSlot (clip);

        if (const int newId = layer.addSample (std::move (clip)); newId > 0)
        {
            newClips.push_back (newId);

            if (const auto* created = layer.findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }
}

void PianoRollComponent::ensureClipSlot (SampleEvent& clip)
{
    // Phase 33: a CUT takes its sampler with it — the sweep runs the moment the
    // last block leaves, so the slot a clipboard holds may be gone by the time
    // it is pasted. Re-registering the file is what makes cut-then-paste a move
    // instead of a block pointed at nothing (which would quietly play the
    // layer's instrument instead of the sample).
    if (clip.outputSlotId >= 0 && rack.findSlot (clip.outputSlotId) == nullptr && clip.audio != nullptr)
        clip.outputSlotId = rack.addSampler (clip.audio, clip.path, clip.name);
}

bool PianoRollComponent::pasteNotesFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    // Phase 57: a group on the clipboard is ONE object with ONE paste, so every
    // entry point goes through pasteGroupFromClipboard. This function used to
    // mint a second group for it — the very thing the user asked to stop — and
    // two paths deciding "does a paste create a group" is how they drift.
    //
    // Phase 63: "a group" became "however many objects the clipboard holds",
    // and the loose notes are pasted by that same transaction now, so a mixed
    // selection lands with one delta instead of two.
    if (! groupClips.empty())
        return pasteGroupFromClipboard();

    if (noteClip.empty())
        return false;

    // Paste at the playhead when there is one — that is the only landmark on
    // this timeline that means "here". With the playhead parked exactly on the
    // block the copy came from, shift by the block's own length, because a
    // paste that lands on top of its source looks like nothing happened.
    double anchor = playheadBeat >= 0.0 ? playheadBeat : noteClipFirstBeat;

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - noteClipFirstBeat) < 1.0e-9)
        anchor = noteClipFirstBeat + juce::jmax (minimumNoteLength(), noteClipEndBeat - noteClipFirstBeat);

    // Same rounding as a drag: the anchor is snapped once and every note keeps
    // its offset, so the block arrives with its internal timing intact.
    const double delta = anchor - noteClipFirstBeat;

    // Loose notes only: a group clipboard returned above, so there is no group
    // to hand these to (Phase 57). Phase 62: and they must ARRIVE loose — the
    // NoteEvent they were copied from still carries its groupId, and pasting it
    // unchanged would quietly drop the copy back into the appearance the user
    // copied it out of (one bracket around two stretches, which is the thing
    // Phase 58 exists to stop).
    std::vector<int> newIds;
    pasteLooseNotesInto (*active, delta, newIds);

    if (newIds.empty())
        return false;

    ensureTimelineExtendsTo (anchor + juce::jmax (minimumNoteLength(), noteClipEndBeat - noteClipFirstBeat) + 4.0);
    setSelection (newIds);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已粘贴 ") + juce::String ((int) newIds.size()) + utf8 (" 个音符"));

    repaint();
    return true;
}

bool PianoRollComponent::pasteGroupFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || groupClips.empty())
        return false;

    //-- The anchor ----------------------------------------------------------
    // The whole clipboard gets ONE anchor — the earliest beat anything on it
    // stands at. A block-only group has no notes to read a start from, and a
    // group whose notes begin later than its first block would otherwise paste
    // shifted by that difference.
    //
    // Phase 63: one anchor for N objects is what keeps their SPACING. Anchoring
    // each group on its own start would stack "two groups four beats apart" on
    // a single beat and look like the merge this phase exists to undo.
    const double sourceFirst = clipboardFirstBeat();
    const double sourceEnd   = clipboardLastBeat();

    double anchor = playheadBeat >= 0.0 ? playheadBeat : sourceFirst;

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - sourceFirst) < 1.0e-9)
        anchor = sourceFirst + juce::jmax (minimumNoteLength(), sourceEnd - sourceFirst);

    const double delta = anchor - sourceFirst;

    std::vector<int> newNotes;
    std::vector<int> newClips;

    for (const auto& object : groupClips)
    {
        //-- Phase 57: a paste reuses the group it was copied from ------------
        // It used to mint a SECOND group ("Synth A 2"), so copying a group and
        // pasting it a bar later split the very thing the user copied into two
        // rows of the group list — two names, two trims to keep in sync, two
        // mounts. The clipboard carries the source id and the paste lends the
        // new members to that same group: the copy IS the group, in a second
        // place.
        //
        // The id is re-checked rather than trusted. Cut-then-paste sweeps the
        // emptied group away, and a paste on another sheet cannot resolve an id
        // from this one; in both cases the members still have to arrive inside
        // SOME group, so one is built from the clipboard's own name and colour.
        //
        // Phase 63: per OBJECT, so a clipboard holding two different groups
        // pastes two — and a clipboard holding two appearances of ONE group
        // pastes two more appearances of it rather than one bigger one.
        const bool reused = object.sourceGroupId >= 0
                              && active->findGroup (object.sourceGroupId) != nullptr;

        const int newGroupId = reused ? object.sourceGroupId
                                      : addGroupCopy (*active, object, delta);

        if (newGroupId < 0)
            continue;

        //-- Phase 58: the paste is a NEW APPEARANCE, not more members -------
        // Phase 57 lent the members to the group itself, which made one bracket
        // swallow both stretches: dragging the copy dragged the original, and
        // the only way to separate them was to disband the group. The paste
        // places another appearance of the same definition — same name, same
        // trim, same mount, same curves — with members of its own.
        const int occurrence = active->nextGroupOccurrence (newGroupId);

        std::vector<int> objectNotes;
        std::vector<int> objectClips;

        insertGroupMembers (*active, newGroupId, occurrence, delta,
                            object.notes, object.clips, objectNotes, objectClips);

        if (objectNotes.empty() && objectClips.empty())
        {
            // Only a group this paste CREATED may be thrown away — the reused
            // one was here before the paste and is not ours to delete.
            if (! reused)
                active->removeGroup (newGroupId);   // nothing landed; no bracket

            continue;
        }

        newNotes.insert (newNotes.end(), objectNotes.begin(), objectNotes.end());
        newClips.insert (newClips.end(), objectClips.begin(), objectClips.end());
    }

    //-- The loose leftovers of a mixed selection ----------------------------
    // Same delta as the objects: a selection of "two groups and a stray note"
    // has to land in the same relationship it was copied in, or the stray note
    // arrives somewhere it never stood.
    pasteLooseNotesInto (*active, delta, newNotes);
    pasteLooseSamplesInto (*active, delta, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;

    ensureTimelineExtendsTo (anchor + juce::jmax (minimumNoteLength(), sourceEnd - sourceFirst) + 4.0);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已粘贴 ") + getGroupClipboardName()
                   + membersDescription ((int) newNotes.size(), (int) newClips.size()));

    repaint();
    return true;
}

//==============================================================================
//  Phase 17: audio clips
//==============================================================================
juce::Rectangle<float> PianoRollComponent::getSampleScreenBounds (int layerIndex, int sampleId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return {};

    const float x0 = screenXForBeat (sample->startBeat);
    const float x1 = screenXForBeat (sample->endBeat());

    // Height is a whole number of pitch lanes, measured from the bottom lane the
    // clip covers up to the top of the highest one. `keyHeight` is the UI scale,
    // not the zoom, so the block always lines up with the keyboard beside it.
    const float top    = screenYForPitch (sample->topPitch());
    const float bottom = screenYForPitch (sample->pitch) + keyHeight;

    return { x0, top, juce::jmax (2.0f, x1 - x0), juce::jmax (keyHeight, bottom - top) };
}

std::shared_ptr<const SampleBuffer> PianoRollComponent::clipAudio (const Layer& layer,
                                                                   const SampleEvent& sample) const
{
    // One call, one rule. The engine publishes clips through the same resolver,
    // so "what the block looks like" and "what it sounds like" cannot drift.
    return routing::resolveSample (layer, sample, rack).audio;
}

//==============================================================================
//  Phase 29: the window onto the file — trim limits and fade handles
//==============================================================================
bool PianoRollComponent::applySampleTrim (const SampleEvent& origin, double wantedBeat, bool fromTail)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* clip = active->findSample (origin.id);

    if (clip == nullptr)
        return false;

    // Phase 69: the same floor the note resize uses, so a sample block can be
    // trimmed down to a 1/64 beat when the view is zoomed in that far.
    const double minLength = minimumNoteLength();

    if (fromTail)
    {
        double length = juce::jmax (minLength, wantedBeat - origin.startBeat);

        // Phase 51: no upper clamp, in ANY mode. The three modes answer "what
        // does the extra width sound like" differently and none of them needs
        // a limit here:
        //
        //   resample / tempo — the playback maths are ratio-based
        //   (stretchRatio = remaining source / length), so a longer block is a
        //   genuinely slower (or stretched) rendering of the same window, and
        //   the ratio clamps itself at publish time (0.125..8);
        //
        //   none — the window is what it is, and the width past the file's end
        //   is silence. The P30 argument against that was "a gesture that
        //   changes the picture and not the sound"; the user overruled it:
        //   being able to reserve visual space (and drag the tail without the
        //   block snapping back) is worth more than the guarantee. The
        //   waveform already draws only the columns that carry audio.
        //
        // Shorter is always allowed, which is why this clamps the lower bound
        // only.
        clip->lengthBeats = length;
    }
    else
    {
        const double end = origin.endBeat();

        // The mirror of the tail's limit: the head can go back only as far as
        // the start offset allows, because before that there is no audio left to
        // play and the block would grow into silence.
        const double offsetBeats = SampleEvent::beatsForSeconds (juce::jmax (0.0, origin.sourceOffsetSeconds),
                                                                 bpm);
        const double earliest = juce::jmax (0.0, origin.startBeat - offsetBeats);
        const double latest   = juce::jmax (earliest, end - minLength);
        const double newStart = juce::jlimit (earliest, latest, wantedBeat);

        const double deltaBeats = newStart - origin.startBeat;      // > 0 means trimmed in

        clip->startBeat   = newStart;
        clip->lengthBeats = juce::jmax (minLength, end - newStart);

        // The window moves with the edge, so the audio that is left stays where
        // it was on the timeline. Without this line a head trim would restart
        // the file at the new start — a cut, not a trim.
        clip->sourceOffsetSeconds = juce::jmax (0.0, origin.sourceOffsetSeconds
                                                       + deltaBeats * 60.0 / juce::jmax (1.0, bpm));
    }

    ensureTimelineExtendsTo (clip->endBeat());

    // Phase 62: a block's width, window and curves are the group's shape too.
    syncShapeOfClip (origin.id);
    return true;
}

bool PianoRollComponent::applySampleTrimDrag (const SampleEvent& primary, double wantedBeat, bool fromTail,
                                              const std::vector<SampleEvent>& siblings)
{
    if (! applySampleTrim (primary, wantedBeat, fromTail))
        return false;

    if (siblings.empty())
        return true;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return true;

    // The delta comes back from the block the pointer owns, NOT from
    // `wantedBeat`: the minimum length and (at the head) the source offset can
    // eat part of the drag. A sibling moved by the raw delta would then part
    // company with the block being dragged — which is exactly how "trim the
    // selection" turns into "one block snapped shut, the others somewhere
    // else".
    const auto* applied = active->findSample (primary.id);

    if (applied == nullptr)
        return true;

    const double delta = fromTail ? applied->endBeat() - primary.endBeat()
                                  : applied->startBeat - primary.startBeat;

    if (std::abs (delta) <= 1.0e-12)
        return true;

    for (const auto& sibling : siblings)
    {
        if (sibling.id == primary.id)
            continue;

        // Each sibling is clamped by its OWN limits (its own source offset at
        // the head, the minimum length at either end), which is why this calls
        // the same trim rather than adding `delta` to a field.
        applySampleTrim (sibling,
                         fromTail ? sibling.endBeat() + delta : sibling.startBeat + delta,
                         fromTail);
    }

    return true;
}

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
    hoveredSampleCurveMenu   = false;

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
    hoveredSampleCurveMenu   = false;

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
    return getSampleMenuArea (layerIndex, sampleId);
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

bool PianoRollComponent::handleSampleAutomationMouseDown (const juce::MouseEvent& e,
                                                          int layerIndex, int sampleId)
{
    auto* layer = stack.getLayer (layerIndex);
    auto* sample = layer != nullptr ? layer->findSample (sampleId) : nullptr;

    if (sample == nullptr)
        return false;

    const auto pos = e.position;

    if (getSampleCloseArea (layerIndex, sampleId).contains (pos))
    {
        closeSampleAutomation();
        return true;
    }

    // Phase 50: the row is the target menu. Phase 65 removed the enable switch
    // that used to split it: the label is now the whole row, and a click
    // anywhere on it opens the target list.
    if (getSampleTargetArea (layerIndex, sampleId).contains (pos))
    {
        showSampleCurveMenu (layerIndex, sampleId, e.getScreenPosition());
        return true;
    }

    if (getSampleMenuArea (layerIndex, sampleId).contains (pos))
        return true;        // inside the row but between the parts: swallow it

    const auto inner = getSampleCurveArea (layerIndex, sampleId);

    if (! inner.contains (pos))
        return true;        // inside the box but between the parts: swallow it

    const int hit = findSampleCurvePointAt (layerIndex, sampleId, pos);

    if (e.mods.isPopupMenu())
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

    const auto target = sample->automationTarget;
    const bool onPan  = target == SampleAutomationTarget::pan;

    juce::PopupMenu menu;

    //-- Which parameter this box edits --------------------------------------
    // Part of the same menu as the curve commands, because "switch to pan" and
    // "make it a left-to-right sweep" are two answers to one question and the
    // user is already looking at this row.
    menu.addSectionHeader (utf8 ("自动化目标"));
    menu.addItem (11, utf8 ("音量"), true, ! onPan);
    menu.addItem (12, utf8 ("声相"), true, onPan);
    menu.addSeparator();

    // Phase 65: no 启用 / 停用 item. The box's switch was removed for the same
    // reason — a drawn curve plays — and a menu item that brings the bypass back
    // would only move the lie one click away.
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

        // Switching the target is a view change, not a curve change: it must
        // not touch either shape, and it must be written down so reopening the
        // box shows the same curve (P42's "selection is state" rule).
        if (result == 11 || result == 12)
        {
            s->automationTarget = result == 12 ? SampleAutomationTarget::pan
                                               : SampleAutomationTarget::volume;

            if (safe->onNoteEdited != nullptr)
                safe->onNoteEdited();

            safe->repaint();
            return;
        }

        const float neutral = sampleAutomationNeutral (s->automationTarget);

        switch (result)
        {
            case 2:
                s->activeCurve().loadPoints ({ { 0.0f, neutral }, { 1.0f, neutral } });
                break;

            // The two shapes the old fade handles drew, kept as one-click
            // presets: the ramps that used to be the ONLY way to shape a block
            // are now two menu items, and a third point is free. On the pan
            // lane the same two ramps read as a sweep across the stereo field.
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
        const bool  onPan  = sample->automationTarget == SampleAutomationTarget::pan;

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

    //-- Menu row ------------------------------------------------------------
    auto menuArea = getSampleMenuArea (layerIndex, sample->id);

    if (! menuArea.isEmpty())
    {
        const bool hot = hoveredSampleCurveMenu;

        // Phase 65: the row is the target button and nothing else. The enable
        // switch is gone — a curve you drew is a curve that plays, and a switch
        // that can silently make the drawing a lie was the one thing in this
        // box that could turn the shape into decoration.
        const auto target = getSampleTargetArea (layerIndex, sample->id);

        // The label names the target and carries a caret: a static word next to
        // nothing reads as decoration, and "there is a second curve" would be
        // invisible without something that looks pressable.
        const juce::String label = sample->automationTarget == SampleAutomationTarget::pan
                                     ? utf8 ("声相") : utf8 ("音量");
        const auto labelFont = atomic::ui::bold (9.0f);

        g.setColour ((hot ? juce::Colours::white : colour).withAlpha (0.9f));
        g.setFont (labelFont);
        g.drawText (label,
                    target.withTrimmedRight (ui::pf (12)),
                    juce::Justification::centredLeft, true);

        juce::Path caret;
        const float cx = target.getRight() - ui::pf (8);
        const float cy = target.getCentreY();

        caret.startNewSubPath (cx - ui::pf (2.5f), cy - ui::pf (1.2f));
        caret.lineTo (cx, cy + ui::pf (1.6f));
        caret.lineTo (cx + ui::pf (2.5f), cy - ui::pf (1.2f));

        g.setColour ((hot ? juce::Colours::white : colour).withAlpha (0.75f));
        g.strokePath (caret, juce::PathStrokeType (ui::pf (1.2f), juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
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


int PianoRollComponent::sampleWaveColumns (const Layer& layer, const SampleEvent& sample,
                                           float blockWidth) const
{
    const auto audio = clipAudio (layer, sample);

    if (audio == nullptr || audio->lengthSeconds() <= 1.0e-6 || beatWidth <= 0.0f)
        return 0;

    const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
    const double secPerPixel    = secondsPerBeat / (double) beatWidth;

    if (secPerPixel <= 0.0)
        return 0;

    const double totalSeconds = audio->lengthSeconds();
    const double startSec     = juce::jlimit (0.0, totalSeconds, sample.sourceOffsetSeconds);
    const double usable       = totalSeconds - startSec;

    if (usable <= 0.0)
        return 0;

    // Phase 31: a block whose audio is squeezed into it shows the WHOLE file, so
    // its wave fills the block by definition. The two fixed-speed modes keep the
    // Phase 30 rule — one pixel is always the same slice of source — and simply
    // stop where the file does.
    if (sampleWaveFillsBlock (layer, sample))
        return juce::jmax (0, (int) blockWidth);

    // The epsilon keeps a block whose audio is exactly as wide as itself from
    // claiming one more column than there are samples to fill it.
    const int fileColumns = (int) std::ceil (usable / secPerPixel - 1.0e-9);

    return juce::jlimit (0, juce::jmax (0, (int) blockWidth), fileColumns);
}

int PianoRollComponent::sampleWaveColumnCount (int sampleId) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return 0;

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return 0;

    return sampleWaveColumns (*layer, *sample, getSampleScreenBounds (stack.getActiveLayerIndex(),
                                                                     sampleId).getWidth());
}

bool PianoRollComponent::restoreSampleLength (int sampleId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return false;

    auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return false;

    const auto buffer = clipAudio (*layer, *sample);

    if (buffer == nullptr || buffer->lengthSeconds() <= 0.0)
        return false;

    const double offsetBeats = SampleEvent::beatsForSeconds (juce::jmax (0.0, sample->sourceOffsetSeconds),
                                                             bpm);

    // Left by exactly the time the head trim ate, so the audio lands back on the
    // timeline where it was. Restoring the length without restoring the position
    // would be a cut, not an undo.
    sample->startBeat = juce::jmax (0.0, sample->startBeat - offsetBeats);

    // Phase 31: the length is the file's, to the last sample, and NOTHING is
    // snapped on the way out. A trim drag rounds to the 1/16 grid by design, so
    // a restore that inherited that rounding would hand back a length that is
    // "on the grid" and still not the file's — the exact complaint that made
    // this a menu item. The one click that means "put it back" must be the one
    // gesture in the clip's life whose result no other setting can perturb.
    sample->lengthBeats = SampleEvent::beatsForSeconds (buffer->lengthSeconds(), bpm);
    sample->sourceOffsetSeconds = 0.0;

    ensureTimelineExtendsTo (sample->endBeat());
    return true;
}

int PianoRollComponent::restoreSelectedSampleLengths()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty())
        return 0;

    // Copy the ids first: restoreSampleLength() touches the layer (timeline
    // extension), and iterating the selection while the model is being edited is
    // how a restore ends up skipping every other block.
    const auto ids = sampleSelection;
    int restored = 0;

    for (const int id : ids)
        if (restoreSampleLength (id))
            ++restored;

    return restored;
}

//==============================================================================
//  Phase 50: cut and reverse — two edits that act on the WINDOW, not the file
//==============================================================================
namespace
{
    /** The part of `env` between two normalised times, re-mapped onto 0..1.

        A split must not restart the shape: a ramp cut in half has to stay a
        ramp on both sides (each covering its own half), or the second half of a
        fade-out would jump back to full volume. Points strictly inside the
        window are carried over; the two new ends take the interpolated value so
        the seam is continuous by construction. */
    NoteEnvelope subEnvelope (const NoteEnvelope& env, float u0, float u1)
    {
        u0 = juce::jlimit (0.0f, 1.0f, u0);
        u1 = juce::jlimit (0.0f, 1.0f, u1);

        if (u1 - u0 < 1.0e-4f)
            u1 = juce::jmin (1.0f, u0 + 1.0e-4f);

        std::vector<EnvelopePoint> points;

        points.push_back ({ 0.0f, env.valueAt (u0) });

        for (const auto& p : env.getPoints())
            if (p.time > u0 + 1.0e-4f && p.time < u1 - 1.0e-4f)
                points.push_back ({ (p.time - u0) / (u1 - u0), p.value });

        points.push_back ({ 1.0f, env.valueAt (u1) });

        NoteEnvelope out;
        out.loadPoints (std::move (points));
        out.setEnabled (env.isEnabled());     // a bypassed curve stays bypassed
        return out;
    }
}

double PianoRollComponent::sampleWindowSeconds (const SampleEvent& sample, const Layer& layer) const
{
    // Seconds of SOURCE this block consumes. Not the block's length: a
    // resampled block eats the file at `speed`, which is the number the cut
    // has to hand the right-hand half — otherwise a split block plays the same
    // two seconds twice.
    const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
    const double outputSeconds  = juce::jmax (0.0, sample.lengthBeats * secondsPerBeat);
    const auto   playback       = samplePlayback (layer, sample);

    const double consumed = outputSeconds * playback.speed;

    if (clipAudio (layer, sample) == nullptr)
        return consumed;                   // no file: the arithmetic still holds

    return juce::jmin (consumed, juce::jmax (0.0, sample.remainingSourceSeconds()));
}

int PianoRollComponent::splitSelectedSamplesAtPlayhead()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty() || playheadBeat < 0.0)
        return 0;

    return splitSamplesAt (sampleSelection, playheadBeat);
}

int PianoRollComponent::splitSamplesAt (const std::vector<int>& ids, double at)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || ids.empty() || at < 0.0)
        return 0;

    // No snapping here: the knife snaps (or not) where it reads the click, and
    // the playhead path cuts exactly where the head is. One meaning per layer —
    // a cut function that also moves the cut would be two features in one.
    int split = 0;

    std::vector<int> newSelection;

    for (const int id : ids)
    {
        auto* clip = layer->findSample (id);

        if (clip == nullptr)
            continue;

        // Strictly inside. A cut exactly on an edge would produce a zero-length
        // block, which the renderer skips but the user then has to delete.
        if (at <= clip->startBeat + 1.0e-6 || at >= clip->endBeat() - 1.0e-6)
        {
            newSelection.push_back (id);
            continue;
        }

        const Layer& layerRef = *layer;       // sampleWindowSeconds takes a const Layer
        const double window   = sampleWindowSeconds (*clip, layerRef);
        const double u        = (at - clip->startBeat) / juce::jmax (1.0e-9, clip->lengthBeats);
        const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);

        // How much SOURCE the left half consumed. Not `window * u`: a block that
        // outruns its file spends the first `window / speed` seconds reading and
        // the rest in silence, so the source advances 1:1 until it runs out and
        // then stops. Scaling the leftover by u would hand the right half an
        // offset that is too small — the first P50 run measured 0.533 where 1.0
        // was correct, because the test file is 2 s and the block asked for 3.
        const double elapsedOutput = u * juce::jmax (0.0, clip->lengthBeats * secondsPerBeat);
        const double advance = juce::jmin (window,
                                           elapsedOutput * samplePlayback (layerRef, *clip).speed);

        const double offset  = clip->sourceOffsetSeconds;
        const bool   reverse = clip->reversed;
        const double rightLength = clip->endBeat() - at;

        SampleEvent right = *clip;            // curves, mount, group, lane all copy
        right.startBeat   = at;
        right.lengthBeats = rightLength;

        // Where the two windows sit in the file. Forwards, the right half picks
        // up where the left one stopped. Backwards, the SCREEN-left half is the
        // END of the file, so it is the one that moves — that asymmetry is the
        // whole meaning of "reversed", and getting it wrong would make a cut
        // reversed block repeat its tail.
        if (! reverse)
        {
            right.sourceOffsetSeconds  = offset + advance;
            clip->sourceOffsetSeconds  = offset;
        }
        else
        {
            right.sourceOffsetSeconds  = offset;
            clip->sourceOffsetSeconds  = offset + juce::jmax (0.0, window - advance);
        }

        const float seam = (float) juce::jlimit (0.0, 1.0, u);

        if (auto* clipCurve = layer->findSample (id))
        {
            clipCurve->volumeCurve = subEnvelope (clipCurve->volumeCurve, 0.0f, seam);
            clipCurve->panCurve    = subEnvelope (clipCurve->panCurve,    0.0f, seam);
            clipCurve->lengthBeats = at - clipCurve->startBeat;
        }

        right.volumeCurve = subEnvelope (right.volumeCurve, seam, 1.0f);
        right.panCurve    = subEnvelope (right.panCurve,    seam, 1.0f);

        const int newId = layer->addSample (std::move (right));

        newSelection.push_back (id);
        newSelection.push_back (newId);
        ++split;
    }

    if (split == 0)
        return 0;

    // Phase 62: same as the note cut — one member became two, everywhere.
    std::vector<int> cutClipIds (ids.begin(), ids.end());
    syncShapesOfMembers ({}, cutClipIds);

    ensureTimelineExtendsTo (at);
    setSampleSelection (newSelection);
    stack.notifyNotesChanged();
    repaint();

    return split;
}

int PianoRollComponent::splitNotesAt (const std::vector<int>& ids, double at)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || ids.empty() || at < 0.0)
        return 0;

    int split = 0;
    std::vector<int> newSelection;

    for (const int id : ids)
    {
        auto* note = layer->findNote (id);

        if (note == nullptr)
            continue;

        // Strictly inside, same rule as the block's cut: an edge cut would
        // create a zero-length note that nothing plays and the eraser must
        // then clean up.
        if (at <= note->startBeat + 1.0e-6 || at >= note->endBeat() - 1.0e-6)
        {
            newSelection.push_back (id);
            continue;
        }

        const double u = (at - note->startBeat) / juce::jmax (1.0e-9, note->lengthBeats);

        NoteEvent right = *note;          // pitch, velocity, mount, group all copy
        right.startBeat   = at;
        right.lengthBeats = note->endBeat() - at;
        right.envelope    = subEnvelope (right.envelope, (float) u, 1.0f);

        note->envelope    = subEnvelope (note->envelope, 0.0f, (float) u);
        note->lengthBeats = at - note->startBeat;

        // Both halves stay in the group: cutting one member of a group makes
        // two members, not one member and one stranger. The group's curve
        // keeps playing both — which is what "these notes are one thing"
        // means even after the user slices through one of them.
        const int newId = layer->addNote (std::move (right));

        newSelection.push_back (id);
        newSelection.push_back (newId);
        ++split;
    }

    if (split == 0)
        return 0;

    // Phase 62: a cut replaces one member with two — a change of shape, so the
    // other appearances get the same cut.
    std::vector<int> cutIds (ids.begin(), ids.end());
    syncShapesOfMembers (cutIds, {});

    setSelection (newSelection);
    stack.notifyNotesChanged();
    repaint();

    return split;
}

int PianoRollComponent::toggleSelectedSampleReverse()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty())
        return 0;

    const auto ids = sampleSelection;
    int changed = 0;

    for (const int id : ids)
        if (auto* clip = layer->findSample (id))
        {
            clip->reversed = ! clip->reversed;
            ++changed;
        }

    if (changed == 0)
        return 0;

    // Phase 62: "this block plays backwards" is part of the shape.
    syncShapesOfMembers ({}, ids);

    stack.notifyNotesChanged();
    repaint();

    return changed;
}

//==============================================================================
//  Phase 31: stretch modes — the picture and the sound obey ONE decision
//==============================================================================
clipstretch::Playback PianoRollComponent::samplePlayback (const Layer& layer,
                                                          const SampleEvent& sample) const
{
    // The file that actually plays, not the one the block was created from: a
    // clip mounted on a sampler stretches against that sampler's audio.
    if (clipAudio (layer, sample) == nullptr)
        return clipstretch::Playback::make (sample.stretchMode, 1.0);

    // Same ratio the engine publishes (natural / drawn), so a badge that says
    // "×1.50" and a render that runs at 1.50 are the same number, not two
    // numbers that happen to be close.
    return clipstretch::Playback::make (sample.stretchMode, sample.stretchRatio (bpm));
}

bool PianoRollComponent::sampleWaveFillsBlock (const Layer& layer, const SampleEvent& sample) const
{
    switch (sample.stretchMode)
    {
        case StretchMode::resample:     // the whole file at a new rate
        case StretchMode::tempo:        // the whole file, pitch preserved
            return clipAudio (layer, sample) != nullptr;

        case StretchMode::none:         // a window onto the file
        case StretchMode::pitch:        // still reads at the file's own speed
        default:
            return false;
    }
}

void PianoRollComponent::setSampleStretchMode (int sampleId, StretchMode mode)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return;

    if (auto* clip = layer->findSample (sampleId))
    {
        clip->stretchMode = mode;
        syncShapeOfClip (sampleId);      // Phase 62: the shape, not this copy
    }
}

int PianoRollComponent::setSelectedSampleStretchMode (StretchMode mode)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return 0;

    // The SELECTION, strictly. The menu decides whether the block it was opened
    // on is part of it (if it is, one command changes them all; if not, the
    // command changes only that block) — this function has no idea which block
    // was clicked, so it must not pretend to.
    const auto ids = sampleSelection;

    if (ids.empty())
        return 0;

    int changed = 0;

    for (const int id : ids)
        if (auto* clip = layer->findSample (id))
        {
            clip->stretchMode = mode;
            ++changed;
        }

    if (changed > 0)
        syncShapesOfMembers ({}, ids);   // Phase 62: one shape, every place

    return changed;
}

bool PianoRollComponent::simulateSampleTrim (int sampleId, double wantedBeat, bool fromTail)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    const auto* clip = active->findSample (sampleId);

    if (clip == nullptr)
        return false;

    const SampleEvent origin = *clip;        // the drag works from a snapshot too

    // Phase 56: the hook has to go through the same "one delta for the whole
    // selection" path the pointer does, or it would be testing a trim that no
    // longer exists. Grabbing a selected block's edge means the selection is
    // the gesture.
    std::vector<SampleEvent> siblings;

    if (isSampleSelected (sampleId) && sampleSelection.size() > 1)
        for (int id : sampleSelection)
            if (id != sampleId)
                if (const auto* other = active->findSample (id))
                    siblings.push_back (*other);

    if (! applySampleTrimDrag (origin, wantedBeat, fromTail, siblings))
        return false;

    stack.notifyNotesChanged();
    return true;
}

SampleEvent* PianoRollComponent::hitTestSample (juce::Point<float> position)
{
    auto* layer = stack.getActiveLayer();

    // Same rule as notes: only the active, visible sheet answers the mouse.
    if (layer == nullptr)
        return nullptr;

    const int index = stack.getActiveLayerIndex();

    // Phase 58: LAST first. Blocks are drawn in list order, so the last one is
    // what the pointer is looking at when two of them overlap — and a hit test
    // that answers with the earliest block grabs the one UNDER the one you
    // aimed at. That is how "drag this block's tail" became "some other block
    // moved": the grab landed on a neighbour whose edge happened to be there.
    const auto& samples = layer->getSamples();

    for (auto it = samples.rbegin(); it != samples.rend(); ++it)
        if (getSampleScreenBounds (index, it->id).contains (position))
            return layer->findSample (it->id);

    return nullptr;
}

const SampleEvent* PianoRollComponent::hitTestSample (juce::Point<float> position) const
{
    const auto* layer = stack.getActiveLayer();

    // Same rule as notes: only the active, visible sheet answers the mouse.
    if (layer == nullptr)
        return nullptr;

    const int index = stack.getActiveLayerIndex();

    // Phase 58: topmost wins — see the non-const overload.
    const auto& samples = layer->getSamples();

    for (auto it = samples.rbegin(); it != samples.rend(); ++it)
        if (getSampleScreenBounds (index, it->id).contains (position))
            return layer->findSample (it->id);

    return nullptr;
}

std::pair<int, bool> PianoRollComponent::selectedTrimEdgeAt (juce::Point<float> position) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty())
        return { -1, false };

    const int   index    = stack.getActiveLayerIndex();
    const float band     = ui::pf (kResizeHandleWidth);
    const auto& samples  = layer->getSamples();

    int   bestId   = -1;
    bool  bestTail = false;
    float bestDist = 1.0e9f;

    // Topmost first, the order hitTestSample answers in: the block drawn last
    // is the one under the pointer, so ties go to it.
    for (auto it = samples.rbegin(); it != samples.rend(); ++it)
    {
        if (! isSampleSelected (it->id))
            continue;

        const auto b = getSampleScreenBounds (index, it->id);

        //-- Phase 59: the pointer has to be INSIDE the block ---------------
        // Without this test `x >= right - band` was true for every pixel to the
        // RIGHT of a block, and `x <= left + band` for every pixel to its LEFT.
        // So with three blocks selected, a press on the middle one's tail was
        // answered by the next block's head (its head band reaches leftwards
        // forever), and the trim then ran on a block whose head cannot move —
        // the user saw "only the rightmost block trims, the others sometimes do
        // nothing at all". The bands are for the inside of the edge; a pointer
        // past the block is not aiming at that block.
        if (b.isEmpty()
              || position.y < b.getY() || position.y > b.getBottom()
              || position.x < b.getX() || position.x > b.getRight())
            continue;

        const float toTail = b.getRight() - position.x;
        const float toHead = position.x - b.getX();

        // The NEARER edge wins. Two blocks that share a boundary both answer
        // "in band" for the same pixel; picking by distance makes that pixel
        // mean "the edge it is drawn on", not "whichever block came first".
        const bool  tail = toTail <= toHead;
        const float dist = tail ? toTail : toHead;

        if (dist > band || dist >= bestDist)
            continue;

        bestId   = it->id;
        bestTail = tail;
        bestDist = dist;
    }

    return bestId >= 0 ? std::pair<int, bool> { bestId, bestTail }
                       : std::pair<int, bool> { -1, false };
}

int PianoRollComponent::getEditableSampleIdAt (juce::Point<float> position) const
{
    if (auto* sample = hitTestSample (position))
        return sample->id;

    return -1;
}

std::pair<int, bool> PianoRollComponent::getBlockGrabAtForTest (juce::Point<float> at) const
{
    // The press path's two steps, in its order. Kept as one callable so the
    // self-test exercises the ORDER and not just the two halves separately:
    // both halves could be right while the order is what broke.
    const auto edge = selectedTrimEdgeAt (at);

    if (edge.first >= 0)
        return edge;

    if (const auto* hit = hitTestSample (at))
        return { hit->id, false };

    return { -1, false };
}

int PianoRollComponent::getActiveLayerSampleCount() const
{
    const auto* layer = stack.getActiveLayer();

    return layer != nullptr ? layer->getSampleCount() : 0;
}

//==============================================================================
//  Phase 28: selecting, moving, copying clips
//==============================================================================
bool PianoRollComponent::deleteSelectedSamples()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return false;

    // Phase 62: a block that leaves the shape leaves it in every appearance.
    std::vector<int> twinNotes, twinClips;
    collectTwinsForEdit (twinNotes, twinClips);

    for (int id : sampleSelection)
        active->removeSample (id);

    for (int id : twinClips)
        active->removeSample (id);

    const int removed = (int) sampleSelection.size();
    sampleSelection.clear();

    // A group whose last member was a clip has just become an empty bracket.
    pruneEmptyGroups();
    stack.notifyNotesChanged();

    // Phase 32: the blocks are gone, so the samplers that only existed to play
    // them have no reason to stay. The host owns the rack, so it does the
    // counting — see MainComponent::pruneOrphanSamplerSlots.
    if (onSampleReferencesChanged != nullptr)
        onSampleReferencesChanged();

    notifySelectionChanged();

    flashStatus (utf8 ("已删除 ") + juce::String (removed) + utf8 (" 个采样"));
    repaint();
    return true;
}

//==============================================================================
//  Phase 32: a block and its sampler are two views of one thing
//==============================================================================
int PianoRollComponent::deleteSamplesMountedOn (int slotId)
{
    if (slotId < 0)
        return 0;

    // Every layer, not just the active one: a sampler is rack-wide, and a clip
    // on a hidden layer still points at it. Leaving those behind would produce
    // exactly the dangling mount this phase exists to prevent.
    int removed = 0;

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        // Collected first: removeSample() rewrites the vector this loop reads.
        std::vector<int> doomed;

        for (const auto& clip : layer->getSamples())
            if (clip.outputSlotId == slotId)
                doomed.push_back (clip.id);

        for (const int id : doomed)
        {
            layer->removeSample (id);
            ++removed;
        }
    }

    if (removed == 0)
        return 0;

    // The selection may now name blocks that no longer exist, and a dangling id
    // makes Delete look broken.
    if (auto* active = stack.getActiveLayer())
        sampleSelection.erase (std::remove_if (sampleSelection.begin(), sampleSelection.end(),
                                               [active] (int id)
                                               { return active->findSample (id) == nullptr; }),
                               sampleSelection.end());

    pruneEmptyGroups();
    stack.notifyNotesChanged();
    notifySelectionChanged();

    // Deliberately NOT onSampleReferencesChanged: the caller is the host, and it
    // is already removing this slot. Asking it to prune again mid-removal would
    // re-enter the rack's own change notification.
    flashStatus (utf8 ("已删除 ") + juce::String (removed) + utf8 (" 个采样"));
    repaint();
    return removed;
}

//==============================================================================
//  Phase 34: the question the cascade deletion has to ask first
//==============================================================================
int PianoRollComponent::countSamplesMountedOn (int slotId) const
{
    if (slotId < 0)
        return 0;

    int count = 0;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = stack.getLayer (i))
            for (const auto& clip : layer->getSamples())
                if (clip.outputSlotId == slotId)
                    ++count;

    return count;
}

juce::StringArray PianoRollComponent::namesOfSamplesMountedOn (int slotId, int limit) const
{
    juce::StringArray names;

    if (slotId < 0 || limit <= 0)
        return names;

    for (int i = 0; i < stack.getNumLayers() && names.size() < limit; ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (layer == nullptr)
            continue;

        for (const auto& clip : layer->getSamples())
        {
            if (clip.outputSlotId != slotId)
                continue;

            names.add (clip.name.isNotEmpty() ? clip.name : utf8 ("未命名采样"));

            if (names.size() >= limit)
                break;
        }
    }

    return names;
}

bool PianoRollComponent::mountSelectedSamplesToSlot (int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty())
        return false;

    const auto* slot = slotId >= 0 ? rack.findSlot (slotId) : nullptr;

    // Only a sampler holds a file to hand over. An instrument slot is a
    // legitimate destination in Phase 27's routing-only world, but this command
    // promises to change WHAT the block holds, and an oscillator has nothing to
    // give. -1 ("follow the layer") is allowed: that is how a block lets go.
    if (slotId >= 0 && (slot == nullptr || ! slot->isSampler()))
        return false;

    // Copied: the loop touches the model, and the history capture the host runs
    // on notifyNotesChanged() can rewrite the selection while it is iterated.
    const auto ids = sampleSelection;
    bool changed = false;

    for (const int id : ids)
    {
        auto* sample = layer->findSample (id);

        if (sample == nullptr)
            continue;

        if (slotId < 0)
        {
            // Keep what is audible right now, then let go of the mount: without
            // this, "follow the layer output" would snap a re-mounted block back
            // to the file it was originally dropped from, i.e. it would change
            // the sound in a menu item that claims to stop naming a synth.
            if (auto heard = routing::resolveSample (*layer, *sample, rack).audio)
                sample->audio = std::move (heard);

            sample->outputSlotId = -1;
            changed = true;
            continue;
        }

        if (slot->sampleBuffer == nullptr || slot->sampleBuffer->lengthSeconds() <= 0.0)
        {
            // A slot whose file is gone keeps the block as it was. The mount is
            // recorded, the block still draws the waveform it had, and the
            // missing audio is the rack's story to tell.
            sample->outputSlotId = slotId;
            changed = true;
            continue;
        }

        // Already on this sampler and already holding its file: nothing to hand
        // over. Without this early out, clicking the card a selected block is
        // ALREADY on would reset the block's length and offset to the whole
        // file — a trim the user made, silently undone by "look at this synth".
        if (sample->outputSlotId == slotId && sample->audio == slot->sampleBuffer)
            continue;

        sample->outputSlotId = slotId;
        sample->audio = slot->sampleBuffer;
        sample->name  = slot->name.isNotEmpty() ? slot->name : sample->name;
        sample->path  = slot->samplePath;

        // The block becomes a window onto the NEW file — whole. Keeping the old
        // length would leave it claiming seconds the new file does not have,
        // which draws as a waveform that stops halfway.
        sample->sourceOffsetSeconds = 0.0;
        sample->lengthBeats = juce::jmax (gridDivision,
                                          SampleEvent::beatsForSeconds (
                                              slot->sampleBuffer->lengthSeconds(), bpm));

        ensureTimelineExtendsTo (sample->endBeat());
        changed = true;
    }

    if (! changed)
        return false;

    stack.notifyNotesChanged();
    notifySelectionChanged();       // the highlight has to follow the new mount

    // The slot we left behind may now have no blocks at all.
    if (onSampleReferencesChanged != nullptr)
        onSampleReferencesChanged();

    repaint();
    return true;
}

bool PianoRollComponent::replaceSamplesWithSlot (int sampleId, int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->findSample (sampleId) == nullptr)
        return false;

    // Only a sampler holds a file to hand over (Phase 27's rule, restated
    // here for the gesture): an oscillator has nothing to give, so "replace
    // the sample" has no meaning on one.
    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr || ! slot->isSampler() || slot->sampleBuffer == nullptr)
        return false;

    // Already part of a multi-block selection? Then the selection is the
    // target — the user said "these" before reaching for the card. Otherwise
    // the block under the pointer is, and it becomes the selection so the
    // result is visible as "the one I just changed".
    if (! isSampleSelected (sampleId))
        setSampleSelection ({ sampleId });

    // Phase 32's rule is the whole implementation: name, path, length and
    // offset follow the new file, startBeat does not, and re-mounting a block
    // that already holds this file is a no-op rather than a silent re-trim.
    return mountSelectedSamplesToSlot (slotId);
}

bool PianoRollComponent::routeNoteToSlot (int noteId, int slotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || layer->findNote (noteId) == nullptr)
        return false;

    const auto* slot = rack.findSlot (slotId);

    if (slot == nullptr || slot->isSampler())
        return false;

    if (! isSelected (noteId))
        setSelection ({ noteId });

    // routeSelectionTo writes the note-level override — the tier that means
    // "just this note". Deliberately NOT mountSelectionTo: that one mounts the
    // whole GROUP when the selection happens to be one, and a card dropped on a
    // single note is the most local statement there is (P24's lesson).
    routeSelectionTo (slotId);
    return true;
}

int PianoRollComponent::selectedSamplerSlotId() const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    for (const int id : sampleSelection)
    {
        const auto* sample = layer->findSample (id);

        if (sample == nullptr)
            continue;

        // viaSampler only: a block that follows its layer is played by an
        // INSTRUMENT, and lighting an instrument card because a clip happens to
        // come out of it would claim the card holds the file. It does not.
        const auto resolved = routing::resolveSample (*layer, *sample, rack);

        if (resolved.viaSampler && resolved.slotId >= 0)
            return resolved.slotId;
    }

    return -1;
}

bool PianoRollComponent::deleteSelectedNotes()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || selection.empty())
        return false;

    const int removed = (int) selection.size();

    // Phase 62: a note that leaves the shape leaves it in every appearance.
    // Collected first — the pairing needs the members that are about to go.
    std::vector<int> twinNotes, twinClips;
    collectTwinsForEdit (twinNotes, twinClips);

    for (int id : selection)
        active->removeNote (id);

    for (int id : twinNotes)
        active->removeNote (id);

    selection.clear();
    hoveredNoteId = -1;
    notifySelectionChanged();

    // A group whose last member just went is an empty bracket that would keep
    // its curve; pruning is the only thing that tells the host to drop it.
    pruneEmptyGroups();
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已删除 ") + juce::String (removed) + utf8 (" 个音符"));
    repaint();
    return true;
}

void PianoRollComponent::captureSampleDragOriginals()
{
    dragClipOriginals.clear();

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& clip : active->getSamples())
        if (isSampleSelected (clip.id))
            dragClipOriginals.push_back (clip);
}

void PianoRollComponent::captureEditOriginals()
{
    // Notes first: captureDragOriginals() owns the clearing of both lists, so
    // this is "everything that moves", stated once instead of in each caller.
    captureDragOriginals();
    captureSampleDragOriginals();
}

void PianoRollComponent::armSampleMove (juce::Point<float> position)
{
    // Phase 29: a mixed selection drags as one object, the same way a group
    // does. What is armed is whatever is selected, not "the clips" — clicking a
    // block when notes are also selected must not leave the notes behind.
    captureEditOriginals();

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return;

    // The rigid move, not a second implementation of it: `moveNote` is "move
    // what is armed" and what is armed here may be clips, notes, or both.
    // clampMove() then applies the block rule — the leftmost member stops at
    // beat 0 and takes the rest with it — which is exactly what a multi-object
    // drag needs.
    dragMode         = DragMode::moveNote;
    dragAnchorBeat   = beatAtScreenX (position.x);
    dragAnchorPitch  = pitchAtScreenY (position.y);

    if (! dragOriginals.empty())
        dragAuditionPitch = dragOriginals.front().pitch;

    dragResizeNoteId = -1;
}

bool PianoRollComponent::simulateSampleMove (double deltaBeats, int deltaPitch)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return false;

    captureSampleDragOriginals();

    if (dragClipOriginals.empty())
        return false;

    const auto move = clampMove ({}, deltaBeats, deltaPitch, false, dragClipOriginals);
    applyMove (move);
    stack.notifyNotesChanged();

    return true;
}

std::pair<std::vector<int>, std::vector<int>>
PianoRollComponent::hitsInMarquee (juce::Rectangle<float> marquee) const
{
    std::pair<std::vector<int>, std::vector<int>> hits;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return hits;

    // Phase 29: the band grabs BOTH kinds. "Everything in this rectangle" is
    // what a marquee means, and making the user sweep twice — once for notes,
    // once for blocks — was the marquee answering a question nobody asked.
    const int index = stack.getActiveLayerIndex();

    for (const auto& note : active->getNotes())
        if (getNoteBounds (note).intersects (marquee))
            hits.first.push_back (note.id);

    for (const auto& clip : active->getSamples())
        if (getSampleScreenBounds (index, clip.id).intersects (marquee))
            hits.second.push_back (clip.id);

    return hits;
}

bool PianoRollComponent::simulateMarquee (juce::Rectangle<float> marquee)
{
    auto hits = hitsInMarquee (marquee);

    if (hits.first.empty() && hits.second.empty())
    {
        setMixedSelection ({}, {});
        return false;
    }

    setMixedSelection (std::move (hits.first), std::move (hits.second));
    return true;
}

bool PianoRollComponent::simulateRulerZoom (float anchorX, float totalDy)
{
    const auto ruler = getRulerArea().toFloat();

    if (! ruler.contains (anchorX, ruler.getCentreY()))
        return false;

    // Exactly what mouseDown captures...
    dragMode                 = DragMode::rulerZoom;
    rulerDragAnchorBeatWidth = beatWidth;
    rulerZoomAnchorX         = anchorX;
    rulerZoomAnchorBeat      = beatAtScreenX (anchorX);

    // ...and exactly what mouseDrag does with the total travel. `totalDy` is the
    // signed distance the pointer moved, which is what the drag algorithm
    // measures; passing it once is the same as a drag with one mouse-move.
    setBeatWidthAround ((float) (rulerDragAnchorBeatWidth * std::pow (1.006, (double) totalDy)),
                        rulerZoomAnchorBeat, rulerZoomAnchorX);

    dragMode = DragMode::none;
    return true;
}

bool PianoRollComponent::duplicateGroupToNextBar (int layerIndex, int groupId, int occurrence)
{
    // The selection this command writes is the ACTIVE layer's, so a group on
    // another sheet has to bring that sheet forward first — otherwise the copy
    // lands on one layer and the highlight on another.
    if (stack.getActiveLayerIndex() != layerIndex)
        stack.setActiveLayerIndex (layerIndex);

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return false;

    // Read once: adding the copy reallocates the group list, so `group` must
    // not be dereferenced after that (P53's dangling-pointer lesson, one
    // container over).
    const juce::String groupName = group->getName();

    std::vector<NoteEvent>    notes;
    std::vector<SampleEvent>  clips;
    double first = std::numeric_limits<double>::max();
    double last  = 0.0;

    // Phase 58: one appearance, unless the caller does not know which (the
    // older "copy the whole group" call, and the self-test, pass -1).
    const auto belong = [groupId, occurrence] (int memberGroup, int memberOccurrence)
    {
        return memberGroup == groupId && (occurrence < 0 || memberOccurrence == occurrence);
    };

    for (const auto& note : layer->getNotes())
        if (belong (note.groupId, note.groupOccurrence))
        {
            notes.push_back (note);
            first = juce::jmin (first, note.startBeat);
            last  = juce::jmax (last, note.endBeat());
        }

    for (const auto& clip : layer->getSamples())
        if (belong (clip.groupId, clip.groupOccurrence))
        {
            clips.push_back (clip);
            first = juce::jmin (first, clip.startBeat);
            last  = juce::jmax (last, clip.endBeat());
        }

    if (notes.empty() && clips.empty())
        return false;

    if (first == std::numeric_limits<double>::max())
        first = 0.0;

    //-- Where the copy goes ------------------------------------------------
    // "复制到后一小节" is the next BAR LINE, not "start + 4 beats" — the two are
    // the same only for a group that starts on a bar line, and a copy that
    // lands on the source is not a copy. If the group is longer than the gap,
    // the next line after its own end is used instead, so the two never
    // overlap.
    double target = nextBarLineAfter (first, gridDivision);

    if (target < last - 1.0e-9)
        target = barLineAtOrAfter (last);

    const double delta = target - first;

    // Phase 58: another APPEARANCE of the same group, not a copy of it. The
    // definition stays one row in the group list; what multiplies is the thing
    // on the timeline, and each one moves on its own. That also means there is
    // no second NoteGroup to build — the name, trim, mount and curves are
    // already there, and the new members simply join them.
    const int copyId        = groupId;
    const int newOccurrence = layer->nextGroupOccurrence (groupId);

    std::vector<int> newNotes;
    std::vector<int> newClips;

    insertGroupMembers (*layer, copyId, newOccurrence, delta, notes, clips, newNotes, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;       // nothing landed; the group is not ours to delete

    ensureTimelineExtendsTo (last + delta + beatsPerBar);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已复制 ") + groupName + membersDescription ((int) newNotes.size(), (int) newClips.size())
                   + utf8 (" 到第 ") + juce::String ((int) std::floor (target / beatsPerBar) + 1)
                   + utf8 (" 小节"));

    repaint();
    return true;
}

bool PianoRollComponent::placeGroupAppearance (int layerIndex, int groupId, double atBeat)
{
    // The selection and the notifications this ends with are the ACTIVE layer's,
    // and the row may have come from another sheet — so bring it forward first,
    // for the same reason duplicateGroupToNextBar does.
    if (stack.getActiveLayerIndex() != layerIndex)
        stack.setActiveLayerIndex (layerIndex);

    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return false;

    // Read before anything grows a vector: adding members can reallocate the
    // group list, so `group` must not be dereferenced after this point.
    const juce::String groupName = group->getName();

    //-- Template: the group's FIRST appearance -----------------------------
    // The row in the list is "this group", not "wherever it was last put".
    // First appearance keeps the answer stable: drag twice from the same row
    // and you get the same shape twice, instead of a copy of a copy.
    const auto occurrences = layer->getGroupOccurrences (groupId);

    if (occurrences.empty())
        return false;

    const int source = occurrences.front();

    std::vector<NoteEvent>   notes;
    std::vector<SampleEvent> clips;
    double first = std::numeric_limits<double>::max();
    double last  = 0.0;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId && note.groupOccurrence == source)
        {
            notes.push_back (note);
            first = juce::jmin (first, note.startBeat);
            last  = juce::jmax (last, note.endBeat());
        }

    for (const auto& clip : layer->getSamples())
        if (clip.groupId == groupId && clip.groupOccurrence == source)
        {
            clips.push_back (clip);
            first = juce::jmin (first, clip.startBeat);
            last  = juce::jmax (last, clip.endBeat());
        }

    if (notes.empty() && clips.empty())
        return false;       // nothing to place; do not invent an empty body

    if (first == std::numeric_limits<double>::max())
        first = 0.0;

    // The pointer's beat is where the FIRST member goes, not where the group
    // object starts in some abstract sense: the members are what you can see.
    const double target = juce::jmax (0.0, atBeat);
    const double delta  = target - first;

    // Phase 58: another appearance of the SAME definition. Same name, colour,
    // mount, trims and curves — the group list gains no row, and this copy
    // moves on its own because it is a different (groupId, occurrence) pair.
    const int newOccurrence = layer->nextGroupOccurrence (groupId);

    std::vector<int> newNotes;
    std::vector<int> newClips;

    insertGroupMembers (*layer, groupId, newOccurrence, delta, notes, clips, newNotes, newClips);

    if (newNotes.empty() && newClips.empty())
        return false;

    ensureTimelineExtendsTo (last + delta + beatsPerBar);
    setMixedSelection (newNotes, newClips);
    stack.notifyNotesChanged();

    flashStatus (groupName + utf8 (" 又放了一处：")
                   + membersDescription ((int) newNotes.size(), (int) newClips.size()));

    repaint();
    return true;
}

std::vector<int> PianoRollComponent::duplicateSelectedSamples (double offsetBeats)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return {};

    std::vector<SampleEvent> sources;
    double longest = 0.0;

    for (const auto& clip : active->getSamples())
        if (isSampleSelected (clip.id))
        {
            sources.push_back (clip);
            longest = juce::jmax (longest, clip.lengthBeats);
        }

    if (sources.empty())
        return {};

    // "Copy to the next bar" means "clear of the source", so the default offset
    // is the length of the longest copied block, not a fixed beat count: two
    // five-bar blocks copied by one bar would overlap themselves.
    const double offset = offsetBeats >= 0.0 ? offsetBeats : juce::jmax (gridDivision, longest);

    std::vector<int> newIds;

    for (auto clip : sources)
    {
        clip.startBeat += offset;
        clip.id = 0;                 // addSample() assigns the next id

        // A copy is a new object, not a second membership: inheriting the group
        // would make the group's bracket swallow the copy and drag it later on,
        // which is not what "duplicate" promised.
        clip.groupId = -1;

        const int newId = active->addSample (std::move (clip));

        if (newId > 0)
        {
            newIds.push_back (newId);

            if (const auto* created = active->findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }

    if (newIds.empty())
        return {};

    setSampleSelection (newIds);
    stack.notifyNotesChanged();

    flashStatus (utf8 ("已复制 ") + juce::String ((int) newIds.size()) + utf8 (" 个采样"));
    return newIds;
}

bool PianoRollComponent::copySamplesToClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleSelection.empty())
        return false;

    sampleClip.clear();
    sampleClipFirstBeat = std::numeric_limits<double>::max();
    double endBeat = 0.0;

    for (const auto& clip : active->getSamples())
    {
        if (! isSampleSelected (clip.id))
            continue;

        sampleClip.push_back (clip);
        sampleClipFirstBeat = juce::jmin (sampleClipFirstBeat, clip.startBeat);
        endBeat             = juce::jmax (endBeat, clip.endBeat());
    }

    if (sampleClip.empty())
    {
        sampleClipFirstBeat = 0.0;
        return false;
    }

    sampleClipLengthBeat = juce::jmax (gridDivision, endBeat - sampleClipFirstBeat);

    // Phase 37: a fresh copy starts a fresh streak. Keeping the old next-beat
    // would send the first paste of a new clipboard to the tail of the previous
    // one instead of to the playhead.
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    flashStatus (utf8 ("已复制 ") + juce::String ((int) sampleClip.size()) + utf8 (" 个采样"));
    return true;
}

bool PianoRollComponent::cutSamplesToClipboard()
{
    if (! copySamplesToClipboard())
        return false;

    return deleteSelectedSamples();
}

bool PianoRollComponent::pasteSamplesFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || sampleClip.empty())
        return false;

    //-- Where this paste lands (Phase 37) ------------------------------------
    // Three rules, in this order.
    //
    //  1. A streak of Ctrl+V steps forward. The first paste goes to the
    //     playhead (or the clipboard's own start); every repeat starts where
    //     the previous one ended. With one fixed anchor the second Ctrl+V
    //     lands exactly on the first, which is why "paste three times" used to
    //     leave one block on the timeline and look like the earlier pastes had
    //     been dragged away. Moving the playhead ends the streak.
    //
    //  2. With snap on, the LANDING BEAT is a grid line. Snapping the anchor
    //     alone used to be enough only while the step-forward came before the
    //     snap; applying it after handed the next paste a beat that is off the
    //     grid whenever the clipboard length is not whole grid steps. So the
    //     step is taken in grid units, and the advance recorded for the next
    //     paste is rounded UP to a grid line as well.
    //
    //  3. A paste that would land on its own source moves past it. Rounding
    //     can eat that move (grid 0.25, clip 0.375 → 0.25 lands inside the
    //     source), so this step rounds up.
    auto snapUp = [this] (double beat)
    {
        return std::ceil (beat / gridDivision - 1.0e-9) * gridDivision;
    };

    const bool continuing = samplePasteNextBeat >= 0.0
                             && std::abs (playheadBeat - samplePastePlayheadSeen) < 1.0e-9;

    double anchor = continuing ? samplePasteNextBeat
                              : (playheadBeat >= 0.0 ? playheadBeat : sampleClipFirstBeat);

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - sampleClipFirstBeat) < 1.0e-9)
    {
        anchor += sampleClipLengthBeat;

        if (snapEnabled)
            anchor = snapUp (anchor);
    }

    const double delta = anchor - sampleClipFirstBeat;

    // A pasted block does NOT inherit the group it was cut from. Membership
    // means "these move together", and a copy that lands four bars later is not
    // part of the shape the original group describes. One implementation with
    // the group paste's leftovers, so the two cannot disagree.
    std::vector<int> newIds;
    pasteLooseSamplesInto (*active, delta, newIds);

    if (newIds.empty())
        return false;

    setSampleSelection (newIds);
    stack.notifyNotesChanged();

    // The next paste in this streak starts where this one stopped — computed
    // from the beat actually written, so a clipboard that spans gaps between
    // its blocks steps by its real width rather than by a block count.
    samplePastePlayheadSeen = playheadBeat;
    samplePasteNextBeat     = snapEnabled ? snapUp (anchor + sampleClipLengthBeat)
                                          : anchor + sampleClipLengthBeat;

    flashStatus (utf8 ("已粘贴 ") + juce::String ((int) newIds.size()) + utf8 (" 个采样"));
    return true;
}

//==============================================================================
//  Phase 29: the edit commands, over a possibly mixed selection
//==============================================================================
void PianoRollComponent::copySelectionToClipboards()
{
    // The clipboards are written by ONE gesture, and the ones with nothing
    // selected are EMPTIED. That is the whole point: a paste must not resurrect
    // the clips from a copy three actions ago just because this time only notes
    // happened to be selected.
    //
    // Phase 63: one pass fills all three — objects, loose notes, loose blocks.
    // Running copySamplesToClipboard() afterwards (as this used to) handed the
    // block clipboard a list that still contained the blocks the object
    // clipboard had already claimed: two descriptions of one copy, and a paste
    // that wrote both.
    if (! selection.empty() || ! sampleSelection.empty())
    {
        copyNotesToClipboard();
        return;
    }

    noteClip.clear();
    noteClipFirstBeat = 0.0;
    noteClipEndBeat   = 0.0;

    sampleClip.clear();
    sampleClipFirstBeat     = 0.0;
    sampleClipLengthBeat    = 1.0;
    samplePasteNextBeat     = -1.0;
    samplePastePlayheadSeen = -2.0;

    groupClips.clear();
}

bool PianoRollComponent::pasteEditClipboard()
{
    //-- Phase 56: a copied object pastes as ONE transaction ----------------
    // Not "the notes, then the blocks": two passes mean two anchors and two
    // halves that do not know about each other — the notes landed in a new
    // group and the blocks landed beside it, ungrouped. A group is one thing,
    // so the paste is one transaction.
    //
    // Phase 63: "a group" became "every object on the clipboard", pasted by the
    // same code with one shared anchor. That is what makes "copy two groups,
    // paste" put back two groups instead of one group holding both.
    if (! groupClips.empty())
        return pasteGroupFromClipboard();

    const bool haveNotes = canPasteNotes();
    const bool haveClips = canPasteSamples();

    if (! haveNotes && ! haveClips)
        return false;

    if (! haveNotes)
        return pasteSamplesFromClipboard();

    if (! haveClips)
        return pasteNotesFromClipboard();

    // Each paste leaves exactly what it inserted selected, so the mixed result
    // is those two answers put back together. Doing it this way keeps a single
    // implementation of "where does a note land" and "where does a block land"
    // — the two anchors are allowed to differ, and they do.
    selection.clear();
    pasteNotesFromClipboard();
    const auto pastedNotes = selection;

    pasteSamplesFromClipboard();
    const auto pastedClips = sampleSelection;

    if (pastedNotes.empty() || pastedClips.empty())
        return true;                    // one kind landed; the other is empty

    setMixedSelection (pastedNotes, pastedClips);
    return true;
}

void PianoRollComponent::duplicateEditSelection()
{
    // Capture the clips BEFORE the note pass: selecting the note copies lets go
    // of the clips (see setSelection), and "one gesture, one copy" has to
    // survive that.
    const auto clipIds = sampleSelection;
    const auto newNotes = duplicateSelectedNotes (1.0);

    if (clipIds.empty())
        return;

    sampleSelection = clipIds;                    // put them back for the clip pass
    const auto newClips = duplicateSelectedSamples();

    if (! newClips.empty() && ! newNotes.empty())
        setMixedSelection (newNotes, newClips);
}

int PianoRollComponent::addSampleAt (double beat, int pitch, const juce::File& file)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    auto loaded = loadSampleFile (file);

    if (! loaded.ok())
    {
        if (onSampleLoadFailed != nullptr)
            onSampleLoadFailed (file.getFileName() + utf8 ("：") + loaded.error);

        return -1;
    }

    return addSampleBlock (beat, pitch, loaded.buffer, file.getFullPathName(),
                           file.getFileName(), rack.addSampler (loaded.buffer,
                                                               file.getFullPathName(),
                                                               file.getFileName()));
}

int PianoRollComponent::addSampleFromSlot (double beat, int pitch, int slotId)
{
    const auto* slot = [this, slotId]() -> const SynthSlot*
    {
        for (int i = 0; i < rack.getNumSlots(); ++i)
            if (const auto* candidate = rack.getSlot (i); candidate != nullptr
                  && candidate->id == slotId)
                return candidate;

        return nullptr;
    }();

    if (slot == nullptr || ! slot->isSampler() || slot->sampleBuffer == nullptr)
        return -1;

    return addSampleBlock (beat, pitch, slot->sampleBuffer, slot->samplePath,
                           juce::File (slot->samplePath).getFileName(), slotId);
}

int PianoRollComponent::addSampleBlock (double beat, int pitch,
                                        std::shared_ptr<const SampleBuffer> buffer,
                                        juce::String path, juce::String name,
                                        int outputSlotId)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || buffer == nullptr)
        return -1;

    SampleEvent sample;
    sample.name  = std::move (name);
    sample.path  = std::move (path);
    sample.audio = std::move (buffer);
    sample.pitch = juce::jlimit (0, numPitches - 1, pitch);

    // How tall the block is: four lanes by default. It is a view-and-target
    // choice, not a musical one — the sound plays at its own rate either way.
    sample.laneSpan = juce::jlimit (1, SampleEvent::numPitchLanes, 4);
    sample.pitch    = juce::jlimit (0, numPitches - sample.laneSpan, sample.pitch);

    // The width comes from the file's real duration, converted at the current
    // tempo. Phase 31: EXACTLY that duration — it used to be rounded up to the
    // grid "so the tail is never clipped off", which made a dropped block a
    // little longer than its audio. That extra width was silence, and worse, it
    // was a length "恢复原始长度" could never give back: the menu restores the
    // FILE's length, so every restore landed a fraction of a beat short of what
    // the drop had created. Rounding was the bug; a block is now exactly as long
    // as the audio it holds, and the two commands agree by construction.
    sample.lengthBeats = juce::jmax (gridDivision,
                                     SampleEvent::beatsForSeconds (sample.audio->lengthSeconds(),
                                                                   bpm));

    sample.startBeat = juce::jmax (0.0, snapEnabled ? snapBeat (beat) : beat);

    // Phase 27: dropping a file also grows a sampler for it in the rack, and
    // the clip is mounted on that sampler. Two consequences, both intended:
    // "which file is this" becomes a routing choice you can see in the rack,
    // and a second clip can be pointed at the same sampler instead of decoding
    // the file again. A rack that already holds this exact path reuses its
    // sampler, so dropping one file twice does not leave twins behind.
    //
    // Phase 52: the caller passes the slot when it already has one (a card
    // dragged out of the rack); otherwise the sampler is grown here.
    sample.outputSlotId = outputSlotId;

    const int newId = layer->addSample (std::move (sample));

    ensureTimelineExtendsTo (beat + layer->findSample (newId)->lengthBeats);
    stack.notifyNotesChanged();

    // Phase 33: a block arrives SELECTED.
    //
    // Two things follow, and both were missing. Delete now removes the block the
    // user just dropped — with nothing selected it used to do nothing at all,
    // which reads as "the drop did not really happen". And the host lights the
    // sampler that holds the file (updateRackRoutingFocus follows a clip
    // selection), so the answer to "where did it land" is on screen instead of
    // being a card among cards.
    setSampleSelection ({ newId });

    return newId;
}

//==============================================================================
void PianoRollComponent::drawSampleBlock (juce::Graphics& g, int layerIndex,
                                          const SampleEvent& sample,
                                          juce::Colour colour, float alpha,
                                          bool selected)
{
    auto rect = getSampleScreenBounds (layerIndex, sample.id);

    if (rect.isEmpty() || rect.getWidth() < 1.0f)
        return;

    // Top lane gets a header strip: the name has to sit somewhere, and putting
    // it inside the first lane keeps the block's footprint honest.
    const float headerH = juce::jmin (ui::pf (13), rect.getHeight() * 0.42f);

    // Phase 27: draw what will PLAY, not what the clip was created from. When a
    // block is mounted on another sampler the renderer reads that sampler's
    // buffer, so drawing the clip's own buffer would show one waveform and
    // sound like another — the exact mismatch this phase exists to avoid. Both
    // answers come from routing::resolveSample, so there is one rule.
    const auto* clipLayer = stack.getLayer (layerIndex);
    const auto  audio = clipLayer != nullptr ? clipAudio (*clipLayer, sample) : sample.audio;
    const bool  loaded = audio != nullptr;

    g.setColour (colour.withAlpha ((loaded ? 0.32f : 0.12f) * alpha * (selected ? 1.6f : 1.0f)));
    g.fillRoundedRectangle (rect, ui::pf (3));

    g.setColour (colour.withAlpha (loaded ? 0.85f * alpha : 0.30f * alpha));
    g.drawRoundedRectangle (rect.reduced (0.5f), ui::pf (3), 1.0f);

    //-- Waveform -------------------------------------------------------------
    // Phase 30: the waveform is drawn at a FIXED scale — one screen pixel is
    // always the same slice of the source — and it simply stops where the file
    // stops. Fitting the window to the block's width meant every trim rescaled
    // the picture: make the block shorter and the same audio was redrawn wider.
    // Trimming must CUT a picture, not redraw it. The fixed scale also makes a
    // block that outruns its file (every drop rounds its length up to the grid)
    // show the truth: a wave that ends, then silence.
    if (loaded && ! audio->peaks.empty() && rect.getWidth() > 6.0f && beatWidth > 0.0f)
    {
        const auto wave = rect.withTrimmedTop (headerH).reduced (ui::pf (2), ui::pf (1));

        if (wave.getHeight() > 3.0f)
        {
            const auto& peaks   = audio->peaks;
            const int   buckets = (int) peaks.size();

            const double totalSeconds   = audio->lengthSeconds();
            const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
            const double fixedPerPixel  = secondsPerBeat / (double) beatWidth;

            const double startSec = juce::jlimit (0.0, juce::jmax (0.0, totalSeconds),
                                                  sample.sourceOffsetSeconds);

            // Phase 31: which slice of source one pixel covers depends on what
            // the block DOES with it. A squeezed block maps the whole remaining
            // file onto its own width; a window (or a transposed block, which
            // still reads at the file's speed) keeps the fixed scale and stops
            // at the end of the file. Same decision as sampleWaveColumns(), from
            // the same call, so the two cannot drift.
            const bool   fillsBlock = clipLayer != nullptr && sampleWaveFillsBlock (*clipLayer, sample);
            const double usable     = juce::jmax (0.0, totalSeconds - startSec);
            const double secPerPixel = (fillsBlock && usable > 0.0)
                                         ? usable / juce::jmax (1.0, (double) wave.getWidth())
                                         : fixedPerPixel;

            // Same call the self-test asks, so the picture and the assertion
            // cannot disagree about how much of the file this block shows.
            const int columns = clipLayer != nullptr
                                  ? sampleWaveColumns (*clipLayer, sample, wave.getWidth())
                                  : 0;

            if (columns > 0 && totalSeconds > 1.0e-6 && secPerPixel > 0.0)
            {
                juce::Path upper, lower;
                const float mid = wave.getCentreY();

                for (int i = 0; i < columns; ++i)
                {
                    // Phase 50: a reversed block draws its columns from the far
                    // end backwards — the picture flips with the sound, which is
                    // what makes "did I reverse the right block" answerable at a
                    // glance instead of by ear.
                    const int column = sample.reversed ? (columns - 1 - i) : i;

                    const double t0 = startSec + (double) column * secPerPixel;
                    const double t1 = t0 + secPerPixel;

                    int b0 = (int) (t0 / totalSeconds * (double) buckets);
                    int b1 = (int) (t1 / totalSeconds * (double) buckets);

                    b0 = juce::jlimit (0, juce::jmax (0, buckets - 1), b0);
                    b1 = juce::jlimit (b0 + 1, buckets, b1);

                    // Peak over the span this column covers, so zooming out keeps
                    // the envelope instead of sampling one bucket out of a
                    // hundred and drawing a different shape at every zoom.
                    float peak = 0.0f;

                    for (int b = b0; b < b1; ++b)
                        peak = juce::jmax (peak, peaks[(size_t) b]);

                    const float level = peak * wave.getHeight() * 0.5f;
                    const float x     = wave.getX() + (float) i;

                    if (i == 0)
                    {
                        upper.startNewSubPath (x, mid - level);
                        lower.startNewSubPath (x, mid + level);
                    }
                    else
                    {
                        upper.lineTo (x, mid - level);
                        lower.lineTo (x, mid + level);
                    }
                }

                g.setColour (colour.brighter (0.75f).withAlpha (0.90f * alpha));
                g.strokePath (upper, juce::PathStrokeType (1.0f));
                g.strokePath (lower, juce::PathStrokeType (1.0f));
            }
        }
    }

    //-- Name strip -----------------------------------------------------------
    if (rect.getWidth() > ui::pf (24))
    {
        g.setColour (colour.withAlpha (0.55f * alpha));
        g.fillRect (rect.withHeight (headerH));

        // Phase 27: the block says which sampler plays it — but only when that
        // is news. A clip dropped from a file is mounted on a sampler that was
        // grown FOR it, so the two names are identical and the arrow would just
        // repeat itself; the label earns its space the moment the clip is
        // pointed at a DIFFERENT sampler, which is exactly when the sound
        // changes without the block moving.
        juce::String label = sample.name;

        if (sample.outputSlotId >= 0)
            if (const auto* slot = rack.findSlot (sample.outputSlotId);
                slot != nullptr && slot->name != sample.name)
                label += juce::String ("  ") + utf8 ("\xe2\x86\x92") + " " + slot->name;

        //-- Phase 31: the stretch badge --------------------------------------
        // A mode that changes the sound but says nothing on screen is a setting
        // nobody can audit. The badge names the mode in the user's words and
        // prints the number it is actually running at, so "why does this block
        // sound fast" has an answer on the block itself.
        juce::String tag;
        float        tagWidth = 0.0f;

        if ((sample.stretchMode != StretchMode::none || sample.reversed)
              && rect.getWidth() > ui::pf (104))
        {
            const auto playback = clipLayer != nullptr
                                    ? samplePlayback (*clipLayer, sample)
                                    : clipstretch::Playback();

            switch (sample.stretchMode)
            {
                case StretchMode::resample:
                    tag = utf8 ("变速变调 ×") + juce::String (playback.speed, 2);
                    break;

                case StretchMode::tempo:
                    tag = utf8 ("变速 ×") + juce::String (playback.speed, 2);
                    break;

                case StretchMode::pitch:
                {
                    const double semi = clipstretch::semitones (playback.pitch);
                    tag = utf8 ("变调 ") + (semi >= 0.0 ? "+" : "")
                            + juce::String (semi, 1);
                    break;
                }

                case StretchMode::none:
                default:
                    break;
            }

            // Phase 50: a reversed block must SAY so. The waveform flipping is
            // the primary cue, but a short file's picture is a few pixels and
            // reads as noise — the pill is the answer that survives a zoom.
            if (sample.reversed)
                tag = tag.isEmpty() ? utf8 ("反向") : utf8 ("反向 · ") + tag;

            if (tag.isNotEmpty())
            {
                const juce::Font tagFont (juce::FontOptions (atomic::ui::sp (8.5f)));

                // Measured with GlyphArrangement: getStringWidthFloat is
                // deprecated in JUCE 8 and cannot account for the fallback font
                // that draws the CJK in that tag.
                juce::GlyphArrangement glyphs;
                glyphs.addLineOfText (tagFont, tag, 0.0f, 0.0f);
                tagWidth = glyphs.getBoundingBox (0, -1, true).getWidth() + ui::pf (8);

                if (headerH > ui::pf (8) && tagWidth < rect.getWidth() * 0.82f)
                {
                    const juce::Rectangle<float> pill (rect.getRight() - tagWidth - ui::pf (2),
                                                       rect.getY() + ui::pf (1.5f),
                                                       tagWidth,
                                                       headerH - ui::pf (3));

                    g.setColour (groupPalette::group1.withAlpha (0.86f * alpha));
                    g.fillRoundedRectangle (pill, ui::pf (2.5f));

                    g.setColour (juce::Colours::black.withAlpha (0.82f * alpha));
                    g.setFont (tagFont);
                    g.drawText (tag, pill, juce::Justification::centred, false);
                }
                else
                {
                    tagWidth = 0.0f;   // it did not fit: the name gets its space back
                }
            }
        }

        g.setColour (juce::Colours::white.withAlpha ((loaded ? 0.92f : 0.45f) * alpha));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (label,
                    rect.withHeight (headerH).withTrimmedLeft (ui::pf (4))
                        .withTrimmedRight (ui::pf (3) + tagWidth),
                    juce::Justification::centredLeft, true);
    }

    //-- Phase 49: the chevron that opens the block's curve box ---------------
    // Only the active layer's blocks answer the mouse, so only they advertise
    // the handle — a ghost block with a button would promise an edit the
    // active-layer rule would refuse. Same glyph as the note's tail button and
    // the group's pill handle: "the thing at the right end opens a curve" is
    // learned once across all three.
    if (layerIndex == stack.getActiveLayerIndex())
    {
        const auto handle = getSampleAutomationHandleBounds (sample);
        const bool open   = (expandedSampleId == sample.id
                               && expandedSampleLayerIndex == layerIndex);
        const bool hot    = (hoveredSampleHandleId == sample.id && hoveredSampleHandle);

        if (! handle.isEmpty())
        {
            g.setColour (open ? colour.brighter (0.8f).withAlpha (alpha * 0.55f)
                              : juce::Colours::black.withAlpha (alpha * 0.30f));
            g.fillRoundedRectangle (handle, ui::pf (2.5f));

            juce::Path chevron;
            const float cx = handle.getCentreX();
            const float cy = handle.getCentreY() + (open ? ui::pf (1.2f) : 0.0f);

            chevron.startNewSubPath (cx - ui::pf (2.8f), cy - ui::pf (1.4f));
            chevron.lineTo (cx, cy + ui::pf (1.8f));
            chevron.lineTo (cx + ui::pf (2.8f), cy - ui::pf (1.4f));

            g.setColour (juce::Colours::white.withAlpha (open || hot ? 0.95f : 0.7f * alpha));
            g.strokePath (chevron, juce::PathStrokeType (ui::pf (1.2f),
                                                         juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
        }
    }

    //-- Phase 28: the block can be the selection -----------------------------
    // Same three signals a selected note uses (brighter body above, a white
    // ring, four corner brackets outside): the two kinds of object live on the
    // same canvas, and "selected" has to look like one thing.
    if (selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.96f));
        g.drawRoundedRectangle (rect.reduced (0.5f), ui::pf (3), ui::pf (1.6f));

        if (rect.getWidth() >= ui::pf (12) && rect.getHeight() >= ui::pf (10))
        {
            const float arm = ui::pf (5);
            const auto  box = rect.expanded (ui::pf (1.5f));
            juce::Path brackets;

            brackets.startNewSubPath (box.getX(), box.getY() + arm);
            brackets.lineTo (box.getX(), box.getY());
            brackets.lineTo (box.getX() + arm, box.getY());

            brackets.startNewSubPath (box.getRight() - arm, box.getY());
            brackets.lineTo (box.getRight(), box.getY());
            brackets.lineTo (box.getRight(), box.getY() + arm);

            brackets.startNewSubPath (box.getRight(), box.getBottom() - arm);
            brackets.lineTo (box.getRight(), box.getBottom());
            brackets.lineTo (box.getRight() - arm, box.getBottom());

            brackets.startNewSubPath (box.getX() + arm, box.getBottom());
            brackets.lineTo (box.getX(), box.getBottom());
            brackets.lineTo (box.getX(), box.getY() + box.getHeight() - arm);

            g.setColour (juce::Colours::white.withAlpha (0.82f));
            g.strokePath (brackets, juce::PathStrokeType (ui::pf (1.4f), juce::PathStrokeType::mitered,
                                                          juce::PathStrokeType::butt));
        }
    }
}

void PianoRollComponent::drawDropPreview (juce::Graphics& g)
{
    if (! sampleDropActive)
        return;

    const auto grid = getGridArea();

    if (! grid.contains (sampleDropPoint))
        return;

    const auto colour = theme::accent;

    // What the card carries, in its own words. A preview that says "replace
    // with THIS" can be checked at a glance; "replace with something" cannot.
    const auto* card = dropSlotId >= 0 ? rack.findSlot (dropSlotId) : nullptr;

    const auto cardTitle = card == nullptr
                             ? juce::String()
                             : (card->isSampler()
                                  ? juce::File (card->samplePath).getFileName()
                                  : card->getDisplayName());

    const auto labelFont = juce::Font (juce::FontOptions (ui::sp (9.0f)));

    // A tag that follows the pointer: the target may be 12 px wide (a note) and
    // a label drawn inside it would be a smear. Clamped to the grid so it
    // cannot be lost off the edge it was dragged towards.
    const auto drawTag = [&] (const juce::String& text, const juce::Colour& tagColour)
    {
        const float w = juce::GlyphArrangement::getStringWidth (labelFont, text) + ui::px (12);
        const float h = ui::sp (17);

        juce::Rectangle<float> box (sampleDropPoint.x + ui::px (12),
                                    sampleDropPoint.y - h - ui::px (8), w, h);

        box.setPosition (juce::jlimit (grid.getX(), juce::jmax (grid.getX(), grid.getRight() - w),
                                       box.getX()),
                         juce::jlimit (grid.getY(), juce::jmax (grid.getY(), grid.getBottom() - h),
                                       box.getY()));

        g.setColour (theme::surface3.withAlpha (0.96f));
        g.fillRoundedRectangle (box, ui::pf (3));

        g.setColour (tagColour.withAlpha (0.9f));
        g.drawRoundedRectangle (box.reduced (0.5f), ui::pf (3), 1.0f);

        g.setColour (theme::ink);
        g.setFont (labelFont);
        g.drawText (text, box.reduced (ui::pf (6.0)), juce::Justification::centredLeft, true);
    };

    //-- 0. a group row: the pointer's beat is the whole answer --------------
    // Phase 58. There is no target to hit, so the preview draws what the drop
    // will be: a line where the group's first member lands, and a ghost of its
    // span, so "one copy of this group, starting here" is visible before the
    // button comes up.
    if (dropKind == CardDropKind::groupRow)
    {
        const auto* layer = stack.getLayer (dropGroupLayer);
        const auto* group = layer != nullptr ? layer->findGroup (dropGroupId) : nullptr;

        if (group == nullptr)
            return;     // deleted mid-drag: do not promise anything

        const auto occurrences = layer->getGroupOccurrences (dropGroupId);

        double first = std::numeric_limits<double>::max();
        double last  = 0.0;

        if (! occurrences.empty())
            for (const auto& note : layer->getNotes())
                if (note.groupId == dropGroupId && note.groupOccurrence == occurrences.front())
                {
                    first = juce::jmin (first, note.startBeat);
                    last  = juce::jmax (last, note.endBeat());
                }

        if (first == std::numeric_limits<double>::max())
            return;

        const double atBeat = juce::jmax (0.0, beatAtScreenX (sampleDropPoint.x));
        const double delta  = atBeat - first;

        const float x0 = screenXForBeat (atBeat);
        const float x1 = screenXForBeat (last + delta);

        juce::Rectangle<float> ghost (x0, grid.getY() + ui::pf (2),
                                      juce::jmax (ui::pf (2), x1 - x0),
                                      grid.getHeight() - ui::pf (4));

        g.setColour (colour.withAlpha (0.10f));
        g.fillRoundedRectangle (ghost, ui::pf (3));

        g.setColour (colour.withAlpha (0.45f));
        juce::Path outline;
        outline.addRoundedRectangle (ghost.reduced (0.5f), ui::pf (3));

        const float dashes[] = { ui::pf (5), ui::pf (4) };
        juce::Path dashed;
        juce::PathStrokeType (ui::pf (1.4f)).createDashedStroke (dashed, outline, dashes, 2);
        g.fillPath (dashed);

        // The beat line itself, so the anchor is unambiguous even when the
        // pointer is inside the ghost.
        g.setColour (colour.withAlpha (0.95f));
        g.fillRect (juce::Rectangle<float> (x0 - ui::pf (0.7f), ghost.getY(),
                                            ui::pf (1.4f), ghost.getHeight()));

        drawTag (utf8 ("放一处 ") + group->getName(), colour);
        return;
    }

    //-- 1. a synth card: only a note is a target ---------------------------
    // Phase 53. Nothing else can be said with a mount — dropping it on empty
    // grid would have to invent a note, which is not what the gesture means.
    if (dropKind == CardDropKind::instrumentCard)
    {
        if (dropTargetNote < 0)
        {
            drawTag (utf8 ("拖到音符上以挂载"), theme::inkSubtle);
            return;
        }

        const auto bounds = getNoteScreenBounds (stack.getActiveLayerIndex(), dropTargetNote);

        if (bounds.isEmpty())
            return;

        g.setColour (colour.withAlpha (0.22f));
        g.fillRoundedRectangle (bounds, ui::pf (2));

        g.setColour (colour.withAlpha (0.95f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (2), ui::pf (1.6f));

        drawTag (utf8 ("挂到 ") + cardTitle, colour);
        return;
    }

    //-- 2. a sampler card over a block: that block is the target -----------
    if (dropKind == CardDropKind::sampleCard && dropTargetSample >= 0)
    {
        const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), dropTargetSample);

        if (bounds.isEmpty())
            return;

        g.setColour (colour.withAlpha (0.20f));
        g.fillRoundedRectangle (bounds, ui::pf (3));

        juce::Path outline, dashed;
        outline.addRoundedRectangle (bounds.reduced (0.5f), ui::pf (3));

        const float dashes[] = { ui::pf (5), ui::pf (4) };
        juce::PathStrokeType (ui::pf (1.8f)).createDashedStroke (dashed, outline, dashes, 2);

        g.setColour (colour.withAlpha (0.95f));
        g.fillPath (dashed);

        drawTag (utf8 ("替换为 ") + cardTitle, colour);
        return;
    }

    //-- 3. a file (or a card) on empty grid: the block it would create -----
    // A preview has to answer "where and how big" before the drop, so it is
    // built from the same numbers addSampleAt() will use: bottom lane + span
    // for height, the snapped duration for width. Anything else would jump.
    const int span  = 4;
    const int pitch = juce::jlimit (0, numPitches - span,
                                    pitchAtScreenY (sampleDropPoint.y));
    const double beat = juce::jmax (0.0, snapEnabled ? snapBeat (beatAtScreenX (sampleDropPoint.x))
                                                     : beatAtScreenX (sampleDropPoint.x));

    // The real duration is unknown until the file is decoded; the default
    // four-lane / one-bar footprint is the honest stand-in.
    const double length = juce::jmax (gridDivision, 4.0 * gridDivision);

    const float x0 = screenXForBeat (beat);
    const float x1 = screenXForBeat (beat + length);
    const float top    = screenYForPitch (pitch + span - 1);
    const float bottom = screenYForPitch (pitch) + keyHeight;

    juce::Rectangle<float> rect (x0, top, juce::jmax (2.0f, x1 - x0),
                                 juce::jmax (keyHeight, bottom - top));

    g.setColour (colour.withAlpha (0.16f));
    g.fillRoundedRectangle (rect, ui::pf (3));

    juce::Path outline, dashed;
    outline.addRoundedRectangle (rect.reduced (0.5f), ui::pf (3));

    const float dashes[] = { ui::pf (5), ui::pf (4) };
    juce::PathStrokeType (ui::pf (1.4f)).createDashedStroke (dashed, outline, dashes, 2);

    g.setColour (colour.withAlpha (0.85f));
    g.fillPath (dashed);

    g.setColour (colour.withAlpha (0.75f));
    g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
    g.drawText (utf8 ("松开以放置采样"),
                rect.withTrimmedLeft (ui::pf (4)), juce::Justification::centredLeft, true);
}

void PianoRollComponent::showSampleMenu (int sampleId, juce::Point<int> screenPos)
{
    const int layerIndex = stack.getActiveLayerIndex();
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("采样块"));

    // Phase 30: "put it back". A trim is a drag on a 6 px band, and nothing in
    // the UI said how to undo it — the only way back was to drag the other edge
    // first. This is the one command that restores the window onto the whole
    // file, so a trim is always reversible without reaching for Ctrl+Z.
    // Phase 31: enabled when ANY block the command would touch is off its full
    // length — the selection if the click was inside it, otherwise just this
    // one. Asking only about the clicked block left a greyed-out "put them back"
    // above five blocks that clearly needed it.
    const bool canRestore = [&]
    {
        const auto needsRestore = [this, layer] (const SampleEvent& clip)
        {
            const auto buffer = clipAudio (*layer, clip);

            return buffer != nullptr
                     && (clip.sourceOffsetSeconds > 1.0e-9
                           || std::abs (clip.lengthBeats
                                          - SampleEvent::beatsForSeconds (buffer->lengthSeconds(), bpm))
                                > 1.0e-6);
        };

        if (isSampleSelected (sample->id))
        {
            for (const auto& clip : layer->getSamples())
                if (isSampleSelected (clip.id) && needsRestore (clip))
                    return true;
        }

        return needsRestore (*sample);
    }();
    menu.addItem (1, utf8 ("复制到后一小节"));
    menu.addItem (2, utf8 ("对齐到网格"), true, false);
    menu.addItem (7, utf8 ("恢复原始长度"), canRestore, false);

    // Phase 51: the cut moved to the toolbar's knife — the menu keeps the verbs
    // that describe the block (reverse, trim, stretch, mount), and "cut here"
    // is a tool, not a property of the block you happened to click.
    menu.addItem (kClipMenuReverse, sample->reversed ? utf8 ("取消反转") : utf8 ("反转音频"),
                  true, sample->reversed);
    menu.addItem (3, utf8 ("加高一行"));
    menu.addItem (4, utf8 ("降低一行"));
    menu.addSeparator();

    //-- Phase 31: what a length change does to the sound ---------------------
    // Before this, dragging an edge always meant "show more or less of the
    // file". That is one of four useful answers, and the other three are what a
    // sampler is for. The current one is ticked, and each label says what will
    // actually change (speed, pitch or both) instead of naming a technique.
    {
        juce::PopupMenu stretch;
        const auto current = sample->stretchMode;

        // Phase 51: 不变速只变调 is no longer offered here. It is a SOURCE-ish
        // decision ("this sample sits a few semitones up") rather than a
        // what-happens-when-I-drag decision, so it lives on the sampler's rack
        // card now — together with 变速变调, which the card also carries. The
        // mode itself still exists and old files with pitch blocks still play
        // (the badge still draws); only the menu entry is gone.
        stretch.addItem (kClipStretchBase + (int) StretchMode::none,
                         utf8 ("原速 · 只截取窗口"), true, current == StretchMode::none);
        stretch.addSeparator();
        stretch.addItem (kClipStretchBase + (int) StretchMode::resample,
                         utf8 ("变速变调 · 像磁带"), true, current == StretchMode::resample);
        stretch.addItem (kClipStretchBase + (int) StretchMode::tempo,
                         utf8 ("只变速不变调 · 时间伸缩"), true, current == StretchMode::tempo);

        menu.addSubMenu (utf8 ("拉伸方式"), stretch);
        menu.addSeparator();
    }

    //-- Phase 27: which sampler plays this block ----------------------------
    // This is the whole point of samplers: the block names a slot, the slot
    // holds a file, so re-pointing the block changes WHAT YOU HEAR without
    // moving the rectangle. The clip's own decoded buffer stays as the fallback
    // for "follow the layer", which is what every pre-Phase-27 project does.
    {
        juce::PopupMenu mount;
        mount.addItem (kClipMenuUnmount, utf8 ("跟随图层输出"), true, sample->outputSlotId < 0);

        const auto samplerIds = rack.getSamplerSlotIds();

        for (const int slotId : samplerIds)
            mount.addItem (kClipMountBase + slotId, rack.getSlotName (slotId), true,
                           sample->outputSlotId == slotId);

        if (samplerIds.empty())
            mount.addItem (kClipMenuNotices, utf8 ("（机架上还没有采样音源）"), false, false);

        menu.addSubMenu (utf8 ("挂载采样音源"), mount);
    }

    //-- Phase 27: grouping ---------------------------------------------------
    // A clip can join a group of notes. Membership is about moving together;
    // it is NOT about routing — see routing::resolveSample, which never looks
    // at a group. That split is what lets a group be re-mounted without
    // silently swapping the audio file underneath a clip.
    {
        const int currentGroup = sample->groupId;
        juce::PopupMenu grouping;

        for (const auto& group : layer->getGroups())
        {
            const int members = layer->countNotesInGroup (group.getId())
                                  + layer->countSamplesInGroup (group.getId());

            grouping.addItem (kClipGroupBase + group.getId(),
                              group.getName() + utf8 (" · ") + juce::String (members),
                              true, currentGroup == group.getId());
        }

        if (layer->getGroups().empty())
            grouping.addItem (kClipMenuNotices, utf8 ("（当前图层还没有编组）"), false, false);

        grouping.addSeparator();

        // Phase 35: the menu that lists groups can also MAKE one. Until now a
        // block could only join a group that already existed — so "group these
        // two samples" required knowing that G existed, on a keyboard, with no
        // group in sight to aim at.
        grouping.addItem (kClipMenuNewGroup, utf8 ("新建编组（含这个采样）"));
        grouping.addItem (kClipMenuUngroup, utf8 ("移出编组"), currentGroup >= 0);

        menu.addSubMenu (utf8 ("编组"), grouping);
    }

    menu.addSeparator();
    menu.addItem (5, utf8 ("删除"));

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, layerIndex, sampleId] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        auto* owner = safe->stack.getLayer (layerIndex);

        if (owner == nullptr)
            return;

        const auto* source = owner->findSample (sampleId);

        if (source == nullptr)
            return;

        if (result == 5)
        {
            // Phase 28: the menu acts on the SELECTION, not just on the block
            // that was right-clicked — the click already made that block the
            // selection, so "delete" on six selected copies deletes six.
            safe->deleteSelectedSamples();
            return;
        }

        if (result == 1)
        {
            safe->duplicateSelectedSamples();
            return;
        }

        // Phase 50: both commands act on the whole selection, like delete and
        // restore already do — the right-click made the block part of it.
        if (result == kClipMenuReverse)
        {
            safe->toggleSelectedSampleReverse();
            return;
        }

        if (auto* target = owner->findSample (sampleId))
        {
            if (result == 2)
                target->startBeat = safe->snapEnabled ? safe->snapBeat (target->startBeat)
                                                      : target->startBeat;
            else if (result == 3)
            {
                target->laneSpan = juce::jlimit (1, SampleEvent::numPitchLanes, target->laneSpan + 1);
                safe->syncShapeOfClip (sampleId);      // Phase 62: height is shape too
            }
            else if (result == 4)
            {
                target->laneSpan = juce::jlimit (1, SampleEvent::numPitchLanes, target->laneSpan - 1);
                safe->syncShapeOfClip (sampleId);
            }
            else if (result == 7)
            {
                // Phase 31: the whole selection, not the one block that was
                // right-clicked. The click already made this the selection, and
                // "put them back" is the command a user needs after trimming
                // six blocks by eye.
                safe->restoreSelectedSampleLengths();

                safe->stack.notifyNotesChanged();
                safe->repaint();
                return;
            }
            else if (result >= kClipStretchBase && result < kClipStretchBase + 8)
            {
                // Phase 31: one mode, applied to everything that was selected —
                // but only if the clicked block is part of that selection.
                // Right-clicking a block outside a multi-selection means "this
                // one", and that is what it must do.
                const auto mode = (StretchMode) (result - kClipStretchBase);

                if (safe->isSampleSelected (sampleId))
                    safe->setSelectedSampleStretchMode (mode);
                else
                    safe->setSampleStretchMode (sampleId, mode);

                safe->stack.notifyNotesChanged();
                safe->repaint();
                return;
            }
            else if (result == kClipMenuUnmount || result >= kClipMountBase)
            {
                // Phase 32: "mount" is no longer a bus swap. The chosen sampler
                // holds a file, so the block takes that file — name, length and
                // offset included — and the selection moves as one. Done here
                // rather than by writing outputSlotId so the rule has one home.
                safe->mountSelectedSamplesToSlot (result == kClipMenuUnmount
                                                    ? -1
                                                    : result - kClipMountBase);
                safe->repaint();
                return;
            }
            else if (result == kClipMenuNewGroup)
            {
                // Phase 35: one command, two halves. The block under the cursor
                // becomes the selection first (the right-click already did that
                // when it was outside one), so "new group" means "these blocks",
                // and the shared path — groupSelection — is what builds it. That
                // is deliberate: G and this menu item must produce the same
                // group, or one of them will drift.
                if (! safe->isSampleSelected (sampleId))
                    safe->setSampleSelection ({ sampleId });

                safe->groupSelection();
                return;
            }
            else if (result >= kClipGroupBase)
                target->groupId = result - kClipGroupBase;
            else if (result == kClipMenuUngroup)
                target->groupId = -1;

            safe->stack.notifyNotesChanged();
            safe->repaint();
        }
    });
}

//==============================================================================
//  DragAndDropTarget — the file browser hands us absolute .wav paths, the rack
//  hands us cards
//==============================================================================
bool PianoRollComponent::isInterestedInDragSource (const SourceDetails& details)
{
    const auto text = details.description.toString();

    // Phase 58: a row dragged out of the group list. Accepted over the whole
    // component, not just the grid, so the drag does not flicker "no" while it
    // crosses the ruler on the way down.
    {
        int groupLayer = -1, groupId = -1;

        if (groupDragTarget (text, groupLayer, groupId))
            return true;
    }

    // Phase 52 / 53: a card dragged out of the rack — the file is already
    // decoded and already has a slot, so this drop is "that source, here".
    // An instrument card has no file at all; it carries a mount.
    if (samplerDragSlotId (text) > 0 || instrumentDragSlotId (text) > 0)
        return true;

    const juce::File file (text);

    return file.hasFileExtension ("wav;wave");
}

void PianoRollComponent::updateCardDrop (juce::Point<float> point,
                                        const juce::String& description)
{
    sampleDropActive = true;
    sampleDropPoint  = point;

    dropSlotId       = -1;
    dropTargetNote   = -1;
    dropTargetSample = -1;
    dropGroupLayer   = -1;
    dropGroupId      = -1;

    // Phase 58: a group row. There is no target to hit — the drop point IS the
    // answer ("put that group's first beat here"), so the preview needs the two
    // numbers and nothing else.
    if (int groupLayer = -1, groupId = -1; groupDragTarget (description, groupLayer, groupId))
    {
        dropKind       = CardDropKind::groupRow;
        dropGroupLayer = groupLayer;
        dropGroupId    = groupId;
        return;
    }

    if (const int slotId = samplerDragSlotId (description); slotId > 0)
    {
        dropKind   = CardDropKind::sampleCard;
        dropSlotId = slotId;

        // A card over a block replaces that block's sample; over empty grid it
        // makes a new one (Phase 52). The hit test is the ACTIVE layer's, which
        // is where both the new block and the replacement would land.
        dropTargetSample = getEditableSampleIdAt (point);
        return;
    }

    if (const int slotId = instrumentDragSlotId (description); slotId > 0)
    {
        dropKind     = CardDropKind::instrumentCard;
        dropSlotId   = slotId;
        dropTargetNote = getEditableNoteIdAt (point);
        return;
    }

    dropKind = CardDropKind::file;
}

void PianoRollComponent::clearCardDrop()
{
    sampleDropActive = false;
    dropKind         = CardDropKind::none;
    dropSlotId       = -1;
    dropTargetNote   = -1;
    dropTargetSample = -1;
    dropGroupLayer   = -1;
    dropGroupId      = -1;
}

void PianoRollComponent::itemDragEnter (const SourceDetails& details)
{
    updateCardDrop (details.localPosition.toFloat(), details.description.toString());
    repaint();
}

void PianoRollComponent::itemDragMove (const SourceDetails& details)
{
    updateCardDrop (details.localPosition.toFloat(), details.description.toString());
    repaint();
}

void PianoRollComponent::itemDragExit (const SourceDetails& details)
{
    juce::ignoreUnused (details);

    clearCardDrop();
    repaint();
}

void PianoRollComponent::itemDropped (const SourceDetails& details)
{
    const auto point = details.localPosition.toFloat();
    const auto text  = details.description.toString();

    // Re-run the drag state at the DROP point rather than trusting the last
    // move: updateCardDrop is the one place the hit test happens, so the
    // preview and the drop cannot pick different targets for the same pointer.
    updateCardDrop (point, text);

    const int targetNote   = dropTargetNote;
    const int targetSample = dropTargetSample;
    const int groupLayer   = dropGroupLayer;
    const int groupId      = dropGroupId;

    clearCardDrop();

    if (! getGridArea().contains (point))
    {
        repaint();
        return;
    }

    // Phase 58: a group row from the group list. Anywhere on the grid is a
    // target — the pointer's beat is where the group's first member goes, the
    // pitch lane it is over does not matter (the members bring their own).
    if (groupLayer >= 0 && groupId >= 0)
    {
        placeGroupAppearance (groupLayer, groupId, beatAtScreenX (point.x));
        repaint();
        return;
    }

    // Phase 53: an instrument card. The only thing it can mean is "this note
    // plays on that synth"; anywhere else the gesture has no target and doing
    // nothing is the honest answer (the preview says so while you hover).
    if (const int slotId = instrumentDragSlotId (text); slotId > 0)
    {
        if (targetNote >= 0)
            routeNoteToSlot (targetNote, slotId);

        repaint();
        return;
    }

    // Phase 52 / 53: a sampler card. Over a block it REPLACES that block's
    // sample; over empty grid it is still "that source, here" and makes a new
    // block — the same anchor rules as a file drop, because to the user it is
    // the same gesture; only the bookkeeping differs.
    if (const int slotId = samplerDragSlotId (text); slotId > 0)
    {
        if (targetSample >= 0)
            replaceSamplesWithSlot (targetSample, slotId);
        else
            addSampleFromSlot (beatAtScreenX (point.x), pitchAtScreenY (point.y), slotId);

        repaint();
        return;
    }

    const juce::File file (text);

    if (! file.existsAsFile())
    {
        if (onSampleLoadFailed != nullptr)
            onSampleLoadFailed (file.getFileName() + utf8 ("：") + utf8 ("文件不存在"));

        repaint();
        return;
    }

    // Beat comes from where the pointer is, pitch from the lane under it. Both
    // are read here rather than inside addSampleAt so the preview and the drop
    // cannot disagree about the anchor.
    addSampleAt (beatAtScreenX (point.x), pitchAtScreenY (point.y), file);
    repaint();
}

void PianoRollComponent::simulateCardDrop (const juce::String& description, juce::Point<float> at)
{
    const juce::DragAndDropTarget::SourceDetails details (description, nullptr, at.toInt());

    itemDropped (details);
}

void PianoRollComponent::simulateDropPreview (const juce::String& description, juce::Point<float> at)
{
    if (description.isEmpty())
        clearCardDrop();
    else
        updateCardDrop (at, description);

    repaint();
}

void PianoRollComponent::drawSamples (juce::Graphics& g)
{
    const int activeIndex = stack.getActiveLayerIndex();

    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        auto* layer = stack.getLayer (i);

        if (! isLayerShown (i) || layer->getSamples().empty())
            continue;

        const bool  onActive = (i == activeIndex);
        const float alpha    = onActive ? 1.0f : 0.55f;

        for (const auto& sample : layer->getSamples())
        {
            const auto bounds = getSampleScreenBounds (i, sample.id);

            if (! bounds.intersects (getGridArea()))
                continue;

            // Phase 28: only the active layer can hold a selection, so a ghost
            // block can never light up as "selected" while the click that would
            // move it lands somewhere else.
            drawSampleBlock (g, i, sample, layer->getColour(), alpha,
                             onActive && isSampleSelected (sample.id));
        }
    }
}

} // namespace atomic

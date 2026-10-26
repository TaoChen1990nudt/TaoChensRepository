#include "PianoRollComponent.h"

#include "Util/Metrics.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"

#include <limits>

namespace atomic
{

namespace
{
    const juce::Colour kRollBackground  { 0xff101114 };
    const juce::Colour kGridBackground  { 0xff17181f };
    const juce::Colour kBlackRowColour  { 0xff0e0f14 };
    const juce::Colour kRulerBackground { 0xff1a1b21 };
    const juce::Colour kKeyWhite        { 0xffd7dae2 };
    const juce::Colour kKeyBlack        { 0xff26282f };
    const juce::Colour kOutline         { 0xff2c2e37 };
    const juce::Colour kText            { 0xff9aa0ad };
    const juce::Colour kPlayheadColour  { 0xffff5f6d };

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

    //-- Phase 29: the two fade handles on a clip -----------------------------
    /** How far down from the block's top edge a fade handle answers. The strip
        stops at half the block's height, so a 1-lane block still keeps a strip
        of body between its fade handle and its trim edge. */
    constexpr float kFadeStripHeight = 13.0f;

    /** Half-width of the grab zone around a fade handle. Design pixels: 5 each
        side of a 4.5 px mark, i.e. a target that is reachable without aiming. */
    constexpr float kFadeGrabRadius = 5.0f;

    /** The mark itself, and the smallest block that has room to draw it. */
    constexpr float kFadeHandleSize = 5.0f;

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

    // Follow the playhead once it approaches the right edge. Done before the
    // dirty-region maths below, because a scroll moves everything and no local
    // repaint would be correct.
    if (transportActive && beats >= 0.0 && ! juce::approximatelyEqual (previous, beats))
    {
        const double visible = getVisibleBeats();
        const double lead    = scrollBeats + visible * 0.75;

        if (visible > 1.0 && (beats < scrollBeats || beats > lead))
        {
            ensureTimelineExtendsTo (beats + visible);
            const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
            scrollBeats = juce::jlimit (0.0, maxBeats, beats - visible * 0.15);

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
            g.setColour (juce::Colour (0xff1b1c20));
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

        g.setColour (juce::Colour (0xff6b7280));
        g.fillPath (lead);
        g.setColour (juce::Colour (0xff1b1c20));
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

        g.setColour (juce::Colour (0xff1b1c20));
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

    // Phase 11: no wall at 64 beats. Scrolling past the end adds another chunk
    // (bounded per call), so the view can keep going as long as the user keeps
    // pushing, while a single bad argument cannot make the ruler five hours long.
    if (beats + visible > timelineBeats)
        ensureTimelineExtendsTo (beats + visible);

    const double maxBeats = juce::jmax (0.0, timelineBeats - visible);
    const double clamped = juce::jlimit (0.0, maxBeats, beats);

    if (juce::approximatelyEqual (clamped, scrollBeats))
        return;

    scrollBeats = clamped;
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

juce::Rectangle<float> PianoRollComponent::getNoteBounds (const NoteEvent& note) const
{
    const float x = screenXForBeat (note.startBeat);
    const float y = screenYForPitch (note.pitch);
    const float w = juce::jmax (5.0f, (float) (note.lengthBeats * beatWidth) - 1.5f);

    return { x, y + 1.0f, w, keyHeight - 2.0f };
}

//==============================================================================
double PianoRollComponent::snapBeat (double beats) noexcept
{
    return std::round (beats / gridDivision) * gridDivision;
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

    if (scrollBeats + visibleBeats > timelineBeats)
        ensureTimelineExtendsTo (scrollBeats + visibleBeats);

    const double maxBeats = juce::jmax (0.0, timelineBeats - visibleBeats);
    scrollBeats = juce::jlimit (0.0, maxBeats, scrollBeats);
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

bool PianoRollComponent::isWholeGroupSelected (int groupId) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || groupId < 0 || selection.empty())
        return false;

    // Every member has to be in the selection, and nothing else may be — a
    // selection that also holds a loose note is two actions, not one.
    const int members = layer->countNotesInGroup (groupId);

    if (members <= 0 || (int) selection.size() != members)
        return false;

    for (int id : selection)
    {
        const auto* note = layer->findNote (id);

        if (note == nullptr || note->groupId != groupId)
            return false;
    }

    return true;
}

void PianoRollComponent::selectGroup (int groupId)
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
        setMixedSelection (layer->getGroupNoteIds (groupId),
                           layer->getGroupSampleIds (groupId));
        return;
    }
}

bool PianoRollComponent::ungroupSelection()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || (selection.empty() && sampleSelection.empty()))
        return false;

    bool changed = false;

    for (int id : selection)
        if (auto* note = layer->findNote (id); note != nullptr && note->groupId >= 0)
        {
            note->groupId = -1;
            changed = true;
        }

    // Phase 35: Shift+G on a mixed selection has to take the blocks out too, or
    // the group would still exist — held open by members the user just asked to
    // leave, and still counted in its bracket.
    for (int id : sampleSelection)
        if (auto* clip = layer->findSample (id); clip != nullptr && clip->groupId >= 0)
        {
            clip->groupId = -1;
            changed = true;
        }

    if (changed)
    {
        pruneEmptyGroups();
        stack.notifyNotesChanged();
        repaint();
    }

    return changed;
}

bool PianoRollComponent::disbandGroup (int layerIndex, int groupId)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr || layer->findGroup (groupId) == nullptr)
        return false;

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

int PianoRollComponent::getGroupLabelAt (juce::Point<float> position) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
    {
        const auto* layer = stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& group : layer->getGroups())
        {
            // Phase 21: the pill now ends in a handle, and that handle is its
            // own hit target. Without this exclusion clicking it would select
            // the group instead of opening the curve — which is exactly the bug
            // the note's tail button already taught us to avoid.
            if (getGroupAutomationHandleBounds (i, group.getId()).contains (position))
                continue;

            if (getGroupLabelBounds (i, group.getId()).contains (position))
                return group.getId();
        }
    }

    return -1;
}

int PianoRollComponent::getGroupFrameAt (juce::Point<float> position) const
{
    // Only the active layer: a ghost group is not editable, and a click on one
    // that picked the group up would drag notes the user cannot even select.
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    for (const auto& group : layer->getGroups())
    {
        const auto content = getGroupContentBounds (*layer, group);

        if (content.isEmpty())
            continue;

        // Same expansion the frame is drawn with, so what looks like the
        // group's edge is the group's edge for the pointer too.
        if (content.expanded (ui::pf (3), ui::pf (3)).contains (position))
            return group.getId();
    }

    return -1;
}

bool PianoRollComponent::isGroupFocused (int layerIndex, int groupId) const
{
    if (focusSlotId < 0)
        return false;

    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return false;

    for (const auto& note : layer->getNotes())
        if (note.groupId == groupId
             && routing::resolve (*layer, note, rack).slotId == focusSlotId)
            return true;

    return false;
}

juce::Rectangle<float> PianoRollComponent::getGroupContentBounds (const Layer& layer,
                                                                 const NoteGroup& group) const
{
    const auto area = getGridArea();
    juce::Rectangle<float> unionBounds;

    for (const auto& note : layer.getNotes())
    {
        if (note.groupId != group.getId())
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
        if (sample.groupId != group.getId())
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

juce::Rectangle<float> PianoRollComponent::getGroupLabelBounds (int layerIndex, int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return {};

    const auto content = getGroupContentBounds (*layer, *group);

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

    const float width = juce::jlimit (ui::pf (34), ui::pf (168),
                                      glyphs.getBoundingBox (0, -1, true).getWidth()
                                        + ui::pf (16) + ui::pf (13));

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
                                                                          int groupId) const
{
    const auto pill = getGroupLabelBounds (layerIndex, groupId);

    if (pill.isEmpty())
        return {};

    // The right end of the pill, inset so the pill's rounded corner still
    // reads: a handle flush with the edge looks like a clipped button.
    const float w = ui::pf (12);

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
    }
}

void PianoRollComponent::showGroupMenu (int layerIndex, int groupId, juce::Point<int> screenPos)
{
    auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return;

    auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return;

    const int members = layer->countNotesInGroup (groupId);
    const int clips   = layer->countSamplesInGroup (groupId);   // Phase 27

    juce::PopupMenu menu;
    menu.addSectionHeader (group->getName() + utf8 (" · ")
                             + juce::String (members) + utf8 (" 音符")
                             + (clips > 0 ? utf8 (" · ") + juce::String (clips) + utf8 (" 采样")
                                          : juce::String()));
    menu.addItem (1, utf8 ("选中整组"));
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
    menu.addSeparator();
    menu.addItem (5, utf8 ("解散编组（音符保留）"));

    juce::Component::SafePointer<PianoRollComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                        [safe, layerIndex, groupId, screenPos] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            switch (result)
                            {
                                case 1: safe->selectGroup (groupId); break;

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
                                    safe->disbandGroup (layerIndex, groupId);
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

                                default: break;
                            }
                        });
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
        static const Tool tools[4] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand };

        for (int i = 0; i < 4; ++i)
            if (getToolButtonBounds (i).contains (pos))
            {
                setTool (tools[i]);
                return;
            }

        if (getToolButtonBounds (4).contains (pos))
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

        if (getToolButtonBounds (5).contains (pos))
        {
            // Phase 16. With nothing selected there is nothing to group, and
            // silently doing nothing is worse than saying so in the strip.
            // Phase 38: "something" means either kind — a clip-only selection
            // used to reach this line and get nothing.
            if (hasAnySelection())
                groupSelection();

            return;
        }

        if (getToolButtonBounds (6).contains (pos))
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

        if (getToolButtonBounds (7).contains (pos))
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

        if (onLocateRequested != nullptr)
        {
            const double beat = juce::jlimit (0.0, timelineBeats, beatAtScreenX (e.position.x));
            onLocateRequested (snapEnabled ? snapBeat (beat) : beat);
        }

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
    if (const int groupId = getGroupLabelAt (e.position); groupId >= 0)
    {
        const int layerIndex = layerIndexOfGroup (groupId);

        if (e.mods.isPopupMenu())
        {
            showGroupMenu (layerIndex, groupId, e.getScreenPosition());
            return;
        }

        armGroupMove (groupId, e.position);
        return;
    }

    // ---- Phase 17: an audio clip under the cursor --------------------------
    // Before the notes: a block is the size of several lanes, so it is the
    // thing you are aiming at when both are under the pointer.
    if (auto* clip = hitTestSample (e.position))
    {
        const int clipId = clip->id;

        // Phase 28: the click is also the selection. Selecting before the menu
        // means every item in it acts on what the user just pointed at.
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
            toggleSampleSelection (clipId);
        else if (! isSampleSelected (clipId))
            setSampleSelection ({ clipId });

        if (e.mods.isPopupMenu())
        {
            showSampleMenu (clipId, e.getScreenPosition());
            return;
        }

        if (tool == Tool::eraser)
        {
            if (auto* activeLayer = stack.getActiveLayer())
            {
                activeLayer->removeSample (clipId);
                sampleSelection.erase (std::remove (sampleSelection.begin(),
                                                    sampleSelection.end(), clipId),
                                       sampleSelection.end());
                pruneEmptyGroups();
                stack.notifyNotesChanged();
            }

            return;
        }

        const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), clipId);

        // Phase 29: three kinds of handle on one block — the two fade points
        // along the top strip, and the two ends. The fades win inside the strip
        // because when both are zero their handles sit exactly on the corners,
        // which is also where the trim bands are; the trim band keeps the rest
        // of the block's height, so both gestures stay reachable.
        if (const int fade = hitTestSampleFadeHandle (e.position); fade >= 0)
        {
            dragSampleId       = clipId;
            dragSampleOriginal = *clip;
            dragAnchorBeat     = beatAtScreenX (e.position.x);
            dragMode           = fade == 0 ? DragMode::fadeSampleIn : DragMode::fadeSampleOut;
            repaint();
            return;
        }

        const float band   = ui::pf (kResizeHandleWidth);
        const bool  onTail = e.position.x > bounds.getRight() - band;
        const bool  onHead = ! onTail && e.position.x < bounds.getX() + band;

        // Phase 28/29: only the grabbed block resizes (same rule as notes, P25);
        // anything else arms a RIGID move of the whole selection, clips and
        // notes together.
        if ((onTail || onHead) && isSampleSelected (clipId))
        {
            dragSampleId       = clipId;
            dragSampleOriginal = *clip;
            dragAnchorBeat     = beatAtScreenX (e.position.x);
            dragMode           = onTail ? DragMode::resizeSample : DragMode::resizeSampleLeft;
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

    // ---- Hand tool: drag to pan --------------------------------------------
    if (tool == Tool::hand)
    {
        dragMode = DragMode::panView;
        panAnchor = e.position;
        panAnchorScrollY = scrollY;
        panAnchorBeats = scrollBeats;
        return;
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
        if (const int frameGroup = getGroupFrameAt (e.position); frameGroup >= 0)
        {
            armGroupMove (frameGroup, e.position);
            return;
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

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            dragOriginals.push_back (note);
}

void PianoRollComponent::captureGroupClips (int groupId)
{
    dragClipOriginals.clear();

    auto* active = stack.getActiveLayer();

    if (active == nullptr || groupId < 0)
        return;

    // Both capture and apply live here rather than in clampMove: the clamp
    // answers "how far", this answers "what moves", and mixing the two is how
    // a group ends up dragging clips that are not in it.
    for (const auto& clip : active->getSamples())
        if (clip.groupId == groupId)
            dragClipOriginals.push_back (clip);
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
}

bool PianoRollComponent::simulateGroupMove (int layerIndex, int groupId,
                                            double deltaBeats, int deltaPitch)
{    if (stack.getActiveLayerIndex() != layerIndex)
        return false;
    auto* active = stack.getActiveLayer();

    if (active == nullptr || active->findGroup (groupId) == nullptr)
        return false;

    selectGroup (groupId);
    captureDragOriginals();
    captureGroupClips (groupId);

    if (dragOriginals.empty() && dragClipOriginals.empty())
        return false;

    const auto move = clampMove (dragOriginals, deltaBeats, deltaPitch, false, dragClipOriginals);
    applyMove (move);
    stack.notifyNotesChanged();

    return true;
}

juce::Rectangle<float> PianoRollComponent::groupFrameOf (int layerIndex, int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    return group != nullptr ? getGroupContentBounds (*layer, *group) : juce::Rectangle<float>();
}

void PianoRollComponent::armGroupMove (int groupId, juce::Point<float> position)
{
    // Selecting is not moving: a group whose only gesture is "become the
    // selection" reads as broken the moment you try to drag it. So the same
    // click arms a rigid move of every member — the rules are unchanged, it is
    // still clampMove() over dragOriginals, exactly like dragging one of its
    // notes.
    selectGroup (groupId);
    captureDragOriginals();
    captureGroupClips (groupId);

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
        // nothing is a note you can lose with a twitch.
        note->lengthBeats = juce::jmax (gridDivision, wantedBeat - origin.startBeat);
        return true;
    }

    // Trim the front: the tail stays where it was, which is what makes "make
    // this start later" not also mean "make this end later".
    const double end      = origin.startBeat + origin.lengthBeats;
    const double newStart = juce::jlimit (0.0,
                                          juce::jmax (0.0, end - gridDivision),
                                          wantedBeat);

    note->startBeat   = newStart;
    note->lengthBeats = juce::jmax (gridDivision, end - newStart);
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
        if (onLocateRequested != nullptr)
        {
            const double beat = juce::jlimit (0.0, timelineBeats, beatAtScreenX (e.position.x));
            onLocateRequested (snapEnabled ? snapBeat (beat) : beat);
        }

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

            applySampleTrim (dragSampleOriginal, wanted, dragMode == DragMode::resizeSample);

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::fadeSampleIn:
        case DragMode::fadeSampleOut:
        {
            if (auto* clip = active->findSample (dragSampleId))
            {
                const bool   atTail = dragMode == DragMode::fadeSampleOut;
                const double x      = beatAtScreenX (e.position.x);

                // Measured inwards from the edge the handle belongs to, so the
                // fade keeps meaning "this much of the block" while the block
                // itself is being dragged around.
                applySampleFade (dragSampleId, atTail,
                                 atTail ? clip->endBeat() - x : x - clip->startBeat);

                stack.notifyNotesChanged();
            }

            break;
        }

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
    dragResizeNoteId = -1;
    dragEnvelopePoint = -1;
    dragSampleId = -1;
    dragGroupPoint = -1;        // Phase 21: the group box's handle, if any

    if (wasGroupCurveDrag)
        stack.notifyNotesChanged();     // the whole drag, as one history step

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
    const int groupUnderMouse = getGroupLabelAt (e.position);
    const int groupLayer      = groupUnderMouse >= 0 ? layerIndexOfGroup (groupUnderMouse) : -1;

    if (groupUnderMouse != hoveredGroupId || groupLayer != hoveredGroupLayer)
    {
        hoveredGroupId    = groupUnderMouse;
        hoveredGroupLayer = groupLayer;
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
        if (hitTestSampleFadeHandle (e.position) >= 0)
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
         || hoveredGroupId != -1 || hoveredGroupHandle || hoveredGroupMenu)
    {
        hoveredNoteId = -1;
        hoveredTool = -1;
        hoveredAutomationMenu = false;
        hoveredEnvelopePoint  = -1;
        hoveredGroupId        = -1;
        hoveredGroupLayer     = -1;
        hoveredGroupHandle    = false;
        hoveredGroupMenu      = false;
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
    const Tool tools[4] { Tool::pencil, Tool::eraser, Tool::select, Tool::hand };

    const auto tint = juce::Colour (0xff4fd1c5);

    for (int i = 0; i < 4; ++i)
    {
        const bool active  = tool == tools[i];
        const bool hovered = hoveredTool == i;
        const auto bounds  = getToolButtonBounds (i).toFloat();

        g.setColour (active ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        if (active)
        {
            g.setColour (tint.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);
        }

        drawToolIcon (g, i, bounds, active ? juce::Colours::white : kText);
    }

    //-- Copy-state button -----------------------------------------------------
    // Not a mode: an action with a lit "loaded" state. It says "画笔状态" on it
    // because a dropper icon alone reads as "pick colour" to everyone who has
    // used a paint program.
    {
        const auto bounds  = getToolButtonBounds (4).toFloat();
        const bool hovered = hoveredTool == 4;
        const bool loaded  = hasPaintState;

        g.setColour (loaded ? tint.withAlpha (0.22f)
                            : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (loaded ? tint.withAlpha (0.85f)
                            : juce::Colours::white.withAlpha (hovered ? 0.20f : 0.10f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

        drawToolIcon (g, 4, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
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
        const auto bounds  = getToolButtonBounds (5).toFloat();
        const bool hovered = hoveredTool == 5;
        const bool inGroup = getSelectedGroupId() >= 0;
        const bool enabled = hasAnySelection();

        g.setColour (inGroup ? tint.withAlpha (0.22f)
                             : juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (inGroup ? tint.withAlpha (0.85f)
                             : juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

        drawToolIcon (g, 5, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
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
        const auto bounds  = getToolButtonBounds (6).toFloat();
        const bool hovered = hoveredTool == 6;
        const bool enabled = hasAnySelection();
        const bool grouped = getSelectedGroupId() >= 0;

        g.setColour (juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

        drawToolIcon (g, 6, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
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
        const auto bounds  = getToolButtonBounds (7).toFloat();
        const bool hovered = hoveredTool == 7;
        const bool on      = snapEnabled;

        g.setColour (on ? tint.withAlpha (0.22f)
                        : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (on ? tint.withAlpha (0.85f)
                        : juce::Colours::white.withAlpha (0.20f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

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
    g.setColour (juce::Colour (0xff4fd1c5).withAlpha (dragMode == DragMode::scrollThumb ? 0.75f : 0.45f));
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

    g.setColour (juce::Colour (0xff4fd1c5).withAlpha (0.075f));

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

    const int firstStep = (int) std::floor (scrollBeats / gridDivision);
    const int lastStep  = (int) std::ceil ((scrollBeats + (double) area.getWidth() / beatWidth) / gridDivision);

    for (int step = firstStep; step <= lastStep; ++step)
    {
        const double beat = (double) step * gridDivision;

        if (beat < 0.0 || beat > timelineBeats)
            continue;

        const float x = std::floor (screenXForBeat (beat)) + 0.5f;

        if (step % 16 == 0)          // bar
            g.setColour (juce::Colours::white.withAlpha (0.16f));
        else if (step % 4 == 0)      // beat
            g.setColour (juce::Colours::white.withAlpha (0.075f));
        else                         // 1/16
            g.setColour (juce::Colours::white.withAlpha (0.030f));

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

        for (const auto& group : layer->getGroups())
        {
            const auto content = getGroupContentBounds (*layer, group);

            if (content.isEmpty())
                continue;

            const bool focused = isGroupFocused (i, group.getId());
            const auto colour  = group.getColour();
            const auto frame   = content.expanded (ui::pf (3), ui::pf (3));

            g.setColour (colour.withAlpha (baseAlpha * (focused ? 0.10f : 0.05f)));
            g.fillRoundedRectangle (frame, ui::pf (5));

            g.setColour (colour.withAlpha (baseAlpha * (focused ? 0.85f : 0.34f)));
            g.drawRoundedRectangle (frame, ui::pf (5), focused ? ui::pf (1.4) : ui::pf (1.0));
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

        for (const auto& group : layer->getGroups())
        {
            const auto pill = getGroupLabelBounds (i, group.getId());

            if (pill.isEmpty())
                continue;

            const bool focused = isGroupFocused (i, group.getId());
            const bool hovered = (hoveredGroupId == group.getId() && hoveredGroupLayer == i);
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

            g.setColour (focused || hovered ? juce::Colour (0xff10141a)
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
                const auto handle = getGroupAutomationHandleBounds (i, group.getId());
                const bool open   = (expandedGroupId == group.getId()
                                       && expandedGroupLayerIndex == i);
                const bool hot    = hoveredGroupHandle
                                      && hoveredHandleGroupId == group.getId()
                                      && hoveredHandleLayer == i;

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

            // A mount marker: the pill's bottom edge is underlined in the
            // synth's own colour, so "this group plays on B" is visible without
            // opening anything. An underline rather than a dot because a filled
            // pill has no room for a dot that reads.
            if (! group.followsLayerOutput())
            {
                const auto* slot = rack.findSlot (group.getOutputSlotId());

                g.setColour ((slot != nullptr ? slot->colour : juce::Colour (0xffff6b6b))
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
    g.setColour (juce::Colour (0xff1b1e25));
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
        const auto badgeColour = target != nullptr ? target->colour : juce::Colour (0xffff6b6b);

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

        g.setColour (juce::Colour (0xff4fd1c5).withAlpha (0.90f));
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

            g.setColour (juce::Colour (0xff4fd1c5).withAlpha (0.30f));

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
            g.setColour (juce::Colour (0xff4fd1c5).withAlpha (0.45f));
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

    g.setColour (juce::Colour (0xff15161b));
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
                g.setColour (juce::Colour (0xff4fd1c5).withAlpha (black ? 0.55f : 0.40f));
                g.fillRect (row);

                g.setColour (juce::Colour (0xff4fd1c5));
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

        case 4:     // copy note state: a dropper, with a drop of what it picked up
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

        case 5:     // group: three blocks behind a bracket
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

        case 6:     // ungroup: the group mark, with the brackets coming off
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

        case 7:     // snap: a small horseshoe magnet, poles up
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
    closeGroupAutomation();

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

    g.setColour (juce::Colour (0xff0b0c10).withAlpha (0.97f));
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

void PianoRollComponent::openGroupAutomation (int layerIndex, int groupId, int target)
{
    // One floating editor at a time, same rule as the note box.
    closeNoteAutomation();

    expandedGroupLayerIndex = layerIndex;
    expandedGroupId         = groupId;
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
                                                          const NoteGroup& group) const
{
    bool   any = false;
    double lo  = 0.0;
    double hi  = 0.0;

    for (const auto& note : layer.getNotes())
    {
        if (note.groupId != group.getId())
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
                                                                      int groupId) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    const auto* group = layer->findGroup (groupId);

    if (group == nullptr)
        return {};

    const auto span = getGroupBeatSpan (*layer, *group);

    if (span.isEmpty())
        return {};

    const auto content = getGroupContentBounds (*layer, *group);

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
    const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);

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
    const auto box = getGroupAutomationBoxBounds (expandedGroupLayerIndex, expandedGroupId);

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

    // The piano roll's own grid: same 1/16 step, same bar / beat / 16th
    // emphasis, same pixel rounding as drawGridLines(). A box that drew
    // decorative quarter-marks put its curve on a grid that did not exist
    // anywhere else on screen, which is the sort of thing that makes an editor
    // feel slightly wrong without anyone being able to say why.
    const int firstStep = (int) std::floor (beatAtScreenX (area.getX()) / gridDivision);
    const int lastStep  = (int) std::ceil  (beatAtScreenX (area.getRight()) / gridDivision);

    for (int step = firstStep; step <= lastStep; ++step)
    {
        const double beat = (double) step * gridDivision;

        if (beat < 0.0 || beat > timelineBeats)
            continue;

        const float x = std::floor (screenXForBeat (beat)) + 0.5f;

        if (x < area.getX() - 1.0f || x > area.getRight() + 1.0f)
            continue;

        if (step % 16 == 0)          // bar
            g.setColour (juce::Colours::white.withAlpha (0.16f));
        else if (step % 4 == 0)      // beat
            g.setColour (juce::Colours::white.withAlpha (0.075f));
        else                         // 1/16
            g.setColour (juce::Colours::white.withAlpha (0.030f));

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

    g.setColour (juce::Colour (0xff0b0c10).withAlpha (0.97f));
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
bool PianoRollComponent::copyNotesToClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || selection.empty())
        return false;

    noteClip.clear();
    noteClipFirstBeat = std::numeric_limits<double>::max();
    noteClipEndBeat   = 0.0;
    groupClip         = GroupClip {};

    for (const auto& note : active->getNotes())
    {
        if (! isSelected (note.id))
            continue;

        noteClip.push_back (note);
        noteClipFirstBeat = juce::jmin (noteClipFirstBeat, note.startBeat);
        noteClipEndBeat   = juce::jmax (noteClipEndBeat, note.startBeat + note.lengthBeats);
    }

    if (noteClip.empty())
    {
        noteClipFirstBeat = 0.0;
        return false;
    }

    //-- Phase 24: a whole group is copied as an OBJECT ----------------------
    // The alternative — copying the members and letting the paste attach them
    // to the same group id — produces one bigger group, not a copy: membership
    // lives on the note, so the two halves would share the mount and the curve.
    const int groupId = getSelectedGroupId();

    if (groupId >= 0 && isWholeGroupSelected (groupId))
    {
        if (const auto* group = active->findGroup (groupId))
        {
            groupClip.valid        = true;
            groupClip.name         = group->getName();
            groupClip.colour       = group->getColour();
            groupClip.outputSlotId = group->getOutputSlotId();
            groupClip.levelDb      = group->getLevelDb();
            groupClip.pan          = group->getPan();
            groupClip.pluginParam  = group->getPluginParam();
            groupClip.startBeat    = noteClipFirstBeat;

            for (int i = 0; i < numGroupAutomationTargets; ++i)
            {
                const auto target = (GroupAutomationTarget) i;
                groupClip.lanes[(size_t) i]  = group->automationFor (target);
                groupClip.laneOn[(size_t) i] = group->automationFor (target).isEnabled();
            }

            flashStatus (utf8 ("已复制 ") + group->getName()
                           + utf8 (" · ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符"));
            return true;
        }
    }

    flashStatus (utf8 ("已复制 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符"));
    return true;
}

bool PianoRollComponent::cutNotesToClipboard()
{
    if (! copyNotesToClipboard())
        return false;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    for (int id : selection)
        active->removeNote (id);

    selection.clear();
    hoveredNoteId = -1;
    notifySelectionChanged();
    pruneEmptyGroups();
    stack.notifyNotesChanged();

    flashStatus ((groupClip.valid ? utf8 ("已剪切 ") + groupClip.name
                                 : utf8 ("已剪切 ") + juce::String ((int) noteClip.size()) + utf8 (" 个音符")));
    return true;
}

bool PianoRollComponent::pasteNotesFromClipboard()
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr || noteClip.empty())
        return false;

    // Paste at the playhead when there is one — that is the only landmark on
    // this timeline that means "here". With the playhead parked exactly on the
    // block the copy came from, shift by the block's own length, because a
    // paste that lands on top of its source looks like nothing happened.
    double anchor = playheadBeat >= 0.0 ? playheadBeat : noteClipFirstBeat;

    if (snapEnabled)
        anchor = snapBeat (anchor);

    if (std::abs (anchor - noteClipFirstBeat) < 1.0e-9)
        anchor = noteClipFirstBeat + juce::jmax (gridDivision, noteClipEndBeat - noteClipFirstBeat);

    // Same rounding as a drag: the anchor is snapped once and every note keeps
    // its offset, so the block arrives with its internal timing intact.
    const double delta = anchor - noteClipFirstBeat;

    //-- Phase 24: a copied group becomes a SECOND group ---------------------
    // Created before the notes so they can be born inside it: a note whose
    // groupId points at a group that does not exist yet would be repaired to
    // "ungrouped" by the model, and the copy would paste as loose notes.
    int newGroupId = -1;

    if (groupClip.valid)
    {
        newGroupId = active->addGroup (uniqueGroupName (*active, groupClip.name), groupClip.colour);

        if (auto* copy = active->findGroup (newGroupId))
        {
            copy->setOutputSlotId (groupClip.outputSlotId);
            copy->setLevelDb (groupClip.levelDb);
            copy->setPan (groupClip.pan);
            copy->setPluginParam (groupClip.pluginParam);
            copy->setCollapsed (false);

            for (int i = 0; i < numGroupAutomationTargets; ++i)
            {
                auto& lane = copy->automationFor ((GroupAutomationTarget) i);

                // A group lane is ABSOLUTE time, so a copy that lands later has
                // to move its curve with it — otherwise pasting four bars later
                // would keep the shape at the old beats and the notes would
                // arrive at a flat line.
                auto points = groupClip.lanes[(size_t) i].getPoints();

                for (auto& point : points)
                    point.beat = juce::jmax (0.0, point.beat + delta);

                lane.loadPoints (std::move (points));
                lane.setEnabled (groupClip.laneOn[(size_t) i]);
            }
        }
        else
        {
            newGroupId = -1;
        }
    }

    std::vector<int> newIds;

    for (auto note : noteClip)
    {
        note.startBeat = juce::jmax (0.0, note.startBeat + delta);
        note.id        = 0;

        if (newGroupId >= 0)
            note.groupId = newGroupId;

        const int newId = active->addNote (note);

        if (newId > 0)
            newIds.push_back (newId);
    }

    if (newIds.empty())
        return false;

    ensureTimelineExtendsTo (anchor + juce::jmax (gridDivision, noteClipEndBeat - noteClipFirstBeat) + 4.0);
    setSelection (newIds);
    stack.notifyNotesChanged();

    if (newGroupId >= 0)
    {
        const auto* pasted = active->findGroup (newGroupId);

        flashStatus (utf8 ("已粘贴 ") + (pasted != nullptr ? pasted->getName() : juce::String())
                       + utf8 (" · ") + juce::String ((int) newIds.size()) + utf8 (" 个音符"));
    }
    else
    {
        flashStatus (utf8 ("已粘贴 ") + juce::String ((int) newIds.size()) + utf8 (" 个音符"));
    }

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
double PianoRollComponent::maxSampleLengthBeats (const Layer& layer, const SampleEvent& sample) const
{
    const auto audio = clipAudio (layer, sample);

    if (audio == nullptr || audio->lengthSeconds() <= 0.0)
        return 0.0;                  // unknown file: no limit, so the block stays editable

    const double remaining = juce::jmax (0.0, audio->lengthSeconds()
                                              - juce::jmax (0.0, sample.sourceOffsetSeconds));

    return SampleEvent::beatsForSeconds (remaining, bpm);
}

bool PianoRollComponent::applySampleTrim (const SampleEvent& origin, double wantedBeat, bool fromTail)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* clip = active->findSample (origin.id);

    if (clip == nullptr)
        return false;

    const double minLength = gridDivision;

    if (fromTail)
    {
        double length = juce::jmax (minLength, wantedBeat - origin.startBeat);

        // A block cannot be longer than the part of the file it plays. Past that
        // the extra width is silence, so "drag it longer" would be a gesture
        // that changes the picture and not the sound — the one thing a block
        // must never do. Shorter is always allowed, which is why this clamps the
        // upper bound only.
        //
        // Phase 30: the ceiling is never BELOW where the drag started. A block
        // dropped onto the grid has its length rounded up, so it is already a
        // little longer than its file; clamping to the file on the first pixel
        // of a drag made the tail snap inwards on its own.
        if (const double maxLength = maxSampleLengthBeats (*active, origin); maxLength > 0.0)
            length = juce::jmin (length, juce::jmax (minLength, juce::jmax (origin.lengthBeats, maxLength)));

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

    // A fade is a length inside the block, so it cannot outlive it.
    clip->fadeInBeats  = juce::jlimit (0.0, clip->lengthBeats, clip->fadeInBeats);
    clip->fadeOutBeats = juce::jlimit (0.0, clip->lengthBeats, clip->fadeOutBeats);

    ensureTimelineExtendsTo (clip->endBeat());
    return true;
}

bool PianoRollComponent::applySampleFade (int sampleId, bool atTail, double beats)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* clip = active->findSample (sampleId);

    if (clip == nullptr)
        return false;

    const double value = juce::jlimit (0.0, clip->lengthBeats, beats);

    if (atTail)
        clip->fadeOutBeats = value;
    else
        clip->fadeInBeats = value;

    return true;
}

float PianoRollComponent::getSampleFadeHandleX (int layerIndex, int sampleId, bool atTail) const
{
    const auto* layer = stack.getLayer (layerIndex);

    if (layer == nullptr)
        return 0.0f;

    const auto* sample = layer->findSample (sampleId);

    if (sample == nullptr)
        return 0.0f;

    const auto rect = getSampleScreenBounds (layerIndex, sampleId);
    const double beats = juce::jmax (0.0, atTail ? sample->fadeOutBeats : sample->fadeInBeats);
    const float  dx    = (float) juce::jmin ((double) rect.getWidth(), beats * beatWidth);

    return atTail ? rect.getRight() - dx : rect.getX() + dx;
}

int PianoRollComponent::hitTestSampleFadeHandle (juce::Point<float> position) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr)
        return -1;

    const int index = stack.getActiveLayerIndex();

    for (const auto& sample : layer->getSamples())
    {
        const auto rect = getSampleScreenBounds (index, sample.id);

        if (! rect.contains (position))
            continue;

        // Only the top strip answers. The rest of the height is the trim band,
        // which is what keeps "make it shorter" reachable on a block whose
        // fades are both zero and whose handles therefore sit on the corners.
        const float strip = juce::jmin (ui::pf (kFadeStripHeight), rect.getHeight() * 0.5f);

        if (position.y > rect.getY() + strip)
            continue;

        const float near = ui::pf (kFadeGrabRadius);
        const float xIn  = getSampleFadeHandleX (index, sample.id, false);
        const float xOut = getSampleFadeHandleX (index, sample.id, true);

        const float dIn  = std::abs (position.x - xIn);
        const float dOut = std::abs (position.x - xOut);

        if (dIn > near && dOut > near)
            continue;

        // Whichever is closer wins, so two handles that overlap in a very short
        // block still give a predictable answer.
        return dIn <= dOut ? 0 : 1;
    }

    return -1;
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

    // A fade is a length inside the block, so it cannot outlive it.
    sample->fadeInBeats  = juce::jlimit (0.0, sample->lengthBeats, sample->fadeInBeats);
    sample->fadeOutBeats = juce::jlimit (0.0, sample->lengthBeats, sample->fadeOutBeats);

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
        clip->stretchMode = mode;
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

    if (! applySampleTrim (origin, wantedBeat, fromTail))
        return false;

    stack.notifyNotesChanged();
    return true;
}

bool PianoRollComponent::simulateSampleFade (int sampleId, bool atTail, double beats)
{
    if (! applySampleFade (sampleId, atTail, beats))
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

    for (const auto& sample : layer->getSamples())
        if (getSampleScreenBounds (index, sample.id).contains (position))
            return layer->findSample (sample.id);

    return nullptr;
}

const SampleEvent* PianoRollComponent::hitTestSample (juce::Point<float> position) const
{
    const auto* layer = stack.getActiveLayer();

    // Same rule as notes: only the active, visible sheet answers the mouse.
    if (layer == nullptr)
        return nullptr;

    const int index = stack.getActiveLayerIndex();

    for (const auto& sample : layer->getSamples())
        if (getSampleScreenBounds (index, sample.id).contains (position))
            return layer->findSample (sample.id);

    return nullptr;
}

int PianoRollComponent::getEditableSampleIdAt (juce::Point<float> position) const
{
    if (auto* sample = hitTestSample (position))
        return sample->id;

    return -1;
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

    for (int id : sampleSelection)
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

        // A fade is a length INSIDE the block, so it cannot outlive it.
        sample->fadeInBeats  = juce::jlimit (0.0, sample->lengthBeats, sample->fadeInBeats);
        sample->fadeOutBeats = juce::jlimit (0.0, sample->lengthBeats, sample->fadeOutBeats);

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

    for (int id : selection)
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

    std::vector<int> newIds;

    for (auto clip : sampleClip)
    {
        clip.startBeat = juce::jmax (0.0, clip.startBeat + delta);
        clip.id        = 0;

        // A pasted block does NOT inherit the group it was cut from. Membership
        // means "these move together", and a copy that lands four bars later is
        // not part of the shape the original group describes.
        clip.groupId   = -1;

        // Phase 33: a CUT takes its sampler with it — the sweep runs the moment
        // the last block leaves, so the slot a clipboard holds may be gone by
        // the time it is pasted. Re-registering the file is what makes
        // cut-then-paste a move instead of a block pointed at nothing (which
        // would quietly play the layer's instrument instead of the sample).
        if (clip.outputSlotId >= 0 && rack.findSlot (clip.outputSlotId) == nullptr
             && clip.audio != nullptr)
            clip.outputSlotId = rack.addSampler (clip.audio, clip.path, clip.name);

        const int newId = active->addSample (std::move (clip));

        if (newId > 0)
        {
            newIds.push_back (newId);

            if (const auto* created = active->findSample (newId))
                ensureTimelineExtendsTo (created->endBeat());
        }
    }

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
    // Both clipboards are written by the same gesture, and the one with nothing
    // selected is EMPTIED. That is the whole point: a paste must not resurrect
    // the clips from a copy three actions ago just because this time only notes
    // happened to be selected.
    if (! selection.empty())
        copyNotesToClipboard();
    else
    {
        noteClip.clear();
        groupClip = GroupClip {};
    }

    if (! sampleSelection.empty())
        copySamplesToClipboard();
    else
    {
        sampleClip.clear();
        sampleClipFirstBeat = 0.0;
        sampleClipLengthBeat = 1.0;
        samplePasteNextBeat = -1.0;
        samplePastePlayheadSeen = -2.0;
    }
}

bool PianoRollComponent::pasteEditClipboard()
{
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

    SampleEvent sample;
    sample.name  = file.getFileName();
    sample.path  = file.getFullPathName();
    sample.audio = loaded.buffer;
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
                                     SampleEvent::beatsForSeconds (loaded.buffer->lengthSeconds(),
                                                                   bpm));

    sample.startBeat = juce::jmax (0.0, snapEnabled ? snapBeat (beat) : beat);

    // Phase 27: dropping a file also grows a sampler for it in the rack, and
    // the clip is mounted on that sampler. Two consequences, both intended:
    // "which file is this" becomes a routing choice you can see in the rack,
    // and a second clip can be pointed at the same sampler instead of decoding
    // the file again. A rack that already holds this exact path reuses its
    // sampler, so dropping one file twice does not leave twins behind.
    sample.outputSlotId = rack.addSampler (loaded.buffer, sample.path, sample.name);

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
                    const double t0 = startSec + (double) i * secPerPixel;
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

        if (sample.stretchMode != StretchMode::none && rect.getWidth() > ui::pf (104))
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

                    g.setColour (juce::Colour (0xffffd166).withAlpha (0.86f * alpha));
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

    //-- Phase 29: the fade shapes, drawn over the body but under the selection
    // ring. After the name strip on purpose: the handles belong to the corners,
    // and a strip filled afterwards would paint over them.
    drawSampleFades (g, rect, sample, colour, alpha);

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

void PianoRollComponent::drawSampleFades (juce::Graphics& g, juce::Rectangle<float> block,
                                          const SampleEvent& sample, juce::Colour colour,
                                          float alpha)
{
    if (block.getWidth() < ui::pf (3.0f) || block.getHeight() < ui::pf (4.0f))
        return;

    const double inBeats  = juce::jlimit (0.0, sample.lengthBeats, sample.fadeInBeats);
    const double outBeats = juce::jlimit (0.0, sample.lengthBeats, sample.fadeOutBeats);

    const float inWidth  = (float) juce::jmin ((double) block.getWidth(), inBeats  * beatWidth);
    const float outWidth = (float) juce::jmin ((double) block.getWidth(), outBeats * beatWidth);

    const float top    = block.getY();
    const float bottom = block.getBottom();
    const float xIn    = block.getX() + inWidth;
    const float xOut   = block.getRight() - outWidth;

    // Shade what the fade takes away. The area ABOVE the gain line is the part
    // that is turned down, so it is darkened rather than redrawn — a second
    // waveform would suggest a second sound.
    g.setColour (juce::Colours::black.withAlpha (0.36f * alpha));

    if (inWidth > 0.5f)
    {
        juce::Path shade;
        shade.startNewSubPath (block.getX(), top);
        shade.lineTo (xIn, top);
        shade.lineTo (block.getX(), bottom);
        shade.closeSubPath();
        g.fillPath (shade);
    }

    if (outWidth > 0.5f)
    {
        juce::Path shade;
        shade.startNewSubPath (xOut, top);
        shade.lineTo (block.getRight(), top);
        shade.lineTo (block.getRight(), bottom);
        shade.closeSubPath();
        g.fillPath (shade);
    }

    // The gain line. Two legs when the fades overlap, so a block that is all
    // fade reads as a triangle instead of a crossed-over scribble.
    juce::Path line;
    line.startNewSubPath (block.getX(), bottom);
    line.lineTo (xIn, top);

    if (xIn < xOut)
        line.lineTo (xOut, top);
    else
        line.startNewSubPath (xOut, top);

    line.lineTo (block.getRight(), bottom);

    g.setColour (colour.brighter (0.9f).withAlpha (0.90f * alpha));
    g.strokePath (line, juce::PathStrokeType (ui::pf (1.0f), juce::PathStrokeType::mitered,
                                              juce::PathStrokeType::butt));

    // The two handles. Small on purpose: they sit on the corners the trim bands
    // also use, and a big square there would make the trim look unavailable.
    const float size = ui::pf (kFadeHandleSize);

    if (block.getWidth() > size * 2.0f)
    {
        for (const float x : { xIn, xOut })
        {
            const auto mark = juce::Rectangle<float> (x - size * 0.5f, top, size, size);

            g.setColour (juce::Colours::white.withAlpha (0.92f * alpha));
            g.fillRoundedRectangle (mark, ui::pf (1.0f));

            g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
            g.drawRoundedRectangle (mark.reduced (0.5f), ui::pf (1.0f), 1.0f);
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

    const auto colour = juce::Colour (0xff4fd1c5);

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

        stretch.addItem (kClipStretchBase + (int) StretchMode::none,
                         utf8 ("原速 · 只截取窗口"), true, current == StretchMode::none);
        stretch.addSeparator();
        stretch.addItem (kClipStretchBase + (int) StretchMode::resample,
                         utf8 ("变速变调 · 像磁带"), true, current == StretchMode::resample);
        stretch.addItem (kClipStretchBase + (int) StretchMode::tempo,
                         utf8 ("只变速不变调 · 时间伸缩"), true, current == StretchMode::tempo);
        stretch.addItem (kClipStretchBase + (int) StretchMode::pitch,
                         utf8 ("不变速只变调 · 按长度移调"), true, current == StretchMode::pitch);

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

        if (auto* target = owner->findSample (sampleId))
        {
            if (result == 2)
                target->startBeat = safe->snapEnabled ? safe->snapBeat (target->startBeat)
                                                      : target->startBeat;
            else if (result == 3)
                target->laneSpan = juce::jlimit (1, SampleEvent::numPitchLanes, target->laneSpan + 1);
            else if (result == 4)
                target->laneSpan = juce::jlimit (1, SampleEvent::numPitchLanes, target->laneSpan - 1);
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
//  DragAndDropTarget — the file browser hands us absolute .wav paths
//==============================================================================
bool PianoRollComponent::isInterestedInDragSource (const SourceDetails& details)
{
    const juce::File file (details.description.toString());

    return file.hasFileExtension ("wav;wave");
}

void PianoRollComponent::itemDragEnter (const SourceDetails& details)
{
    sampleDropActive = true;
    sampleDropPoint  = details.localPosition.toFloat();
    repaint();
}

void PianoRollComponent::itemDragMove (const SourceDetails& details)
{
    sampleDropActive = true;
    sampleDropPoint  = details.localPosition.toFloat();
    repaint();
}

void PianoRollComponent::itemDragExit (const SourceDetails& details)
{
    juce::ignoreUnused (details);

    sampleDropActive = false;
    repaint();
}

void PianoRollComponent::itemDropped (const SourceDetails& details)
{
    sampleDropActive = false;

    const auto point = details.localPosition.toFloat();

    if (! getGridArea().contains (point))
    {
        repaint();
        return;
    }

    const juce::File file (details.description.toString());

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

#include "PianoRollComponent.h"

#include "Util/Metrics.h"

#include "Media/SampleLibrary.h"
#include "Util/Text.h"

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
    constexpr float kNoteHandleGap   = 3.0f;

    /** Body left over once both resize bands and the button are subtracted.
        Under this the button is not drawn at all: a note that short has no room
        for a button that is not also a resize edge. */
    constexpr float kNoteHandleMinBody = 6.0f;

    juce::Font boldFont (float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (atomic::ui::sp (height)).withStyle ("Bold"));
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
    auto* layer = const_cast<PianoRollComponent*> (this)->stack.getLayer (layerIndex);

    if (layer == nullptr)
        return {};

    if (auto* note = layer->findNote (noteId))
        return getNoteBounds (*note);

    return {};
}

bool PianoRollComponent::isLayerShown (int layerIndex) const
{
    auto& self = *const_cast<PianoRollComponent*> (this);
    auto* layer = self.stack.getLayer (layerIndex);

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
        dragMode = DragMode::none;
        dragOriginals.clear();
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

    auto* self = const_cast<PianoRollComponent*> (this);

    for (int i = 0; i < self->stack.getNumLayers(); ++i)
    {
        auto* layer = self->stack.getLayer (i);

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

    auto* layer = stack.getLayer (layerIndex);

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

    auto* self = const_cast<PianoRollComponent*> (this);

    for (int i = 0; i < self->stack.getNumLayers(); ++i)
        if (auto* layer = self->stack.getLayer (i))
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
    notifySelectionChanged();
    repaint();
}

void PianoRollComponent::notifySelectionChanged()
{
    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

int PianoRollComponent::getSelectedRoutingSlotId() const
{
    auto* layer = const_cast<PianoRollComponent*> (this)->stack.getActiveLayer();

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
    auto* layer = const_cast<PianoRollComponent*> (this)->stack.getActiveLayer();

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

    for (int id : selection)
        if (auto* note = layer->findNote (id))
            note->outputSlotOverride = slotId < 0 ? -1 : slotId;

    stack.notifyNotesChanged();
    notifySelectionChanged();
    repaint();
}

void PianoRollComponent::toggleSelection (int noteId)
{
    if (isSelected (noteId))
        selection.erase (std::remove (selection.begin(), selection.end(), noteId), selection.end());
    else
        selection.push_back (noteId);

    notifySelectionChanged();
    repaint();
}

//==============================================================================
//  Phase 16: note groups
//==============================================================================
int PianoRollComponent::groupSelection()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return -1;

    // The name/colour cycle follows the number of groups that already exist, so
    // a session reads A, B, C… even after a few have been dissolved.
    const int ordinal = (int) layer->getGroups().size();
    const int groupId = layer->addGroup (groupPalette::defaultName (ordinal),
                                          groupPalette::nextGroupColour (ordinal));

    layer->assignNotesToGroup (selection, groupId);

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
    auto* layer = const_cast<PianoRollComponent*> (this)->stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return -1;

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

    return groupId;
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

        setSelection (layer->getGroupNoteIds (groupId));
        return;
    }
}

bool PianoRollComponent::ungroupSelection()
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || selection.empty())
        return false;

    bool changed = false;

    for (int id : selection)
        if (auto* note = layer->findNote (id); note != nullptr && note->groupId >= 0)
        {
            note->groupId = -1;
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
        const auto* layer = const_cast<PianoRollComponent*> (this)->stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& group : layer->getGroups())
            if (getGroupLabelBounds (i, group.getId()).contains (position))
                return group.getId();
    }

    return -1;
}

bool PianoRollComponent::isGroupFocused (int layerIndex, int groupId) const
{
    if (focusSlotId < 0)
        return false;

    const auto* layer = const_cast<PianoRollComponent*> (this)->stack.getLayer (layerIndex);

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

    return unionBounds;
}

juce::Rectangle<float> PianoRollComponent::getGroupLabelBounds (int layerIndex, int groupId) const
{
    const auto* layer = const_cast<PianoRollComponent*> (this)->stack.getLayer (layerIndex);

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

    const juce::String text = group->getName() + utf8 (" · ")
                                + juce::String (layer->countNotesInGroup (groupId));

    juce::Font font (juce::FontOptions (atomic::ui::sp (9.5f)));

    // Measured with GlyphArrangement rather than Font::getStringWidthFloat —
    // that one is deprecated in JUCE 8 (it cannot account for the fallback that
    // draws CJK) and this project builds warning-free.
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);

    const float width = juce::jlimit (ui::pf (34), ui::pf (150),
                                      glyphs.getBoundingBox (0, -1, true).getWidth() + ui::pf (16));

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

int PianoRollComponent::getFocusedNoteCount() const
{
    if (focusSlotId < 0)
        return 0;

    auto& self = *const_cast<PianoRollComponent*> (this);
    int count = 0;

    for (int i = 0; i < self.stack.getNumLayers(); ++i)
    {
        const auto* layer = self.stack.getLayer (i);

        if (! isLayerShown (i))
            continue;

        for (const auto& note : layer->getNotes())
            if (routing::resolve (*layer, note, self.rack).slotId == focusSlotId)
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

int PianoRollComponent::layerIndexOfGroup (int groupId) const
{
    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (const auto* layer = const_cast<PianoRollComponent*> (this)->stack.getLayer (i);
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
            if (layer->countNotesInGroup (group.getId()) == 0)
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

    juce::PopupMenu menu;
    menu.addSectionHeader (group->getName() + utf8 (" · ")
                             + juce::String (members) + utf8 (" 音符"));
    menu.addItem (1, utf8 ("选中整组"));
    menu.addItem (2, utf8 ("编组挂载音源…"));
    // Phase 20: two parameters, two rows. The tick means "this curve already
    // has a row in the automation window", so the menu doubles as the index.
    menu.addItem (3, utf8 ("创建自动化 · 组音量"), true,
                  group->getLevelAutomation().isEnabled());
    menu.addItem (4, utf8 ("创建自动化 · 组声相"), true,
                  group->getPanAutomation().isEnabled());
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

                                default: break;
                            }
                        });
}

void PianoRollComponent::showCanvasMenu (juce::Point<int> screenPos)
{
    const int count   = (int) selection.size();
    const int groupId = getSelectedGroupId();
    const int ownerLayer = groupId >= 0 ? layerIndexOfGroup (groupId) : -1;

    juce::PopupMenu menu;
    menu.addSectionHeader (count > 0 ? juce::String (count) + utf8 (" 个音符")
                                     : juce::String (utf8 ("钢琴卷帘")));

    menu.addItem (1, utf8 ("编组选中音符"), count > 0);
    menu.addItem (2, utf8 ("选中整组"), groupId >= 0);
    menu.addItem (3, utf8 ("取消编组"), groupId >= 0);
    menu.addSeparator();
    menu.addItem (4, utf8 ("编组挂载音源…"), groupId >= 0);
    menu.addItem (5, utf8 ("创建自动化 · 组音量"), groupId >= 0);
    menu.addItem (6, utf8 ("创建自动化 · 组声相"), groupId >= 0);

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

NoteEvent* PianoRollComponent::hitTestActiveLayer (juce::Point<float> position) const
{
    auto* active = const_cast<PianoRollComponent*> (this)->stack.getActiveLayer();

    if (active == nullptr)
        return nullptr;

    auto& notes = active->getNotes();

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
            if (! selection.empty())
                groupSelection();

            return;
        }

        if (getToolButtonBounds (6).contains (pos))
        {
            setSnapEnabled (! snapEnabled);
            repaint (getToolbarArea());
            return;
        }

        if (getToolButtonBounds (7).contains (pos))
        {
            // Phase 20: the inverse of 编组, next to it. Detaching the selection
            // never moves a note; it only drops the membership, and an emptied
            // group is pruned (which is what tells the host to drop its curve).
            if (! selection.empty())
                ungroupSelection();

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
            dragMode = DragMode::rulerZoom;
            rulerDragAnchorY = e.position.y;
            rulerDragAnchorBeatWidth = beatWidth;
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

    // ---- Phase 8: the inline automation box owns its own clicks ------------
    // Checked before the right-click bail-out, because right-clicking a point
    // in the box deletes it and right-clicking the menu opens the target menu.
    if (expandedNoteId >= 0)
    {
        if (auto* expanded = getExpandedNote())
        {
            if (getNoteAutomationBoxBounds (*expanded).contains (e.position))
            {
                handleAutomationMouseDown (e, *expanded);
                return;
            }
        }
        else
        {
            closeNoteAutomation();       // the note it pointed at is gone
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

        selectGroup (groupId);
        return;
    }

    // ---- Phase 17: an audio clip under the cursor --------------------------
    // Before the notes: a block is the size of several lanes, so it is the
    // thing you are aiming at when both are under the pointer.
    if (auto* clip = hitTestSample (e.position))
    {
        const int clipId = clip->id;

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
                stack.notifyNotesChanged();
            }

            return;
        }

        dragSampleId       = clipId;
        dragSampleOriginal = *clip;
        dragAnchorBeat     = beatAtScreenX (e.position.x);
        dragAnchorPitch    = pitchAtScreenY (e.position.y);

        const auto bounds = getSampleScreenBounds (stack.getActiveLayerIndex(), clipId);

        dragMode = (e.position.x > bounds.getRight() - ui::pf (kResizeHandleWidth)) ? DragMode::resizeSample
                                                                                  : DragMode::moveSample;
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
            captureDragOriginals();
        }

        return;
    }

    // ---- Empty grid: marquee over the ACTIVE layer only ----------------------
    if (! e.mods.isShiftDown())
    {
        selection.clear();
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

void PianoRollComponent::captureDragOriginals()
{
    dragOriginals.clear();

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return;

    for (const auto& note : active->getNotes())
        if (isSelected (note.id))
            dragOriginals.push_back (note);
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
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
        const float dy = rulerDragAnchorY - e.position.y;

        setBeatWidth ((float) (rulerDragAnchorBeatWidth * std::pow (1.006, (double) dy)));
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
            const double rawDelta   = beatAtScreenX (e.position.x) - dragAnchorBeat;
            const int    pitchDelta = pitchAtScreenY (e.position.y) - dragAnchorPitch;

            for (const auto& origin : dragOriginals)
                if (auto* note = active->findNote (origin.id))
                {
                    const double moved = origin.startBeat + rawDelta;
                    note->startBeat = juce::jlimit (0.0, juce::jmax (0.0, timelineBeats - gridDivision),
                                                    snapEnabled ? snapBeat (moved) : moved);
                    note->pitch = juce::jlimit (0, numPitches - 1, origin.pitch + pitchDelta);
                }

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
            const double rawX     = beatAtScreenX (e.position.x);
            const double targetEnd = snapEnabled ? snapBeat (rawX) : rawX;

            for (const auto& origin : dragOriginals)
                if (auto* note = active->findNote (origin.id))
                    note->lengthBeats = juce::jmax (gridDivision, targetEnd - note->startBeat);

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::resizeNoteLeft:
        {
            // The tail stays where it is and the head moves. That is what makes
            // "trim the front" possible without also re-typing the length: the
            // note the user sees end at beat 8 still ends at beat 8 afterwards.
            const double rawX = beatAtScreenX (e.position.x);
            const double wantedStart = snapEnabled ? snapBeat (rawX) : rawX;

            for (const auto& origin : dragOriginals)
                if (auto* note = active->findNote (origin.id))
                {
                    const double end = origin.startBeat + origin.lengthBeats;
                    const double newStart = juce::jlimit (0.0,
                                                          juce::jmax (0.0, end - gridDivision),
                                                          wantedStart);

                    note->startBeat  = newStart;
                    note->lengthBeats = juce::jmax (gridDivision, end - newStart);
                }

            stack.notifyNotesChanged();
            break;
        }

        case DragMode::moveSample:
        case DragMode::resizeSample:
        {
            auto* clip = active->findSample (dragSampleId);

            if (clip == nullptr)
                break;

            if (dragMode == DragMode::moveSample)
            {
                const double rawDelta   = beatAtScreenX (e.position.x) - dragAnchorBeat;
                const int    pitchDelta = pitchAtScreenY (e.position.y) - dragAnchorPitch;

                const double moved = dragSampleOriginal.startBeat + rawDelta;

                clip->startBeat = juce::jlimit (0.0, juce::jmax (0.0, timelineBeats - gridDivision),
                                                snapEnabled ? snapBeat (moved) : moved);

                // The block keeps its integer lane span; only its bottom lane
                // moves, so it can never end up straddling a lane boundary.
                clip->pitch = juce::jlimit (0, numPitches - juce::jmax (1, clip->laneSpan),
                                            dragSampleOriginal.pitch + pitchDelta);
                ensureTimelineExtendsTo (clip->endBeat());
            }
            else
            {
                const double rawX      = beatAtScreenX (e.position.x);
                const double targetEnd = snapEnabled ? snapBeat (rawX) : rawX;

                clip->lengthBeats = juce::jmax (gridDivision, targetEnd - clip->startBeat);
                ensureTimelineExtendsTo (clip->endBeat());
            }

            stack.notifyNotesChanged();
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
    const bool wasRulerDrag = dragMode == DragMode::rulerScrub
                                || dragMode == DragMode::rulerZoom;

    if (dragMode == DragMode::marquee)
    {
        auto marquee = juce::Rectangle<float> (marqueeStart, marqueeCurrent).getSmallestIntegerContainer().toFloat();

        if (marquee.getWidth() > 3.0f || marquee.getHeight() > 3.0f)
        {
            std::vector<int> hits;

            if (auto* active = stack.getActiveLayer())
            {
                for (const auto& note : active->getNotes())
                    if (getNoteBounds (note).intersects (marquee))
                        hits.push_back (note.id);
            }

            if (marqueeStart.getDistanceFrom (marqueeCurrent) > 0.0f)
                setSelection (std::move (hits));
        }
    }

    dragMode = DragMode::none;
    dragOriginals.clear();
    dragEnvelopePoint = -1;
    dragSampleId = -1;

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
         || hoveredGroupId != -1)
    {
        hoveredNoteId = -1;
        hoveredTool = -1;
        hoveredAutomationMenu = false;
        hoveredEnvelopePoint  = -1;
        hoveredGroupId        = -1;
        hoveredGroupLayer     = -1;
        repaint();
    }
}

void PianoRollComponent::setBeatWidth (float pixels)
{
    const float clamped = juce::jlimit (minBeatWidth, maxBeatWidth, pixels);

    if (std::abs (clamped - beatWidth) < 0.01f)
        return;

    // Anchor on the middle of the view rather than the left edge: the cursor is
    // usually near the middle, and pinning beat 0 would throw the music away
    // from it on every notch.
    const auto   grid     = getGridArea();
    const double centreBeat = scrollBeats + (double) (grid.getWidth() * 0.5f) / (double) beatWidth;

    beatWidth   = clamped;
    scrollBeats = centreBeat - (double) (grid.getWidth() * 0.5f) / (double) beatWidth;

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

    if (key.getKeyCode() == 'D' && key.getModifiers().isCommandDown())
    {
        if (! selection.empty())
        {
            duplicateSelectedNotes (1.0);
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
        else if (! key.getModifiers().isAnyModifierKeyDown() && ! selection.empty())
        {
            if (groupSelection() > 0)
                return true;
        }
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (auto* active = stack.getActiveLayer())
        {
            if (! selection.empty())
            {
                for (int id : selection)
                    active->removeNote (id);

                selection.clear();
                hoveredNoteId = -1;
                notifySelectionChanged();
                pruneEmptyGroups();
                stack.notifyNotesChanged();
                return true;
            }
        }
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
        const bool enabled = ! selection.empty();

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

    //-- Phase 18: snap toggle ------------------------------------------------
    // A toggle, not an action: it stays lit for as long as the grid is on, and
    // it switches the AUTOMATION window's grid too. One switch, one answer —
    // two independent snap flags is how a project ends up with notes on the
    // grid and curves half a tick off it.
    {
        const auto bounds  = getToolButtonBounds (6).toFloat();
        const bool hovered = hoveredTool == 6;
        const bool on      = snapEnabled;

        g.setColour (on ? tint.withAlpha (0.22f)
                        : juce::Colours::white.withAlpha (hovered ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (on ? tint.withAlpha (0.85f)
                        : juce::Colours::white.withAlpha (0.20f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

        // Phase 20: a magnet instead of the 3x3 dot grid. "Snap" is a thing
        // every DAW draws as a magnet, and a dot grid reads as "grid view".
        drawToolIcon (g, 6, bounds.withTrimmedLeft (ui::pf (7)).withWidth (ui::pf (18)),
                      on ? juce::Colours::white : kText.withAlpha (0.75f));

        g.setColour (on ? juce::Colours::white : kText.withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("吸附"),
                    bounds.withTrimmedLeft (ui::pf (26)), juce::Justification::centredLeft, false);
    }

    //-- Phase 20: ungroup ----------------------------------------------------
    // Same shape as 编组, dimmed until there is a selection — the pair reads as
    // one control with two directions, which is exactly what it is.
    {
        const auto bounds  = getToolButtonBounds (7).toFloat();
        const bool hovered = hoveredTool == 7;
        const bool enabled = ! selection.empty();
        const bool grouped = getSelectedGroupId() >= 0;

        g.setColour (juce::Colours::white.withAlpha (hovered && enabled ? 0.12f : 0.05f));
        g.fillRoundedRectangle (bounds, ui::pf (5));

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.20f : 0.08f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), ui::pf (5), 1.0f);

        drawToolIcon (g, 7, bounds.withTrimmedLeft (ui::pf (6)).withWidth (ui::pf (18)),
                      kText.withAlpha (enabled ? 1.0f : 0.4f));

        g.setColour (kText.withAlpha (enabled && grouped ? 1.0f : (enabled ? 0.72f : 0.45f)));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (utf8 ("解组"),
                    bounds.withTrimmedLeft (ui::pf (24)), juce::Justification::centredLeft, false);
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

    if (hasPaintState)
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

    // --- Pass 3 (Phase 16): the spotlight --------------------------------
    // When a synth is focused, everything else is veiled and the notes that
    // actually reach it are painted a second time on top. Dimming the rest is
    // what makes "which notes are on this synth" answerable at a glance; the
    // re-draw is a few dozen blocks, not a second full pass.
    if (focusSlotId >= 0)
    {
        g.setColour (juce::Colours::black.withAlpha (0.46f));
        g.fillRect (area);

        for (int i = 0; i < stack.getNumLayers(); ++i)
        {
            auto* layer = stack.getLayer (i);

            if (! isLayerShown (i))
                continue;

            const bool  onActive = (i == activeIndex);
            const float alpha    = onActive ? solidAlpha : ghostAlphaValue;

            for (const auto& note : layer->getNotes())
            {
                if (! getNoteBounds (note).intersects (area))
                    continue;

                if (routing::resolve (*layer, note, rack).slotId != focusSlotId)
                    continue;

                drawNote (g, note, layer->getColour(), alpha,
                          onActive && isSelected (note.id),
                          onActive && note.id == hoveredNoteId,
                          progressOf (note));
            }
        }
    }
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

            // Focused and hovered pills are filled; the rest are outlined so a
            // screen with six groups is not six shouting tabs.
            if (focused || hovered)
            {
                g.setColour (colour.withAlpha (alpha * (focused ? 0.92f : 0.75f)));
                g.fillRoundedRectangle (pill, ui::pf (4));
            }
            else
            {
                g.setColour (colour.withAlpha (alpha * 0.14f));
                g.fillRoundedRectangle (pill, ui::pf (4));
                g.setColour (colour.withAlpha (alpha * 0.55f));
                g.drawRoundedRectangle (pill, ui::pf (4), 1.0f);
            }

            const juce::String text = group.getName() + utf8 (" · ")
                                        + juce::String (layer->countNotesInGroup (group.getId()));

            g.setColour (focused || hovered ? juce::Colour (0xff10141a) : colour.withAlpha (alpha));
            g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.5f))));
            g.drawText (text, pill.reduced (ui::pf (5), 0.0f), juce::Justification::centredLeft, false);

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
                                            .withTrimmedRight (ui::pf (4)),
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

            // A soft halo, then a crisp edge. Two rings rather than a fatter
            // one: a single 4 px outline eats a 14 px note.
            g.setColour (ring.withAlpha (onActive ? 0.20f : 0.12f));
            g.drawRoundedRectangle (bounds.expanded (ui::pf (1.6)), ui::pf (4), ui::pf (2.0));

            g.setColour (ring.withAlpha (onActive ? 0.92f : 0.50f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, ui::pf (1.2));

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

void PianoRollComponent::drawNote (juce::Graphics& g, const NoteEvent& note, juce::Colour colour,
                                   float alpha, bool selected, bool hovered, double progress)
{
    auto bounds = getNoteBounds (note);

    // Velocity modulates brightness, not opacity — opacity is reserved for
    // the ghost / solid distinction and must stay unambiguous.
    const auto tinted = colour.withMultipliedBrightness (0.78f + 0.35f * note.velocity);
    const bool sounding = progress >= 0.0;

    g.setColour (tinted.withAlpha (alpha * 0.88f));
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

    if (selected)
    {
        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.6f);
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

        case 6:     // snap: a small horseshoe magnet, poles up
        {
            // Drawn as a path, not as a filled shape: the U is one stroke of
            // constant width, so it stays a magnet at 16 px instead of turning
            // into a blob. Right leg -> bottom arc -> left leg, because JUCE
            // measures arc angles clockwise from 12 o'clock and the path has to
            // travel the way the stroke expects.
            juce::Path horseshoe;
            horseshoe.startNewSubPath (box.getX() + 12.0f, box.getY() + 3.6f);
            horseshoe.lineTo (box.getX() + 12.0f, box.getY() + 9.2f);
            horseshoe.addCentredArc (box.getX() + 8.0f, box.getY() + 9.2f, 4.0f, 4.0f, 0.0f,
                                     juce::MathConstants<float>::pi * 0.5f,
                                     juce::MathConstants<float>::pi * 1.5f, false);
            horseshoe.lineTo (box.getX() + 4.0f, box.getY() + 3.6f);

            g.strokePath (horseshoe, juce::PathStrokeType (3.0f, juce::PathStrokeType::mitered,
                                                           juce::PathStrokeType::butt));

            // The pole faces are wider than the legs, which is the one detail
            // that separates a magnet from the letter U.
            g.fillRoundedRectangle (juce::Rectangle<float> (box.getX() + 1.5f, box.getY() + 1.5f,
                                                           4.9f, 3.2f), 0.9f);
            g.fillRoundedRectangle (juce::Rectangle<float> (box.getX() + 9.6f, box.getY() + 1.5f,
                                                           4.9f, 3.2f), 0.9f);
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
    repaint();
}

NoteEvent* PianoRollComponent::getExpandedNote() const
{
    auto* self = const_cast<PianoRollComponent*> (this);

    if (auto* layer = self->stack.getLayer (expandedLayerIndex))
        return layer->findNote (expandedNoteId);

    return nullptr;
}

void PianoRollComponent::openNoteAutomation (int layerIndex, int noteId)
{
    expandedLayerIndex = layerIndex;
    expandedNoteId     = noteId;
    dragEnvelopePoint  = -1;
    repaint();
}

juce::Rectangle<float> PianoRollComponent::getNoteAutomationHandleBounds (const NoteEvent& note) const
{
    const auto bounds = getNoteBounds (note);

    const float band  = ui::pf (kResizeHandleWidth);
    const float gap   = ui::pf (kNoteHandleGap);
    const float width = ui::pf (kNoteHandleWidth);

    // Distance from the note's right edge to the button's left edge: the resize
    // band first, then the air gap. The button therefore never covers the edge
    // that lengthens the note.
    const float inset = band + gap + width;

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

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("调制的参数"));

    for (auto target : { NoteModTarget::level, NoteModTarget::pan, NoteModTarget::pitch })
        menu.addItem ((int) target + 1, modTargetLabel (target) + "  ·  " + modTargetHint (target),
                      true, (int) target == current);

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

        if (auto* target = layer->findNote (noteId))
        {
            target->modTarget = (NoteModTarget) (result - 1);

            if (safe->onNoteEdited != nullptr)
                safe->onNoteEdited();

            safe->repaint();
        }
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
    g.setColour (juce::Colour (0xff0b0c10).withAlpha (0.97f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (colour.withAlpha (0.8f));
    g.drawRoundedRectangle (box, 5.0f, 1.2f);

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

        drawModTargetIcon (g, note->modTarget, glyph, juce::Colours::white.withAlpha (0.92f));
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (10.0f)).withStyle ("Bold")));
        g.drawText (modTargetLabel (note->modTarget),
                    menuArea.reduced (6.0f, 0.0f).withTrimmedRight (12.0f),
                    juce::Justification::centredLeft, false);

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
//  Phase 17: audio clips
//==============================================================================
juce::Rectangle<float> PianoRollComponent::getSampleScreenBounds (int layerIndex, int sampleId) const
{
    auto& self = *const_cast<PianoRollComponent*> (this);
    const auto* layer = self.stack.getLayer (layerIndex);

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

SampleEvent* PianoRollComponent::hitTestSample (juce::Point<float> position) const
{
    auto& self = *const_cast<PianoRollComponent*> (this);
    auto* layer = self.stack.getActiveLayer();

    // Same rule as notes: only the active, visible sheet answers the mouse.
    if (layer == nullptr)
        return nullptr;

    const int index = self.stack.getActiveLayerIndex();

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
    auto& self = *const_cast<PianoRollComponent*> (this);
    auto* layer = self.stack.getActiveLayer();

    return layer != nullptr ? layer->getSampleCount() : 0;
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
    // tempo and rounded UP to the grid so the tail is never clipped off.
    const double exact = SampleEvent::beatsForSeconds (loaded.buffer->lengthSeconds(), bpm);
    sample.lengthBeats = juce::jmax (gridDivision,
                                     std::ceil (exact / gridDivision) * gridDivision);

    sample.startBeat = juce::jmax (0.0, snapEnabled ? snapBeat (beat) : beat);

    const int newId = layer->addSample (std::move (sample));

    ensureTimelineExtendsTo (beat + layer->findSample (newId)->lengthBeats);
    stack.notifyNotesChanged();

    return newId;
}

//==============================================================================
void PianoRollComponent::drawSampleBlock (juce::Graphics& g, int layerIndex,
                                          const SampleEvent& sample,
                                          juce::Colour colour, float alpha)
{
    auto rect = getSampleScreenBounds (layerIndex, sample.id);

    if (rect.isEmpty() || rect.getWidth() < 1.0f)
        return;

    // Top lane gets a header strip: the name has to sit somewhere, and putting
    // it inside the first lane keeps the block's footprint honest.
    const float headerH = juce::jmin (ui::pf (13), rect.getHeight() * 0.42f);

    g.setColour (colour.withAlpha (sample.isLoaded() ? 0.32f * alpha : 0.12f * alpha));
    g.fillRoundedRectangle (rect, ui::pf (3));

    g.setColour (colour.withAlpha (sample.isLoaded() ? 0.85f * alpha : 0.30f * alpha));
    g.drawRoundedRectangle (rect.reduced (0.5f), ui::pf (3), 1.0f);

    //-- Waveform -------------------------------------------------------------
    if (sample.isLoaded() && ! sample.audio->peaks.empty() && rect.getWidth() > 6.0f)
    {
        const auto wave = rect.withTrimmedTop (headerH).reduced (ui::pf (2), ui::pf (1));

        if (wave.getHeight() > 3.0f)
        {
            const auto& peaks = sample.audio->peaks;
            const int   buckets = (int) peaks.size();
            const int   columns = juce::jmin ((int) wave.getWidth(), buckets);

            juce::Path upper, lower;
            const float mid = wave.getCentreY();

            for (int i = 0; i < columns; ++i)
            {
                const int   b = (int) ((double) i * buckets / juce::jmax (1, columns));
                const float level = peaks[(size_t) juce::jlimit (0, buckets - 1, b)]
                                      * wave.getHeight() * 0.5f;
                const float x = wave.getX() + (float) i;

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

    //-- Name strip -----------------------------------------------------------
    if (rect.getWidth() > ui::pf (24))
    {
        g.setColour (colour.withAlpha (0.55f * alpha));
        g.fillRect (rect.withHeight (headerH));

        g.setColour (juce::Colours::white.withAlpha ((sample.isLoaded() ? 0.92f : 0.45f) * alpha));
        g.setFont (juce::Font (juce::FontOptions (atomic::ui::sp (9.0f))));
        g.drawText (sample.name,
                    rect.withHeight (headerH).withTrimmedLeft (ui::pf (4))
                        .withTrimmedRight (ui::pf (3)),
                    juce::Justification::centredLeft, true);
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

    if (layer == nullptr || layer->findSample (sampleId) == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (utf8 ("采样块"));

    menu.addItem (1, utf8 ("复制到后一小节"));
    menu.addItem (2, utf8 ("对齐到网格"), true, false);
    menu.addItem (3, utf8 ("加高一行"));
    menu.addItem (4, utf8 ("降低一行"));
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
            owner->removeSample (sampleId);
            safe->stack.notifyNotesChanged();
            safe->repaint();
            return;
        }

        if (result == 1)
        {
            // Copy the whole event: the clip carries its buffer by shared_ptr,
            // so a duplicate costs a pointer, not another decode.
            SampleEvent copy = *source;
            copy.startBeat += juce::jmax (1.0, source->lengthBeats);
            copy.id = 0;
            copy.name = source->name;

            const int newId = owner->addSample (std::move (copy));
            safe->ensureTimelineExtendsTo (owner->findSample (newId)->endBeat());
            safe->stack.notifyNotesChanged();
            safe->repaint();
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

            drawSampleBlock (g, i, sample, layer->getColour(), alpha);
        }
    }
}

} // namespace atomic

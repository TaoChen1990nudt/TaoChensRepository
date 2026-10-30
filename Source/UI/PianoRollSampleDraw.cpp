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
        juce::String label = sampleLabelText (sample);

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

        //-- P92: a target under the pointer means "replace", not "add" ------
        // The preview changes SHAPE, not just wording: a card over a block
        // does the same thing, for the same reason — the gesture is a
        // different verb, and this is the only place the user finds out
        // before letting go.
        if (const auto* victim = layer->findGroup (dropReplaceGroupId))
        {
            const auto box = groupFrameOf (stack.getActiveLayerIndex(), dropReplaceGroupId,
                                           dropReplaceOccurrence);

            if (! box.isEmpty())
            {
                g.setColour (theme::warn.withAlpha (0.18f));
                g.fillRoundedRectangle (box, ui::pf (4));

                g.setColour (theme::warn.withAlpha (0.95f));
                g.drawRoundedRectangle (box.reduced (ui::pf (0.7f)), ui::pf (4), ui::pf (2.0f));
            }

            drawTag (utf8 ("替换 ") + victim->getName() + utf8 (" → ") + group->getName(),
                     theme::warn);
            return;
        }

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
            else if (result == 3 || result == 4)
            {
                // Phase 72: the whole selection, like delete and restore — and
                // every other appearance of their groups, which
                // growSelectedSamples reaches through syncShapeOfClip. Before
                // this the command wrote one block's laneSpan and called the
                // sync by hand: the same edit, but the multi-selection and the
                // "which copies does it reach" rule lived in two places.
                safe->growSelectedSamples (result == 3 ? 1 : -1);
                safe->stack.notifyNotesChanged();
                safe->repaint();
                return;
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

        // P92: a group already on the sheet turns "place a copy" into "take
        // that one's place". Resolved here, in the one function both the
        // preview and the drop run.
        dropReplaceGroupId = getGroupReplaceTargetAt (point, groupLayer, groupId,
                                                      &dropReplaceOccurrence);
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
    dropReplaceGroupId    = -1;      // P92
    dropReplaceOccurrence = -1;
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

    // P92: read before the clear — a drop over a group replaces that
    // appearance, and `groupLayer`/`groupId` name the row that was dragged.
    const int replaceGroup      = dropReplaceGroupId;
    const int replaceOccurrence = dropReplaceOccurrence;

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
        // P92: over a group's own bracket, this row takes that appearance's
        // place; anywhere else it is still "one more copy, starting here".
        if (replaceGroup >= 0 && replaceOccurrence >= 0)
            replaceGroupAppearance (groupLayer, groupId, replaceGroup, replaceOccurrence);
        else
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

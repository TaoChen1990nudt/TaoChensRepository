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
//-- Phase 72: how TALL a block is — the two horizontal edges -----------------
//
// Length was the only thing an edge could change: the head and the tail were
// the only bands, so a block's height was whatever `laneSpan` the drop gave it
// (four lanes) and the menu's 加高一行 / 降低一行 were the only way to change it
// afterwards. The two edges a block already has are enough to make that a
// gesture — the same one a note gets on its tail, turned ninety degrees.

int PianoRollComponent::maxLaneSpanFor (int pitch) const noexcept
{
    // A block sits ON a lane and grows up. 128 lanes of roll, so the ceiling is
    // what is left above the lane it stands on. Never below 1: a block that
    // cannot be one lane tall is not a block.
    return juce::jlimit (1, SampleEvent::numPitchLanes, numPitches - pitch);
}


bool PianoRollComponent::applySampleSpan (const SampleEvent& origin, float wantedY, bool fromTop,
                                          bool propagate)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    auto* clip = active->findSample (origin.id);

    if (clip == nullptr)
        return false;

    // Whole lanes only, and the pointer's lane IS the edge: `laneSpan` is a
    // count of pitch lanes, so a block never straddles a boundary. This is why
    // the drag reads the row under the pointer instead of a pixel delta — the
    // picture the user is aiming at is made of rows.
    const int lane = juce::jlimit (0, numPitches - 1, pitchAtScreenY (wantedY));

    const int bottom = origin.pitch;

    if (fromTop)
    {
        // The bottom lane does not move; the top edge follows the pointer. Push
        // it below the bottom and the block is one lane tall — there is no
        // "negative height", and flipping it to grow downwards mid-drag would
        // make the block jump to the other side of the pointer.
        clip->pitch    = bottom;
        clip->laneSpan = juce::jlimit (1, maxLaneSpanFor (bottom),
                                       juce::jmax (bottom, lane) - bottom + 1);
    }
    else
    {
        // The top lane does not move; the bottom edge follows the pointer. The
        // mirror of the case above: the block cannot get shorter than one lane
        // and cannot turn inside out.
        const int newBottom = juce::jlimit (0, origin.topPitch(), lane);

        clip->pitch    = newBottom;
        clip->laneSpan = origin.topPitch() - newBottom + 1;
    }

    // Phase 62/72: how many lanes a block covers is SHAPE, exactly like its
    // width — so the other appearances of its group get the same height.
    //
    // Phase 75: a selection-wide drag turns this off and syncs once at the end.
    // Syncing per block inside the loop let the first block's copy overwrite the
    // second block's CURRENT height, so the two took turns being the template and
    // the result depended on which one the loop happened to visit last.
    if (propagate)
        syncShapeOfClip (origin.id);

    return true;
}


bool PianoRollComponent::applySampleSpanDrag (const SampleEvent& primary, float wantedY, bool fromTop,
                                              const std::vector<SampleEvent>& siblings)
{
    // Phase 75: the primary is written first but NOT propagated — every block in
    // this step is measured against the shape it had when the button went down,
    // and a sync in the middle of the loop rewrites the ones it has not reached
    // yet. That is the "sometimes" in "sometimes the blocks end up different":
    // whichever the loop visits last becomes the template, and the ones before
    // it are holding a height the template never had.
    if (! applySampleSpan (primary, wantedY, fromTop, false))
        return false;

    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return true;

    const auto* applied = active->findSample (primary.id);

    if (applied == nullptr)
        return true;

    // The APPLIED delta, never the pointer's: the lane limits can eat part of
    // the drag for the block the pointer owns, and a sibling moved by the raw
    // delta would then part company with it. Same arithmetic, and same reason,
    // as applySampleTrimDrag.
    const int delta = fromTop ? applied->topPitch() - primary.topPitch()
                              : applied->pitch - primary.pitch;

    if (delta != 0)
        for (const auto& sibling : siblings)
        {
            if (sibling.id == primary.id)
                continue;

            auto* target = active->findSample (sibling.id);

            if (target == nullptr)
                continue;

            if (fromTop)
                target->laneSpan = juce::jlimit (1, maxLaneSpanFor (sibling.pitch),
                                                 sibling.laneSpan + delta);
            else
            {
                target->pitch    = juce::jlimit (0, sibling.topPitch(), sibling.pitch + delta);
                target->laneSpan = sibling.topPitch() - target->pitch + 1;
            }
        }

    // One sync per group, after every block has its new height. The values agree
    // by then, so it no longer matters which appearance is the source — which is
    // the point: the result must not depend on the order of a loop.
    std::vector<int> touchedIds;
    touchedIds.push_back (primary.id);

    for (const auto& sibling : siblings)
        touchedIds.push_back (sibling.id);

    for (const auto& entry : groupOccurrencesOf ({}, touchedIds))
        active->syncGroupShape (entry.first, entry.second);

    stack.notifyNotesChanged();
    return true;
}


bool PianoRollComponent::simulateSpanDragFromPress (juce::Point<float> pressPos, float wantedY)
{
    // The real order: the press captures both origins, the drag applies one step
    // against those snapshots. Driving it any other way proves nothing about a
    // multi-selection, because a selection is only "several shapes" until
    // something in the layer has already been rewritten by this same gesture.
    handlePress (pressPos, {}, 1);

    const bool fromTop = dragMode == DragMode::resizeSampleTop;

    if (dragMode != DragMode::resizeSampleTop && dragMode != DragMode::resizeSampleBottom)
    {
        dragMode = DragMode::none;
        return false;
    }

    applySampleSpanDrag (dragSampleOriginal, wantedY, fromTop, dragSpanOriginals);
    stack.notifyNotesChanged();

    dragMode = DragMode::none;
    dragOriginals.clear();
    dragClipOriginals.clear();
    dragTrimOriginals.clear();
    dragSpanOriginals.clear();
    dragSampleId = -1;

    return true;
}


bool PianoRollComponent::simulateSampleSpanDrag (int sampleId, float wantedY, bool fromTop)
{
    auto* active = stack.getActiveLayer();

    if (active == nullptr)
        return false;

    const auto* clip = active->findSample (sampleId);

    if (clip == nullptr)
        return false;

    const SampleEvent origin = *clip;

    std::vector<SampleEvent> siblings;

    if (sampleSelection.size() > 1)
        for (int id : sampleSelection)
            if (const auto* other = active->findSample (id))
                siblings.push_back (*other);

    const bool changed = applySampleSpanDrag (origin, wantedY, fromTop, siblings);

    stack.notifyNotesChanged();
    return changed;
}


void PianoRollComponent::growSelectedSamples (int lanes)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || lanes == 0)
        return;

    for (int id : sampleSelection)
    {
        auto* clip = layer->findSample (id);

        if (clip == nullptr)
            continue;

        // 加高 keeps the bottom lane and grows upward, 降低 takes the top lane
        // away. Neither moves the block's place on the timeline. The ceiling is
        // the roll's top lane — without it, 加高 on a block sitting at lane 126
        // pushed laneSpan past 128 and the block was drawn out of the grid.
        clip->laneSpan = juce::jlimit (1, maxLaneSpanFor (clip->pitch),
                                       clip->laneSpan + lanes);
    }

    // Phase 62/72: a height is shape, so every appearance of the group follows.
    for (int id : sampleSelection)
        syncShapeOfClip (id);
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
    std::vector<int> newSelection;
    const int split = splitSampleBlocksRaw (ids, at, newSelection);

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


/** Phase 96: the cut itself, with no selection or notification attached.

    Comping splits a block up to four times inside one gesture, and every one of
    those cuts is an intermediate: the piece that matters is whichever survives
    the trimming. Running the public `splitSamplesAt` for each would move the
    user's selection four times and repaint four times for an edit that has not
    finished yet — so the arithmetic lives here and the public verb keeps its
    own bookkeeping on top. The two paths cut identically because there is only
    one of them. */
int PianoRollComponent::splitSampleBlocksRaw (const std::vector<int>& ids, double at,
                                              std::vector<int>& newSelection)
{
    auto* layer = stack.getActiveLayer();

    if (layer == nullptr || ids.empty() || at < 0.0)
        return 0;

    // No snapping here: the knife snaps (or not) where it reads the click, and
    // the playhead path cuts exactly where the head is. One meaning per layer —
    // a cut function that also moves the cut would be two features in one.
    int split = 0;

    newSelection.clear();

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
            // Phase 82: a split copies the curves that EXIST. ensureCurveDriving
            // is the write door — the read accessors are const on purpose, so a
            // painter cannot accidentally create a curve by looking at it.
            auto& leftVolume = clipCurve->ensureCurveDriving (SampleAutomationTarget::volume);
            auto& leftPan    = clipCurve->ensureCurveDriving (SampleAutomationTarget::pan);
            leftVolume = subEnvelope (leftVolume, 0.0f, seam);
            leftPan    = subEnvelope (leftPan,    0.0f, seam);
            clipCurve->lengthBeats = at - clipCurve->startBeat;
        }

        auto& rightVolume = right.ensureCurveDriving (SampleAutomationTarget::volume);
        auto& rightPan    = right.ensureCurveDriving (SampleAutomationTarget::pan);
        rightVolume = subEnvelope (rightVolume, seam, 1.0f);
        rightPan    = subEnvelope (rightPan,    seam, 1.0f);

        const int newId = layer->addSample (std::move (right));

        newSelection.push_back (id);
        newSelection.push_back (newId);
        ++split;
    }

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
        right.envelope()    = subEnvelope (right.envelope(), (float) u, 1.0f);

        note->envelope()    = subEnvelope (note->envelope(), 0.0f, (float) u);
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


PianoRollComponent::ClipEdge PianoRollComponent::edgeOfClip (juce::Rectangle<float> bounds,
                                                            juce::Point<float> position)
{
    if (bounds.isEmpty()
          || position.y < bounds.getY() || position.y > bounds.getBottom()
          || position.x < bounds.getX() || position.x > bounds.getRight())
        return ClipEdge::none;

    const float band = ui::pf (kResizeHandleWidth);

    // Phase 72: the height bands are narrower than the length bands on a short
    // block. One lane is only keyHeight tall, and two full bands would cover it
    // completely — leaving no pixel that means "move this block".
    const float bandY = juce::jmin (band, bounds.getHeight() * 0.25f);

    const float toTail   = bounds.getRight() - position.x;
    const float toHead   = position.x - bounds.getX();
    const float toTop    = position.y - bounds.getY();
    const float toBottom = bounds.getBottom() - position.y;

    // Nearest edge wins, so a corner pixel means the edge it is drawn on
    // instead of "whichever test ran first". Length edges win ties: length is
    // the edit a block is usually aimed at, and it was here first.
    ClipEdge best     = ClipEdge::none;
    float    bestDist = 1.0e9f;

    if (toTail <= band && toTail < bestDist)
    {
        bestDist = toTail;
        best     = ClipEdge::tail;
    }

    if (toHead <= band && toHead < bestDist)
    {
        bestDist = toHead;
        best     = ClipEdge::head;
    }

    if (toTop <= bandY && toTop < bestDist)
    {
        bestDist = toTop;
        best     = ClipEdge::top;
    }

    if (toBottom <= bandY && toBottom < bestDist)
        best = ClipEdge::bottom;

    return best;
}


std::pair<int, PianoRollComponent::ClipEdge>
PianoRollComponent::clipEdgeAt (juce::Point<float> position) const
{
    const auto* layer = stack.getActiveLayer();

    if (layer == nullptr || sampleSelection.empty())
        return { -1, ClipEdge::none };

    const int   index   = stack.getActiveLayerIndex();
    const auto& samples = layer->getSamples();

    int      bestId   = -1;
    ClipEdge bestEdge = ClipEdge::none;
    float    bestDist = 1.0e9f;

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
        const auto edge = edgeOfClip (b, position);

        if (edge == ClipEdge::none)
            continue;

        // The NEARER edge wins. Two blocks that share a boundary both answer
        // "in band" for the same pixel; picking by distance makes that pixel
        // mean "the edge it is drawn on", not "whichever block came first".
        const float dist = edge == ClipEdge::tail ? b.getRight() - position.x
                         : edge == ClipEdge::head ? position.x - b.getX()
                         : edge == ClipEdge::top  ? position.y - b.getY()
                                                  : b.getBottom() - position.y;

        if (dist >= bestDist)
            continue;

        bestId   = it->id;
        bestEdge = edge;
        bestDist = dist;
    }

    if (bestId >= 0)
        return { bestId, bestEdge };

    //-- Phase 75: a selected block still owns the press from its middle -----
    // No band answered, so this pixel belongs to whichever block is DRAWN here —
    // which is not necessarily one the user has selected. Two blocks stacked on
    // the same beat and lane (a paste on top of itself is the usual way) made
    // the press land on the stranger: the selection was rewritten to it and only
    // that one moved, so a multi-selection "sometimes moved only one block".
    //
    // The answer is (id, none): the block is grabbed, no edge is. `handlePress`
    // reads that as "keep the selection, arm a move", which is what the user
    // meant by pressing the thing they had already selected.
    for (auto it = samples.rbegin(); it != samples.rend(); ++it)
    {
        if (! isSampleSelected (it->id))
            continue;

        if (getSampleScreenBounds (index, it->id).contains (position))
            return { it->id, ClipEdge::none };
    }

    return { -1, ClipEdge::none };
}


std::pair<int, bool> PianoRollComponent::selectedTrimEdgeAt (juce::Point<float> position) const
{
    // Phase 72: one hit test, four answers. This caller only cares about the
    // two that change the LENGTH, so it filters — but it asks the same
    // function the height drag asks, so a corner pixel cannot be "head" for
    // one and "top" for the other.
    const auto edge = clipEdgeAt (position);

    if (edge.second != ClipEdge::tail && edge.second != ClipEdge::head)
        return { -1, false };

    return { edge.first, edge.second == ClipEdge::tail };
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


std::pair<int, int> PianoRollComponent::getBlockEdgeAtForTest (juce::Point<float> at) const
{
    const auto edge = clipEdgeAt (at);

    return { edge.first, (int) edge.second };
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
} // namespace atomic

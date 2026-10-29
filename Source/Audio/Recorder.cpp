#include "Recorder.h"

#include <algorithm>
#include <cmath>

namespace atomic
{

namespace
{
    /** A tap shorter than this is still a note the user played. One 64th is the
        finest grid the roll has (P69), so anything below it is unclickable —
        but the MODEL still has no lower bound; this is what a take produces. */
    constexpr double minRecordedNoteBeats = 1.0 / 64.0;

    /** A recorded CC is kept when the knob moved by this much... */
    constexpr float controlValueStep = 0.01f;

    /** ...or when this much time passed since the last kept point. Without the
        second test a slow sweep (0.2 over four bars) would thin down to two
        points because no single event moves the value by 1%. */
    constexpr double controlTimeStep = 0.5;
}

//==============================================================================
RecordTarget recordTargetFor (int controller) noexcept
{
    if (controller == 7 || controller == 11)     // channel volume / expression
        return RecordTarget::level;

    if (controller == 10)                        // pan
        return RecordTarget::pan;

    return RecordTarget::none;
}

//==============================================================================
void TakeCollector::begin (double startBeatToUse)
{
    held.clear();
    take = {};
    take.startBeat = startBeatToUse;
    take.endBeat   = startBeatToUse;
    startBeat      = startBeatToUse;
    active         = true;
}

int TakeCollector::findHeld (int pitch) const noexcept
{
    for (int i = (int) held.size() - 1; i >= 0; --i)
        if (held[(size_t) i].pitch == pitch)
            return i;

    return -1;
}

void TakeCollector::appendNote (int pitch, float velocity, double noteStart, double noteEnd)
{
    RecordedNote note;
    note.pitch     = juce::jlimit (0, 127, pitch);
    note.velocity  = juce::jlimit (0.0f, 1.0f, velocity);
    note.startBeat = juce::jmax (0.0, noteStart);
    note.endBeat   = juce::jmax (note.startBeat + minRecordedNoteBeats, noteEnd);

    // Recording order is chronological, and a note can only start after the one
    // before it — no sort needed, and no stable_sort over a vector that arrives
    // one event at a time.
    take.notes.push_back (note);
}

void TakeCollector::noteOn (int pitch, float velocity, double beat)
{
    if (! active)
        return;

    // Re-trigger without an off: close the note that was already sounding at
    // this instant rather than leaving it open to the end of the take.
    if (const int existing = findHeld (pitch); existing >= 0)
    {
        appendNote (pitch, held[(size_t) existing].velocity,
                    held[(size_t) existing].startBeat, beat);
        held.erase (held.begin() + existing);
    }

    held.push_back ({ pitch, juce::jlimit (0.0f, 1.0f, velocity), juce::jmax (0.0, beat) });
}

void TakeCollector::noteOff (int pitch, double beat)
{
    if (! active)
        return;

    const int index = findHeld (pitch);

    // An off with no on — the key was already down when the record button was
    // pressed. Nothing to close, and inventing a note for it would put a
    // zero-length blob at the record start.
    if (index < 0)
        return;

    appendNote (pitch, held[(size_t) index].velocity, held[(size_t) index].startBeat, beat);
    held.erase (held.begin() + index);
}

void TakeCollector::control (int controller, int value7, double beat)
{
    if (! active)
        return;

    if (recordTargetFor (controller) == RecordTarget::none)
    {
        ++take.ignoredControls;
        return;
    }

    take.controls.push_back ({ controller,
                               juce::jlimit (0.0f, 1.0f, (float) value7 / 127.0f),
                               juce::jmax (0.0, beat) });
}

Take TakeCollector::finish (double endBeat)
{
    const double stop = juce::jmax (startBeat, endBeat);

    // Anything still held ends with the transport. The alternative — leaving it
    // open — makes the last chord exactly as long as whatever the user did
    // after pressing stop.
    for (const auto& h : held)
        appendNote (h.pitch, h.velocity, h.startBeat, stop);

    held.clear();

    take.endBeat = juce::jmax (stop, take.startBeat);
    active       = false;

    Take finished = std::move (take);
    take = {};
    return finished;
}

//==============================================================================
std::vector<AutomationPoint> decimateRecordedCurve (const std::vector<RecordedControl>& controls,
                                                    RecordTarget target,
                                                    int limit)
{
    std::vector<AutomationPoint> result;

    if (target == RecordTarget::none || limit < 2)
        return result;

    std::vector<RecordedControl> picked;
    picked.reserve (controls.size());

    for (const auto& c : controls)
        if (recordTargetFor (c.controller) == target)
            picked.push_back (c);

    // One point is not a curve, and loadPoints() would repair it into a flat
    // line across the whole project — a take where the user nudged a knob once
    // would silently rewrite the group's level everywhere.
    if (picked.size() < 2)
        return result;

    std::stable_sort (picked.begin(), picked.end(),
                      [] (const RecordedControl& a, const RecordedControl& b) { return a.beat < b.beat; });

    auto push = [&result] (const RecordedControl& c, float value)
    {
        // Two points on the same beat describe a jump; the later one is where
        // the knob ended up, which is the same rule setPoints() uses for tempo.
        if (! result.empty() && juce::approximatelyEqual (result.back().beat, c.beat))
        {
            result.back().value = juce::jlimit (0.0f, 1.0f, value);
            return;
        }

        result.push_back ({ c.beat, juce::jlimit (0.0f, 1.0f, value) });
    };

    push (picked.front(), picked.front().value);

    double lastBeat  = picked.front().beat;
    float  lastValue = picked.front().value;

    for (size_t i = 1; i + 1 < picked.size(); ++i)
    {
        const bool moved = std::abs (picked[i].value - lastValue) >= controlValueStep;
        const bool aged  = picked[i].beat - lastBeat >= controlTimeStep;

        if (! moved && ! aged)
            continue;

        push (picked[i], picked[i].value);
        lastBeat  = picked[i].beat;
        lastValue = picked[i].value;
    }

    push (picked.back(), picked.back().value);

    if ((int) result.size() <= limit)
        return result;

    // Still too many: keep an even sample of what survived. First and last are
    // pinned because a curve is read from its ends inward.
    std::vector<AutomationPoint> thinned;
    thinned.reserve ((size_t) limit);

    for (int i = 0; i < limit; ++i)
    {
        const double pos = (double) i * (double) (result.size() - 1) / (double) (limit - 1);
        thinned.push_back (result[(size_t) std::llround (pos)]);
    }

    return thinned;
}

//==============================================================================
void RecordBuffer::prepare (double sampleRateToUse, int numChannels, double maxSeconds)
{
    sampleRate = sampleRateToUse > 0.0 ? sampleRateToUse : 44100.0;
    channels   = juce::jmax (0, numChannels);
    armed.store (false, std::memory_order_release);
    writePos.store (0, std::memory_order_release);
    frozenSamples = 0;

    if (channels <= 0 || maxSeconds <= 0.0)
    {
        storage.setSize (0, 0);
        return;
    }

    const int capacity = (int) juce::jlimit (1.0, (double) (1 << 24), maxSeconds * sampleRate);
    storage.setSize (channels, capacity);
    storage.clear();
}

void RecordBuffer::begin()
{
    if (channels <= 0 || storage.getNumSamples() <= 0)
        return;

    writePos.store (0, std::memory_order_release);
    frozenSamples = 0;
    armed.store (true, std::memory_order_release);
}

void RecordBuffer::finish()
{
    // Snapshot BEFORE disarming: a block that is still being written has not
    // stored its new position yet, so it falls outside the take. Disarming
    // first and reading afterwards would hand back a block that is half-written.
    frozenSamples = writePos.load (std::memory_order_acquire);
    armed.store (false, std::memory_order_release);
}

void RecordBuffer::append (const float* const* inputs, int numInputs, int numSamples) noexcept
{
    if (! armed.load (std::memory_order_acquire) || inputs == nullptr || numSamples <= 0)
        return;

    const int capacity = storage.getNumSamples();
    const int start    = writePos.load (std::memory_order_relaxed);

    if (start >= capacity)
    {
        // Full: stop rather than wrap. Losing the tail reads as "it stopped";
        // overwriting the head would destroy the take from the front.
        armed.store (false, std::memory_order_release);
        return;
    }

    const int count = juce::jmin (numSamples, capacity - start);

    if (numInputs <= 0)
    {
        // An input-less device still advances the tape, so the take stays
        // aligned with the transport instead of starting wherever sound first
        // appeared.
        for (int ch = 0; ch < channels; ++ch)
            storage.clear (ch, start, count);

        writePos.store (start + count, std::memory_order_release);
        return;
    }

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* source = inputs[juce::jmin (ch, numInputs - 1)];

        if (source == nullptr)
            storage.clear (ch, start, count);
        else
            storage.copyFrom (ch, start, source, count);
    }

    writePos.store (start + count, std::memory_order_release);
}

juce::AudioBuffer<float> RecordBuffer::takeAudio() const
{
    const int count = getNumSamples();

    if (count <= 0 || channels <= 0 || storage.getNumSamples() < count)
        return {};

    juce::AudioBuffer<float> out (channels, count);

    for (int ch = 0; ch < channels; ++ch)
        out.copyFrom (ch, 0, storage, ch, 0, count);

    return out;
}

} // namespace atomic

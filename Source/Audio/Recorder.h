#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Model/AutomationLane.h"
#include "Util/Text.h"

namespace atomic
{

//==============================================================================
/** Phase 81: which of the group's lanes a recorded controller drives.

    "Record automation" is a sentence about a *knob*, not about a note: the
    thing you record while playing is what your hand did to a controller. The
    three controls picked here are the ones a MIDI keyboard actually has and the
    ones a group has a lane for:

      * CC 7  (channel volume) and CC 11 (expression) -> the group's level lane.
        Both are 0..127 multipliers, and the level lane is a 0..1 multiplier, so
        the mapping is a division and nothing else.
      * CC 10 (pan) -> the group's pan lane. 64 is the centre, which is exactly
        0.5 in the lane's own units.

    Everything else is counted and dropped rather than guessed at: a mod wheel
    does not obviously mean "volume", and inventing a mapping the user did not
    ask for is how a recording comes back sounding wrong for no visible reason.
*/
enum class RecordTarget
{
    none,
    level,
    pan
};

RecordTarget recordTargetFor (int controller) noexcept;

//==============================================================================
/** One note as it came in, in absolute project beats.

    Velocity is kept as played — it belongs to the note (NoteEvent::velocity) and
    is not a volume curve. Writing it twice would attenuate those notes twice. */
struct RecordedNote
{
    int    pitch      = 60;
    float  velocity   = 0.8f;
    double startBeat  = 0.0;
    double endBeat    = 0.0;
};

struct RecordedControl
{
    int    controller = 7;
    float  value      = 1.0f;      // already 0..1
    double beat       = 0.0;
};

/** Everything one pass of the record button produced. */
struct Take
{
    double startBeat = 0.0;
    double endBeat   = 0.0;

    std::vector<RecordedNote>    notes;
    std::vector<RecordedControl> controls;
    int                          ignoredControls = 0;

    bool isEmpty() const noexcept { return notes.empty(); }

    int getNumControlsFor (RecordTarget target) const noexcept
    {
        int count = 0;

        for (const auto& c : controls)
            if (recordTargetFor (c.controller) == target)
                ++count;

        return count;
    }

    double lengthBeats() const noexcept { return juce::jmax (0.0, endBeat - startBeat); }
};

//==============================================================================
/**
    Collects what arrives while the record button is lit.

    Message thread only. MIDI callbacks run on the MIDI thread, so the host
    captures the beat position there (one atomic read) and hops over here — the
    timestamp is taken on the MIDI thread, so the message-thread delay never
    becomes timing error.

    Two decisions worth knowing:

      * **Same pitch twice without a note-off closes the first note.** A keyboard
        that sends a re-trigger instead of off-then-on is common, and leaving the
        first note open would stretch it to the end of the take.
      * **A note still held when the transport stops ends there.** Otherwise a
        take's last chord is as long as whatever the user did afterwards.
*/
class TakeCollector
{
public:
    void begin (double startBeat);

    void noteOn  (int pitch, float velocity, double beat);
    void noteOff (int pitch, double beat);
    void control (int controller, int value7, double beat);

    /** Closes any held notes, hands back the take and clears the collector. */
    Take finish (double endBeat);

    bool   isActive() const noexcept   { return active; }
    double getStartBeat() const noexcept { return startBeat; }
    int    getNumHeld() const noexcept { return (int) held.size(); }
    int    getNumNotes() const noexcept { return (int) take.notes.size(); }

private:
    struct Held
    {
        int    pitch     = 60;
        float  velocity  = 0.8f;
        double startBeat = 0.0;
    };

    int findHeld (int pitch) const noexcept;

    void appendNote (int pitch, float velocity, double startBeat, double endBeat);

    std::vector<Held> held;
    Take take;
    bool   active     = false;
    double startBeat  = 0.0;
};

//==============================================================================
/** A lane holds 32 points; a CC sweep can produce hundreds.

    Kept one under the ceiling so a later edit still has room to add a point.
    Two passes: drop points that barely moved (a knob at rest floods the lane),
    then, if it is still too long, keep an even sample of what is left. First and
    last survive both passes — the start and the end of the gesture are the two
    points a curve is read by.
*/
inline constexpr int maxRecordedCurvePoints = 30;

std::vector<AutomationPoint> decimateRecordedCurve (const std::vector<RecordedControl>& controls,
                                                   RecordTarget target,
                                                   int limit = maxRecordedCurvePoints);

//==============================================================================
/**
    The audio side of recording: a preallocated buffer the audio callback writes
    into, with no locking and no allocation.

    `append` runs on the audio thread and only touches a preallocated
    AudioBuffer plus two atomics. When the buffer is full it stops itself rather
    than overwriting the beginning — losing the tail of a very long take is
    legible ("it stopped"), losing the head is not.

    `finish` runs on the message thread and reads back exactly
    `getNumSamples()`, which is the position the audio thread had reached when
    recording was switched off. A callback that is mid-write at that moment is
    writing *past* that point, so the take is never torn.
*/
class RecordBuffer
{
public:
    /** Message thread, before begin(). Channels below 1 record nothing. */
    void prepare (double sampleRateToUse, int numChannels, double maxSeconds);

    /** Arms the tape.

        `samplesToSkip` (Phase 88) drops that many input samples from the head
        without ever writing them. A count-in arms the recorder at the moment the
        user hits the button and then must not keep the four beats of click — and
        of the player's own warm-up — that happen before the take's first beat.
        Dropping them here is sample-exact; starting the recorder when the
        count-in ends would be a timer tick late, and the take would begin
        slightly after its own downbeat. */
    void begin (int samplesToSkip = 0);
    void finish();

    /** Audio thread. `inputs` may be null when the device has no inputs. */
    void append (const float* const* inputs, int numInputs, int numSamples) noexcept;

    bool isRecording() const noexcept { return armed.load (std::memory_order_acquire); }

    /** While recording: how far the tape has advanced. After finish(): the
        length that was frozen then — which is the only answer the caller wants,
        because a callback that was mid-block has written past it. */
    int getNumSamples() const noexcept
    {
        return armed.load (std::memory_order_acquire) ? writePos.load (std::memory_order_acquire)
                                                      : frozenSamples;
    }
    double getSampleRate() const noexcept { return sampleRate; }
    int    getNumChannels() const noexcept { return channels; }
    double getSeconds() const noexcept
    {
        return sampleRate > 0.0 ? getNumSamples() / sampleRate : 0.0;
    }

    /** Message thread, after finish(). Null when nothing was captured. */
    juce::AudioBuffer<float> takeAudio() const;

private:
    juce::AudioBuffer<float> storage;
    std::atomic<int>  writePos { 0 };
    std::atomic<bool> armed { false };

    /** Phase 88: input samples still to be thrown away before the take starts.
        Counted down on the audio thread, one block at a time. */
    std::atomic<int>  skipRemaining { 0 };

    double sampleRate = 0.0;
    int    channels   = 0;

    /** Set by finish() on the message thread, read by getNumSamples() there.
        Not atomic on purpose: the message thread is the only reader, and making
        it atomic would hide the fact that this is a hand-off, not a shared
        variable. */
    int frozenSamples = 0;

    JUCE_LEAK_DETECTOR (RecordBuffer)
};

} // namespace atomic

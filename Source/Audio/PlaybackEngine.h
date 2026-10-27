#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "Model/AutomationLane.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/MixerModel.h"

#include "Effects.h"
#include "ClipStretch.h"

namespace juce { class AudioProcessor; }

namespace atomic
{

class PluginHost;

//==============================================================================
/**
    One note, frozen into a form the audio thread can render without touching
    the model.

    Note what is copied in: the envelope, the waveform, the velocity. The audio
    thread never reads a Layer, a NoteEvent or a RackModel. If it did, it would
    be racing the message thread every time you dragged a note.

    Note what is NOT here any more (Phase 5): the fader gain. A note carries its
    own velocity; the layer fader and the synth fader belong to buses that a
    note merely passes through, and buses are evaluated once per block so a
    fader move is smooth instead of stepping per note.
*/
struct ScheduledNote
{
    int    slotId       = -1;
    int    pitch        = 60;
    int    layerIndex   = -1;
    int    noteId       = -1;
    double startSeconds = 0.0;
    double endSeconds   = 0.0;
    float  gain         = 0.0f;        // velocity only
    Waveform waveform   = Waveform::saw;
    NoteEnvelope envelope;
    bool   overridden   = false;       // reached this synth by bypassing its layer

    /** Phase 16: which note group this came from, or -1.

        The group's level curve is NOT copied in per note: the schedule holds one
        `GroupMix` per group and the voice stage looks it up by id, so a group of
        two hundred notes does not carry two hundred copies of the same curve. */
    int    groupId      = -1;

    /** Phase 8: what the envelope drives, and — when it drives pitch — the
        integral it needs.

        A pitch curve cannot be evaluated per sample the obvious way: phase is
        `f * elapsed`, so scaling `f` by a moving curve multiplies the *growing*
        elapsed time by a changing factor and the note turns into a siren. What
        is needed is ∫(f(u) - f) dt, which for a normalised curve is
        `f * length * ∫(2^(env(u)-1) - 1) du`. That inner integral depends only
        on u, so it is tabulated once at publish time and linearly interpolated
        per sample. Stateless, cheap, and the offline render walks the exact
        same code as the live one. */
    NoteModTarget      modTarget = NoteModTarget::level;
    std::vector<float> pitchIntegral;

    /** 0..1 at 1/64 steps: [0] is the note head, [64] the tail. */
    static constexpr int pitchIntegralSteps = 64;

    float pitchIntegralAt (float u) const noexcept
    {
        if (pitchIntegral.empty())
            return 0.0f;

        const float scaled = juce::jlimit (0.0f, 1.0f, u) * (float) pitchIntegralSteps;
        const int   i0     = juce::jmin (pitchIntegralSteps - 1, (int) scaled);
        const float frac   = scaled - (float) i0;

        return pitchIntegral[(size_t) i0] * (1.0f - frac) + pitchIntegral[(size_t) i0 + 1] * frac;
    }

    double lengthSeconds() const noexcept { return endSeconds - startSeconds; }
};

//==============================================================================
/** A layer's channel strip, frozen at publish time.

    `audible` already folds in mute and the "somebody else is soloed" rule. The
    audio thread never sees a solo flag, so it cannot invent a second, subtly
    different interpretation of the same button.

    Volume automation MULTIPLIES the fader; pan automation OFFSETS it. Stated
    once here because both the mixer maths and the panel readout depend on it.

    Phase 7: the volume and pan curves are separate members. Both can be live at
    the same time — that is the point of listing them as two automation tracks.
*/
struct LayerMix
{
    int   layerIndex  = -1;
    bool  audible     = true;
    float gain        = 1.0f;          // linear fader gain (mute NOT included)
    float pan         = 0.0f;          // -1..+1

    bool volumeAutomated = false;
    bool panAutomated    = false;
    std::vector<AutomationPoint> volumePoints, panPoints;

    float gainAt (double beat) const noexcept
    {
        const float fader = audible ? gain : 0.0f;

        if (! volumeAutomated)
            return fader;

        return fader * automationValueAt (volumePoints, beat);
    }

    float panAt (double beat) const noexcept
    {
        float result = pan;

        if (panAutomated)
            result += automationValueAt (panPoints, beat) * 2.0f - 1.0f;

        return juce::jlimit (-1.0f, 1.0f, result);
    }
};

//==============================================================================
/**
    One console channel, frozen at publish time.

    Phase 11: a channel is a BUS that one or more synths feed, not "the layer
    with the same index". Its eight inserts therefore process the summed output
    of whatever instruments are routed here — which is what a delay on channel 3
    is supposed to mean: one delay, one memory, however many synths arrive.

    A channel with no source is never processed at all, so an insert that has
    been set up but not routed to does not colour anything.
*/
struct ChannelMix
{
    int channel = -1;
    std::array<fx::Settings, fx::ChainState::maxSlots> inserts;

    /** Phase 23: the strip this channel's post-insert signal is summed into.
        `MixerModel::masterChannel` means "straight to the master", which is
        what every channel did before this field existed. */
    int output = MixerModel::masterChannel;

    /** Phase 25: mono monitoring. Applied after the inserts and before the
        sum, so the effect chain still works on the stereo signal it was set up
        for — this is a listen control, not a mix control. */
    bool mono = false;
};

//==============================================================================
/** A synth's output stage, frozen at publish time. Its filter is the one synth
    parameter macro automation can drive in this phase. */
struct SlotMix
{
    int   slotId  = -1;

    /** Non-zero: a hosted VST3 makes this synth's sound, so the per-voice
        oscillator stage must leave its notes alone. */
    int   pluginId = 0;

    /** Phase 27: a slot that holds one file instead of an oscillator. It renders
        clips and nothing else — the voice stage skips it, for the same reason
        it skips a hosted plugin: two sources for one slot would double it. */
    bool  sampler = false;

    /** -1 = straight into the master strip, skipping every console channel. */
    int   mixerChannel = -1;

    /** Phase 43: the two flags `audible` is made of, kept separately because
        mute and solo are now automatable and the audio thread has to be able to
        re-ask "is this slot audible AT THIS BEAT". `audible` stays as the
        publish-time answer, which is what the live/audition path uses. */
    bool  muted   = false;
    bool  soloed  = false;
    bool  audible = true;

    float gain    = 1.0f;
    float pan     = 0.0f;

    /** Phase 19: the oscillator shape this synth produces.

        Added for the audition voice: when a note is drawn or dragged the engine
        has to sound it immediately, and the only description of "what this
        synth sounds like" it is allowed to read is the frozen schedule. */
    Waveform waveform = Waveform::saw;

    bool filterAutomated = false;
    bool levelAutomated  = false;
    std::vector<AutomationPoint> filterPoints, levelPoints;

    //-- Phase 43: pan / mute / solo curves --------------------------------
    bool panAutomated  = false;
    bool muteAutomated = false;
    bool soloAutomated = false;
    std::vector<AutomationPoint> panPoints, mutePoints, soloPoints;

    /** Mute is a GATE, not a level: the curve is read as a switch at 0.5, so a
        mute that is "nearly off" is still off. Mute automation that faded would
        be a second volume lane with a worse name. */
    bool isMutedAt (double beat) const noexcept
    {
        return muted || (muteAutomated && automationValueAt (mutePoints, beat) < 0.5f);
    }

    bool isSoloedAt (double beat) const noexcept
    {
        return soloed || (soloAutomated && automationValueAt (soloPoints, beat) >= 0.5f);
    }

    /** Audible at this beat, given whether ANY slot is soloed at this beat.

        `anySoloNow` is computed once per block by the renderer: an automated
        solo makes "who is soloed" a function of time, and publish time cannot
        answer a question that changes while the block plays. Before Phase 43
        this was `audible`, a constant — the same number, but only true when no
        solo curve exists. */
    bool isAudibleAt (double beat, bool anySoloNow) const noexcept
    {
        return ! isMutedAt (beat) && (! anySoloNow || isSoloedAt (beat));
    }

    /** Pan is ADDED, exactly like a layer's and a group's: a 0..1 curve means
        "-1..+1 offset", so a curve sitting at the middle changes nothing. */
    float panAt (double beat) const noexcept
    {
        float result = pan;

        if (panAutomated)
            result += automationValueAt (panPoints, beat) * 2.0f - 1.0f;

        return juce::jlimit (-1.0f, 1.0f, result);
    }

    /** The synth's own level at this beat: fader × mute gate × level curve.

        `audible` is deliberately NOT in here any more. It folds in the static
        solo resolution, and solo is now a per-beat question — the renderer asks
        `isAudibleAt` with the block's answer, so letting this function also
        consult a publish-time boolean would silence the two of them arguing. */
    float gainAt (double beat) const noexcept
    {
        const float gate  = isMutedAt (beat) ? 0.0f : 1.0f;
        const float curve = levelAutomated ? automationValueAt (levelPoints, beat) : 1.0f;

        return gain * gate * curve;
    }

    /** 0..1 "how open the filter is". Only meaningful when the filter lane is
        live; the caller checks that flag first. */
    float filterOpennessAt (double beat) const noexcept
    {
        return juce::jlimit (0.0f, 1.0f, automationValueAt (filterPoints, beat));
    }
};

//==============================================================================
/** Phase 16: a note group's macro curve, frozen at publish time.

    `gainAt()` is a plain multiplier on the member notes' level — a group is a
    trim over its notes, not a bus of its own. That is why it lives in the voice
    stage, next to the layer fader, and why a group does not appear in the
    console: you hear it in the notes, you do not route it.
*/
struct GroupMix
{
    int  groupId = -1;
    bool levelAutomated = false;
    bool panAutomated   = false;
    std::vector<AutomationPoint> levelPoints, panPoints;

    /** Phase 17: the group's own fader and pan, frozen.

        They land in exactly the same arithmetic as the layer's: the fader
        MULTIPLIES the curve and the pan ADDS to the angle. A group is still not
        a bus — no channel, no inserts, no meter — so "volume" here means "trim
        the notes that belong to me", which is what the group list is for.

        Phase 35 completes the sentence: "me" now includes the group's SAMPLE
        blocks. A clip names a group the same way a note does, and it looks the
        group up in this same list, so a fader, a curve, a pan and a mute mean
        the same thing on both kinds. What a group still never decides is
        ROUTING for a clip — which sampler plays it stays resolveSample's
        business, because a block's audio must not change because somebody
        re-mounted a group. */
    float gain = 1.0f;
    float pan  = 0.0f;

    /** Phase 35: mute, folded in here rather than checked by every caller.
        One flag, one place: a note and a clip can never disagree about whether
        a muted group is silent. */
    bool  muted = false;

    float gainAt (double beat) const noexcept
    {
        if (muted)
            return 0.0f;

        const float curve = levelAutomated ? automationValueAt (levelPoints, beat) : 1.0f;

        return gain * curve;
    }

    /** Phase 20: pan is ADDED, exactly like the layer's — a 0..1 curve means
        "-1..+1 offset", so a curve sitting at the middle changes nothing and
        the group trim still works on its own. */
    float panAt (double beat) const noexcept
    {
        float result = pan;

        if (panAutomated)
            result += automationValueAt (panPoints, beat) * 2.0f - 1.0f;

        return juce::jlimit (-1.0f, 1.0f, result);
    }
};

//==============================================================================
/** Phase 17: one audio clip, frozen for the audio thread.

    Holds a shared_ptr to the decoded buffer rather than a path: the renderer
    must never touch the file system, and the buffer is immutable, so the
    pointer being shared is the whole thread-safety story.

    Position is stored in seconds like every other scheduled event, because that
    is what the render loop walks; the beat rectangle stays in the model. */
struct ScheduledClip
{
    int    slotId     = -1;
    int    layerIndex = -1;
    int    clipId     = -1;
    double startSeconds = 0.0;
    double endSeconds   = 0.0;
    float  gain         = 1.0f;     // clip trim only; layer and synth apply at the bus

    /** Phase 35: which note group this block belongs to, or -1.

        The same field, the same meaning, the same lookup as ScheduledNote: the
        clip's fader, pan and mute come from the group's frozen `GroupMix`. It
        says NOTHING about routing — `slotId` above is the only answer to "what
        plays this", and it was resolved without looking at the group. */
    int    groupId      = -1;

    //-- Phase 29: where in the file ------------------------------------------
    /** Seconds into the source at `startSeconds`. The block is a window: after
        a head trim the audio must pick up where it left off, not restart. */
    double sourceOffsetSeconds = 0.0;

    //-- Phase 49: the block's own volume curve -------------------------------
    /** The clip's normalised curve, converted to SECONDS at publish time.

        The render loop measures `elapsed` in seconds, so the curve arrives in
        seconds too — x counts from the block's start, y is the 0..1 gain
        multiplier. Same trade as the fades this replaces (and the pitch
        integral): pay the conversion once on the message thread, keep the
        audio thread to multiply-add only. Disabled means unity, which the
        flag says in one branch instead of a curve lookup. */
    bool                        gainAutomated = false;
    std::vector<AutomationPoint> gainCurve;    // x = seconds since clip start

    //-- Phase 31: what a length change does to the sound --------------------
    /** Two ratios, frozen. The audio thread must not compute a log, a division
        or a lookup per sample, so the clip's mode and its natural/drawn ratio
        become `speed` and `pitch` once, here. `none` and a 1:1 block leave both
        at 1.0, which is the identity the Phase 30 renderer already was. */
    clipstretch::Playback stretch;

    std::shared_ptr<const SampleBuffer> audio;

    double lengthSeconds() const noexcept { return endSeconds - startSeconds; }
};

//==============================================================================
/** Phase 24: one hosted-instrument parameter driven by a curve, frozen.

    The two sources of such a curve are different in every other respect (a note
    envelope is normalised over the note, a group lane is absolute project
    time) but identical once published: absolute beats, 0..1 values, and a
    window over which the curve is the authority. Doing the conversion HERE
    means the audio thread needs one evaluator, not two.

    Phase 36: `priority` no longer decides a winner — it is the ORDER the
    curves are applied in, and on the same parameter they are multiplied
    together (see automation::stackPluginParam). Two curves both at 0.5 give
    0.25, which is the same arithmetic the level stack uses.

    The field survives because "which curve is the finer statement" is still
    useful information when reading a schedule, and because a future mixer-style
    parameter (an additive one) would need it back. Nothing depends on it for
    correctness any more. */
struct PluginParamCurve
{
    int pluginId    = 0;      // PluginHost instance id, not a slot id
    int paramIndex  = -1;     // index into AudioProcessor::getParameters()
    double startBeat = 0.0;   // window over which this curve writes
    double endBeat   = 0.0;
    int    priority  = 0;
    std::vector<AutomationPoint> points;   // absolute beats, 0..1 values
};

//==============================================================================
struct PublishedSchedule
{
    std::vector<ScheduledNote> notes;

    /** Phase 17: audio clips, sorted by start time like the notes. */
    std::vector<ScheduledClip> clips;
    std::vector<LayerMix>      layers;
    std::vector<SlotMix>       slots;
    std::vector<GroupMix>      groups;

    /** One entry per mixer channel, in channel order. */
    std::vector<ChannelMix>    channels;

    /** Phase 24: curves that write a hosted instrument's parameter, sorted by
        ascending priority so "apply them all in order" is the whole rule. */
    std::vector<PluginParamCurve> pluginParams;

    /** Phase 23: the order the console strips must be summed in, sources
        first. Resolved once at publish time, from the strip routing, because
        the audio thread must never walk a routing graph — and because "which
        order is legal" is a question about the model, not about a block. */
    std::vector<int>           channelOrder;

    /** Project length in beats: what the transport loops over. */
    double lengthBeats = 0.0;

    /** Where the last note ends, in beats. The transport loops over
        `lengthBeats` (never shorter than the project grid), but an export must
        stop at the content: a 63-beat song should not bounce a silent 64th. */
    double contentBeats = 0.0;

    /** Wall-clock length of one beat *for this schedule*. Everything that
        converts between beats and seconds uses this one number, so a tempo
        change can never leave the playhead pointing at a different moment
        than the notes it is supposed to be crossing. */
    double secondsPerBeat = 0.5;

    double lengthSeconds() const noexcept { return lengthBeats * secondsPerBeat; }

    /** Master strip, frozen. `gain` already folds in the master mute. */
    std::array<fx::Settings, fx::ChainState::maxSlots> masterInserts;
    float  masterGain = 0.22f;
    float  masterPan  = 0.0f;

    /** Phase 26: mono monitoring on the master strip. Applied at the very end,
        after the inserts and after the fader/pan ramp — that is the point where
        the master's signal leaves for the device, which is the only place a
        master-only listen control can act. */
    bool   masterMono = false;

    /** Phase 18: master automation, snapshotted the same way a layer's is.
        Volume MULTIPLIES the fader and pan ADDS to it — the same arithmetic the
        layer lanes use, so "automate the master" behaves like "automate a
        layer" instead of inventing a second law. */
    bool masterLevelAutomated = false;
    bool masterPanAutomated   = false;
    std::vector<AutomationPoint> masterLevelPoints;
    std::vector<AutomationPoint> masterPanPoints;

    /** Phase 43: the master strip's mute lane, a gate like a slot's. The master
        gained a third lane when the channel strips did — "the four controls are
        automatable" is not a rule you can tell a user is almost true. */
    bool masterMuteAutomated = false;
    std::vector<AutomationPoint> masterMutePoints;
};

//==============================================================================
/** Peak measurements from a render. Used by the UI meters and by the headless
    self-test, which checks that a note really did sound on the synth it was
    routed to rather than merely being labelled that way. */
struct RenderStats
{
    /** Phase 26: a channel is measured in TWO channels, because a strip's meter
        has two bars. `peakForChannel` still answers with the louder side, so
        every pre-Phase-26 check keeps its meaning. */
    struct ChannelPeak
    {
        int   channel = -1;
        float left    = 0.0f;
        float right   = 0.0f;

        float loudest() const noexcept { return juce::jmax (left, right); }
    };

    float peak = 0.0f;
    std::vector<std::pair<int, float>> slotPeaks;    // slotId  -> peak
    std::vector<std::pair<int, float>> layerPeaks;   // layerIndex -> peak
    std::vector<ChannelPeak>           channelPeaks; // channel -> peak per side

    RenderStats();

    void  addPeak (int slotId, float newPeak);
    float peakForSlot (int slotId) const;
    void  addLayerPeak (int layerIndex, float newPeak);
    float peakForLayer (int layerIndex) const;
    void  addChannelPeak (int channel, float leftPeak, float rightPeak);
    float peakForChannel (int channel) const;
    float peakForChannelSide (int channel, int side) const;
};

//==============================================================================
/**
    The synth voice pool, the mixer and the transport.

    Design notes worth knowing before changing anything here:

    - **Stateless voices.** A voice's phase is computed from absolute time
      (`freq * (t - noteStart)`), not accumulated across blocks. That means the
      offline render used by the self-test and the live render are literally the
      same code path — "it passes the test" and "it sounds right" cannot drift
      apart.

    - **Two-stage summing (Phase 5).** Voices are summed into a per-slot stereo
      scratch bus, the bus gets the synth's filter and fader, and only then does
      it reach the master. Layers are applied at the voice stage and synths at
      the bus stage, which is the only topology in which a note that bypasses
      its layer still obeys both faders.

    - **The schedule is immutable once published.** The message thread builds a
      whole new PublishedSchedule and swaps a shared_ptr under a spin lock that
      is only ever held for that assignment. The audio thread copies the pointer
      and renders from it. No lock is held while any audio is computed.

    - **Routing is resolved here, once, at publish time.**
*/
class PlaybackEngine
{
public:
    static constexpr int    maxMeteredSlots = 16;
    static constexpr int    maxMeteredLayers = 16;
    static constexpr int    maxLayersPerBlock = 16;
    static constexpr double defaultBpm      = 120.0;
    static constexpr double timelineBeats   = timeline::defaultLengthBeats;

    PlaybackEngine();

    void   setSampleRate (double newSampleRate);
    double getSampleRate() const noexcept { return sampleRate; }

    /** Hosted VST3 instruments and inserts. Not owned: the session owns the
        host and outlives the engine. Passing nullptr (the default) is the
        pre-Phase-9 behaviour — everything plays on the internal oscillator. */
    void        setPluginHost (PluginHost* host) noexcept { pluginHost = host; }
    PluginHost* getPluginHost() const noexcept            { return pluginHost; }

    /** Sizes the internal scratch bus so the audio callback never allocates. */
    void prepareToPlay (double newSampleRate, int expectedBlockSize);

    /** Message thread only. Flattens every layer into ScheduledNotes, resolving
        each note's output via routing::resolve(), and freezes every fader,
        pan, mute, solo and automation curve into the schedule.

        `requestedLengthBeats` is 0 to mean "as long as the content, but never
        shorter than the project grid". */
    void publishSchedule (const LayerStack& stack, const RackModel& rack,
                          double bpm, double requestedLengthBeats = 0.0,
                          const MixerModel* mixer = nullptr);

    std::shared_ptr<const PublishedSchedule> getSchedule() const;

    //-- Transport ------------------------------------------------------------
    /** The transport counts in BEATS, not seconds.

        Beats are the invariant. At 120 BPM beat 8 is 4 seconds; at 60 BPM it is
        8 seconds; but the playhead is on beat 8 either way, and every note is
        still exactly where the user drew it. Storing seconds here would make a
        tempo change teleport the playhead onto a different musical moment —
        the kind of bug that only shows up while dragging the BPM field. */
    void   setPositionBeats (double beats);
    double getPositionBeats() const noexcept { return positionBeats.load (std::memory_order_relaxed); }

    /** Seconds are always derived from the current tempo, never stored. */
    double getPosition() const;
    void   setPosition (double seconds);

    double getSecondsPerBeat() const;
    double getLengthBeats() const noexcept   { return lengthBeats.load (std::memory_order_relaxed); }
    double getLengthSeconds() const noexcept { return lengthSeconds.load (std::memory_order_relaxed); }

    //-- Block timing, for the UI's sub-block interpolation -------------------
    double getBlockStartBeats() const noexcept   { return blockStartBeats.load (std::memory_order_relaxed); }
    double getBlockBeats() const noexcept        { return blockBeats.load (std::memory_order_relaxed); }
    double getBlockStartTimeMs() const noexcept  { return blockStartTimeMs.load (std::memory_order_relaxed); }

    /** Audio thread. Renders [position, position + numSamples) and advances,
        wrapping at lengthBeats — including when the wrap lands mid-block. */
    void renderNextBlock (juce::AudioBuffer<float>& output, int numSamples);

    /** Deterministic render from t = 0, ignoring the transport. */
    RenderStats renderOffline (double seconds) const;

    /** Deterministic render of an arbitrary window, for envelope analysis. */
    void renderWindow (double startSeconds, int numSamples,
                       juce::AudioBuffer<float>& output, RenderStats* stats) const;

    /** Deterministic render of a span of an explicitly supplied schedule into a
        region of `destination`.

        This is what the offline exporter walks, block by block, over the whole
        song. Two reasons it takes the schedule as an argument instead of using
        the published one: the export runs on a background thread while the
        user keeps editing, and the engine it renders with needs its own filter
        memory — not the live one's — so a bounce can never be coloured by
        whatever was playing at the time. */
    void renderScheduleSpan (const PublishedSchedule& schedule, double startSeconds,
                             int numSamples, juce::AudioBuffer<float>& destination,
                             int destinationOffset, RenderStats* stats = nullptr) const;

    float getSlotPeak (int slotId) const;
    float getLayerPeak (int layerIndex) const;
    float getChannelPeak (int channel) const;

    /** Phase 26: one side of a strip's meter. `side` 0 = left, 1 = right; a
        channel that does not exist reads 0 rather than throwing an index. */
    float getChannelPeakSide (int channel, int side) const;
    float getMasterPeak (int channel) const;

    /** Clears every meter, and every filter's memory. Call before a deterministic
        measurement so a previous pass cannot colour the next one. */
    void  resetMixState();

    //-- Phase 19: audition ---------------------------------------------------
    /** Sounds one note NOW, without touching the transport and whether or not
        the transport is running. Fired when the user draws a note or drags one
        onto a new pitch: editing a pitch grid in silence is guesswork.

        `retrigger` = true starts the preview envelope from the top (a newly
        drawn note). False means "same gesture, new pitch": the voice keeps its
        phase and its envelope, so dragging across an octave is one sound
        sliding rather than twelve blips. */
    void auditionNote (int pitch, float velocity, int slotId, bool retrigger = true);

    /** True while a preview is pending or sounding. The device callback uses it
        to keep rendering after the transport's fade-out has finished. */
    bool hasAudition() const noexcept
    {
        return auditionEngaged.load (std::memory_order_relaxed)
                 || auditionSerial.load (std::memory_order_relaxed)
                      != auditionConsumed.load (std::memory_order_relaxed);
    }

    /** Adds the preview voice into `output` (which already carries the mix).
        Audio thread only. Deliberately NOT part of renderNextBlock: a preview is
        not transport audio, and the export walker must never bounce one. */
    void renderAudition (juce::AudioBuffer<float>& output, int numSamples);

    //-- Phase 20: sample preview (clicking a file in the browser) ------------
    /** Plays `buffer` from its first frame for at most `seconds`, right now,
        transport running or not.

        Starting a preview while another one is sounding REPLACES it. That is
        the requirement, not an implementation accident: two files auditioning
        at once is never what a single click meant, and a crossfade would make
        it sound like a deliberate blend. The old one stops where it is; the new
        one ramps in over 4 ms so the cut is not a click. */
    void previewSample (std::shared_ptr<const SampleBuffer> buffer, double seconds);

    /** Silences the preview at the next block. The buffer stays alive until the
        following preview replaces it — freeing eight megabytes on the audio
        thread is the one thing this is careful about. */
    void stopSamplePreview();

    /** True while a preview is pending or sounding; the device callback asks
        this to keep rendering with the transport stopped. */
    bool hasSamplePreview() const noexcept
    {
        return previewEngaged.load (std::memory_order_relaxed)
                 || previewSerial.load (std::memory_order_relaxed)
                      != previewConsumed.load (std::memory_order_relaxed);
    }

    /** Adds the sample preview into `output`. Audio thread only, and — like the
        note preview — deliberately not part of renderNextBlock. */
    void renderSamplePreview (juce::AudioBuffer<float>& output, int numSamples);

    //-- Phase 39: live performance -------------------------------------------
    /** A key held down right now, from outside the timeline: a MIDI controller
        or the computer keyboard.

        This is NOT the audition voice above. Audition answers "what does the
        note I just drew sound like" — one gesture, one envelope, gone. A played
        key has a beginning AND an end, and up to sixteen of them at once, so it
        needs its own polyphonic voice pool and a real note-off. Sharing the
        audition voice would have given the app a monophonic keyboard whose
        notes fade out on their own, which is not playing.

        Safe to call from the message thread and from the MIDI thread: the hand
        off is a lock-free queue, and the only lock is taken by producers, never
        by the audio thread. */
    void noteOnLive (int slotId, int pitch, float velocity);

    /** Releases whatever is holding `pitch`. Matched by PITCH alone — a
        keyboard cannot have two of the same pitch down at once, and it lets the
        note-off find its voice even if the target synth changed mid-note. */
    void noteOffLive (int pitch);

    /** Every held note released, plugins included. */
    void allLiveNotesOff();

    /** True while anything live is sounding or waiting: the device callback
        uses it to keep rendering with the transport stopped. */
    bool hasLiveNotes() const noexcept
    {
        return liveVoicesActive.load (std::memory_order_relaxed) > 0
                 || livePluginEngaged.load (std::memory_order_relaxed) > 0
                 || liveQueued.load (std::memory_order_relaxed) > 0;
    }

    /** How many held voices there are right now. The settings page and the
        self-test both ask this rather than guessing from pixels. */
    int getLiveVoiceCount() const noexcept
    {
        return liveVoicesActive.load (std::memory_order_relaxed);
    }

    /** Audio thread, once per device callback, BEFORE any rendering.

        It is the single point where the queue is drained, for one reason: a
        hosted instrument is rendered by the transport when the transport is
        running and by `renderLive` when it is not. Both need the same events,
        and consuming them twice would send every note-on twice. One drain, then
        both readers look at the same array. */
    void beginLiveBlock();

    /** Adds the held voices into `output` (which already carries the mix).
        Audio thread only; called straight from the device callback, after the
        transport's fade, for the same reason the audition voice is. */
    void renderLive (juce::AudioBuffer<float>& output, int numSamples);

    /** Drops hosted instruments' held notes and effects' tails. Called when the
        transport stops or loops: a note-off that never got sent would leave a
        plugin droning into the next playback. */
    void  allNotesOffHosted();

    /** Phase 42: one block of meter fall.

        A meter is a READING, not a maximum. Every render path stores the block
        peak with `jmax`, which is correct within a block and wrong across them:
        the bar then holds the loudest thing it ever saw and never comes back
        down. Called once per device callback, before rendering, so a silent
        block is a block the meters fall through. The rate is in dB per second
        and is evaluated over this callback's own duration, so a 256-sample
        buffer and a 2048-sample one fall at the same speed. */
    void  decayMeters (int numSamples);

    /** True while any meter still reads something above the noise floor. The
        window uses it to keep repainting the bars after the transport stops —
        and to stop repainting them, forever, once they have settled at zero. */
    bool  hasMeters() const noexcept;

    void  resetMeters();

    /** How many notes currently resolve to this slot, and how many of those got
        there by bypassing their layer. The rack displays both. */
    int countNotesRoutedTo (int slotId) const;
    int countOverridesRoutedTo (int slotId) const;

private:
    /** Per-layer values for the block being rendered: ramps in, ramps out. */
    struct LayerBlock
    {
        int   layerIndex = -1;
        float gain0 = 0.0f, gain1 = 0.0f;
        float pan0  = 0.0f, pan1  = 0.0f;
    };

    /** Phase 16: the same idea for a group's level curve. */
    struct GroupBlock
    {
        int   groupId = -1;
        float gain0 = 1.0f, gain1 = 1.0f;
        float pan0  = 0.0f, pan1  = 0.0f;   // Phase 20: panned per block too
    };

    static constexpr int maxGroupMixesPerBlock = 32;

    struct FilterState
    {
        float zL = 0.0f;
        float zR = 0.0f;
    };

    void renderInto (const PublishedSchedule&, double startSeconds, int numSamples,
                     juce::AudioBuffer<float>&, RenderStats*) const;

    void applySlotFilter (const SlotMix&, double startBeat, int numSamples) const;

    /** Stage 1b: hosted instruments. Each plugin renders its own stereo output
        from MIDI built out of the notes routed to it, and adds it straight into
        the bus its slot feeds (a console channel, or the master). */
    void renderHostedInstruments (const PublishedSchedule&, double startSeconds,
                                  int numSamples,
                                  std::array<bool, MixerModel::numChannels + 1>& busUsed,
                                  RenderStats* stats) const;

    /** One insert chain, in slot order. Native effects come from `chain`,
        VST3 effects from the plugin host. */
    void processInsertChain (const std::array<fx::Settings, fx::ChainState::maxSlots>& settings,
                             juce::AudioBuffer<float>& buffer, int numSamples,
                             fx::ChainState& chain) const;

    /** One hosted insert, in place. */
    void processHostedEffect (int pluginInstanceId, juce::AudioBuffer<float>& buffer,
                              int numSamples) const;

    /** Phase 24: writes every published plugin-parameter curve that covers this
        chunk into `plugin`. Called with the plugin lock held, between the MIDI
        build and processBlock, once per chunk.

        Only sets a parameter when the value actually changed: a parameter
        notification per block per curve would make the plugin redraw its whole
        editor sixty times a second for nothing. */
    void applyHostedParamAutomation (juce::AudioProcessor& plugin,
                                     const PublishedSchedule&,
                                     int pluginInstanceId,
                                     double chunkStartBeat, double chunkEndBeat) const;

    double sampleRate = 44100.0;

    mutable juce::SpinLock scheduleLock;
    std::shared_ptr<const PublishedSchedule> schedule;

    std::atomic<double> positionBeats { 0.0 };
    std::atomic<double> lengthBeats   { timelineBeats };
    std::atomic<double> lengthSeconds { timelineBeats * 0.5 };

    std::atomic<double> blockStartBeats  { 0.0 };
    std::atomic<double> blockBeats       { 0.0 };
    std::atomic<double> blockStartTimeMs { 0.0 };

    std::array<std::atomic<float>, maxMeteredSlots>  meters;
    std::array<std::atomic<float>, maxMeteredLayers> layerMeters;

    /** Phase 26: two arrays, not one. A strip's meter has a bar per side, and
        averaging them at the source would throw away the only thing a stereo
        meter is for — seeing that a hard-panned strip is silent on one side. */
    std::array<std::atomic<float>, MixerModel::numChannels> channelMetersL;
    std::array<std::atomic<float>, MixerModel::numChannels> channelMetersR;
    std::array<std::atomic<float>, 2>                masterMeters;

    /** Reused every block: one stereo bus, one slot at a time. Allocated in
        prepareToPlay, never inside the callback. */
    mutable juce::AudioBuffer<float> mixScratch;

    /** Phase 11: per console channel, plus index `numChannels` for "bypasses
        the console". A layer is not a bus any more: a sheet of notes picks an
        instrument, and the instrument is what a channel receives.

        Phase 25: with a 127-strip ceiling these arrays are CAPACITY, not the
        live count. Buffers are one block each and are sized in prepareToPlay,
        so all of them can exist; the insert chains cannot — see chainsReady. */
    static constexpr int numBuses = MixerModel::numChannels + 1;
    mutable std::array<juce::AudioBuffer<float>, numBuses> channelBuses;

    /** Insert memory for every bus the console can address. The objects stay
        EMPTY until prepare() sizes their delay lines, and prepare() is only ever
        called for indices below `chainsReady`: a delay line per insert per
        channel is ~700 kB, so preparing all 128 up front would cost ~90 MB for
        a session that opens eight strips.

        Grows only. The message thread prepares new indices before publishing a
        schedule that references them, so the audio thread can never reach an
        unprepared chain. */
    mutable std::array<fx::ChainState, MixerModel::numChannels + 1> channelChains;
    std::atomic<int>  chainsReady      { 0 };
    std::atomic<bool> masterChainReady { false };

    /** Message thread. Guarantees chains 0..count-1 plus the master chain exist.
        Called from publish (a growing console must have memory before it
        sounds) and from prepareToPlay. */
    void prepareChannelChains (int count);

    /** The plugin chain pointer, and the scratch it renders into. Both are only
        touched while the caller holds the host's lock. */
    PluginHost* pluginHost = nullptr;
    mutable juce::AudioBuffer<float> pluginBuffer;
    mutable juce::MidiBuffer        pluginMidi;

    /** Set by the message thread on stop / loop, honoured by the audio thread
        under the plugin lock. A flag instead of a call, because the message
        thread must not block waiting for the callback to release the lock. */
    mutable std::atomic<bool> pluginPanicRequested { false };

    /** The synth filters' memory. Audio thread only. */
    mutable std::array<FilterState, maxMeteredSlots> slotFilters;

    //-- Phase 19: the preview voice -----------------------------------------
    /** Message thread -> audio thread. One serial per request: the payload is
        written first and the serial is bumped last with release semantics, so
        the audio thread can never see a half-written request. */
    std::atomic<int>   auditionSerial    { 0 };
    std::atomic<int>   auditionConsumed  { 0 };      // audio -> message: which serial was seen
    std::atomic<int>   auditionPitch     { 60 };
    std::atomic<int>   auditionSlotId    { -1 };
    std::atomic<float> auditionVelocity  { 0.8f };
    std::atomic<bool>  auditionRetrigger { true };
    std::atomic<bool>  auditionEngaged   { false };

    /** Audio thread only. Phase kept across a pitch change so a drag does not
        restart the waveform mid-cycle. */
    struct AuditionVoice
    {
        int      serial   = 0;
        int      pitch    = 60;
        int      slotId   = -1;
        Waveform waveform = Waveform::sine;
        float    velocity = 0.8f;
        float    gain     = 1.0f;
        float    pan      = 0.0f;
        double   phase    = 0.0;
        double   played   = 0.0;      // seconds since the gesture started
        float    amplitude = 0.0f;    // smoothed, so no gesture can click
        bool     active   = false;
    };

    mutable AuditionVoice auditionVoice;

    //-- Phase 39: live performance -------------------------------------------
    /** One played key, queued. Small and flat on purpose: the audio thread
        reads these and does nothing but compare ids. */
    struct LiveEvent
    {
        int   slotId   = -1;
        int   pitch    = 60;
        float velocity = 0.8f;
        bool  on       = false;

        /** "Let go of everything." The one event that has to exist, because
            releasing every voice means reading audio-thread state — and the
            caller (a settings click, a source switch) is on the message
            thread, where that state is not readable at all. */
        bool  allOff   = false;
    };

    static constexpr int liveQueueSize   = 512;    // producer side, runs of keys
    static constexpr int maxLivePerBlock = 128;    // drained in one callback
    static constexpr int maxLiveVoices   = 16;
    static constexpr int maxLivePlugins  = 8;
    static constexpr int maxTransportPluginSlots = 32;

    /** Producer side. Only producers take the lock — two of them can exist at
        once (the MIDI thread and the message thread), and AbstractFifo assumes
        exactly one, so they take turns instead. The audio thread never locks. */
    juce::SpinLock liveProducerLock;
    juce::AbstractFifo liveFifo { liveQueueSize };
    std::array<LiveEvent, liveQueueSize> liveQueue;
    std::atomic<int> liveQueued { 0 };

    void pushLiveEvent (const LiveEvent& event);

    /** Audio thread. One held key of a synth that makes its own waveform. */
    struct LiveVoice
    {
        int      slotId   = -1;
        int      pitch    = 60;
        float    velocity = 0.8f;
        Waveform waveform = Waveform::sine;
        float    gain     = 1.0f;
        float    pan      = 0.0f;
        double   phase    = 0.0;
        float    amp      = 0.0f;     // smoothed; a key never clicks on or off
        bool     released = false;
        bool     active   = false;
    };

    std::array<LiveEvent, maxLivePerBlock> pendingLive;
    int  pendingLiveCount = 0;

    /** Which synth each held pitch went to. A note-off arrives with nothing but
        a pitch, so this is what puts the release back on the right instrument —
        including a hosted one, which would otherwise hold the note for ever. */
    struct LiveHeldNote
    {
        int pitch  = -1;
        int slotId = -1;
    };

    std::array<LiveHeldNote, 32> liveHeldNotes;

    std::array<LiveVoice, maxLiveVoices>   liveVoices;
    std::atomic<int> liveVoicesActive { 0 };

    /** A hosted instrument that somebody is playing. `held` is the notes; the
        tail keeps the plugin rendering after the last note-off, because a pad
        with a four-second release is still making sound when the key is up and
        the transport is stopped — and a plugin that stops being processed
        freezes its tail mid-note. */
    struct LivePluginSlot
    {
        int  slotId     = -1;
        int  tailFrames = 0;

        /** Which pitches this instrument is holding. A count would not do: a
            "let go of everything" switch has to send a real note-off per key,
            and a plugin left holding a note drones into the next session. */
        std::array<int, 16> held;
        int numHeld = 0;
    };

    std::array<LivePluginSlot, maxLivePlugins> livePluginSlots;
    std::atomic<int> livePluginEngaged { 0 };

    /** Slot ids the transport rendered in this block, so `renderLive` does not
        process the same plugin a second time. Processing a plugin twice per
        callback does not double its output — it doubles the rate its own DSP
        runs at, which is a bug that only shows up as "the LFO is twice as
        fast when the transport is rolling". */
    mutable std::array<int, maxTransportPluginSlots> transportPluginSlots;
    mutable int numTransportPluginSlots = 0;

    juce::AudioBuffer<float> livePluginBuffer;
    juce::MidiBuffer        liveMidi;

    void applyLiveEvent (LiveEvent& event);
    LiveVoice* findLiveVoice (int pitch, bool includeReleased);
    int        freeLiveVoiceSlot() const;
    LivePluginSlot* findLivePluginSlot (int slotId);
    void  renderLiveOscillators (const PublishedSchedule*, float masterGain,
                                 float masterPan, juce::AudioBuffer<float>&, int numSamples);
    void  renderLivePlugins (const PublishedSchedule*, float masterGain,
                             juce::AudioBuffer<float>&, int numSamples);

    //-- Phase 20: the sample preview voice -----------------------------------
    /** The hand-off. The message thread writes the buffer under the lock and
        bumps `previewSerial` last; the audio thread copies the pointer out
        under a TRY-lock and gives up until the next block if the message thread
        happens to be inside. Same trade the plugin host makes, for the same
        reason: a shared_ptr assignment is a few pointer writes, not an
        allocation, and losing one block of a preview is inaudible. */
    juce::SpinLock previewLock;
    std::shared_ptr<const SampleBuffer> previewPending;
    std::atomic<int>    previewSerial   { 0 };
    std::atomic<int>    previewConsumed { 0 };
    std::atomic<double> previewSeconds  { 3.0 };
    std::atomic<bool>   previewEngaged  { false };

    /** Audio thread only. */
    struct PreviewVoice
    {
        int    serial   = 0;
        std::shared_ptr<const SampleBuffer> buffer;
        double position = 0.0;    // in SOURCE frames
        double frames   = 0.0;    // how far this preview may go
        float  gain     = 0.0f;   // smoothed, so the cut in/out cannot click
        bool   active   = false;
    };

    mutable PreviewVoice previewVoice;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackEngine)
};

} // namespace atomic

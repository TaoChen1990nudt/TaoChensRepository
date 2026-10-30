#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "Model/AutomationLane.h"
#include "Model/TempoModel.h"
#include "Model/HardwareRack.h"
#include "Model/LayerModel.h"
#include "Model/LoopRegion.h"
#include "Model/MixerModel.h"

#include "Effects.h"
#include "ClipStretch.h"
#include "Metronome.h"
#include "PdcDelay.h"

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
    bool   overridden   = false;       // reached this synth by bypassing its layer

    //-- Phase 82: one snapshot per routed curve ------------------------------
    /** A note can carry a level curve AND a pan curve AND a pitch curve at the
        same time now, so "the note's envelope" is no longer one object.

        Each slot is inert unless its flag is set, and the flag is what a
        disabled curve used to be: an un-routed curve must cost nothing on the
        audio thread, and it must not be mistaken for a flat one — a flat LEVEL
        curve is 1.0 (no change) while a flat PAN curve is 0.5 (centre), so the
        two cannot share a single "no curve" value.

        Snapshotted by value at publish time, like every other curve that
        reaches the audio thread: the renderer never looks at the note. */
    NoteEnvelope levelCurve;
    NoteEnvelope panCurve;
    NoteEnvelope pitchCurve;
    NoteEnvelope pluginCurve;

    bool levelCurveOn = false;
    bool panCurveOn   = false;
    bool pitchCurveOn = false;

    /** >= 0 when `pluginCurve` drives parameter N of this note's synth. */
    int  pluginParamIndex = -1;

    /** Phase 16: which note group this came from, or -1.

        The group's level curve is NOT copied in per note: the schedule holds one
        `GroupMix` per group and the voice stage looks it up by id, so a group of
        two hundred notes does not carry two hundred copies of the same curve. */
    int    groupId      = -1;

    /** Phase 58: which appearance of that group — only the renderer's "just
        this copy" scope cares, but it has to survive publication or the scope
        cannot tell the chorus copy from the verse one. */
    int    groupOccurrence = 0;

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

    /** Phase 83: what this strip's inserts cost, in samples.

        A hosted VST3 insert may report latency; the built-ins never do (none of
        them looks ahead). Folded at publish time, because the audio thread must
        not ask a plugin anything — it would be a lock, a virtual call, and an
        answer that can change under it. */
    int insertLatency = 0;

    /** Phase 83: the latency of everything that arrives at this strip's INPUT.

        Zero when nothing feeds it. Otherwise the slowest child's end — which is
        also the instant this strip's own voices have to be held back to, since
        a direct voice arrives at 0. */
    int entryLatency = 0;
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
        plays this", and it was resolved without looking at the group.

        Phase 58: `groupOccurrence` says which appearance, for the renderer's
        "just this copy" scope. It changes nothing about how the block sounds. */
    int    groupId      = -1;
    int    groupOccurrence = 0;

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

    /** Phase 50: the clip's pan curve, converted to seconds the same way and
        for the same reason. 0.5 is centre, so the published values are offsets
        in -1..+1 added to the fader sums — one arithmetic with the layer's own
        pan lane, which is where "pan is a sum" was first written down. */
    bool                        panAutomated = false;
    std::vector<AutomationPoint> panCurve;     // x = seconds since clip start

    /** Phase 50: read the window right to left. The renderer mirrors its
        elapsed time instead of the buffer, so a reversed block stays a pure
        function of the clock and cannot drag the shared file with it. */
    bool                        reversed = false;

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

    /** Phase 83: the console's total latency, in samples.

        Zero unless some insert reports latency. Every path is held back to this
        figure at the joins it passes through, so the mix as a whole is late by
        exactly this many samples — uniform, inaudible for playback, and the
        number a future "align the playhead / the recording" step would need.
        Nothing compensates for it today; correcting it would shift the sound
        against the playhead, which is a separate decision from aligning two
        strips against each other. */
    int maxPdcLatency = 0;

    /** Project length in beats: what the transport loops over. */
    double lengthBeats = 0.0;

    /** Where the last note ends, in beats. The transport loops over
        `lengthBeats` (never shorter than the project grid), but an export must
        stop at the content: a 63-beat song should not bounce a silent 64th. */
    double contentBeats = 0.0;

    /** Phase 80: beat <-> second conversion FOR THIS SCHEDULE.

        It used to be one `double secondsPerBeat`, which is exactly right until
        the tempo moves — and then it is the source of every wrong answer at
        once: note starts, clip lengths, the playhead, the loop point. A map
        replaces it and keeps the same promise the single number made ("one
        authority for beat-to-second"), now for a tempo that changes.

        A project without tempo automation still gets a flat map, so the
        arithmetic degenerates to the old multiplication.
    */
    TempoMap tempo;

    double lengthSeconds() const noexcept { return tempo.secondsAtBeat (lengthBeats); }

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

    /** Phase 70: empties the vectors without giving back the memory they hold.

        The audio thread reuses ONE instance across callbacks (see
        `liveStats`), so this has to be `clear()`, not "construct a fresh one" —
        a new object per block meant three reserves and three frees per device
        callback, which is exactly the kind of allocation a real-time callback
        is not allowed to make. `std::vector::clear()` keeps the capacity, so
        the arrays bought in prepareToPlay are spent once and never again. */
    void clear() noexcept
    {
        peak = 0.0f;
        slotPeaks.clear();
        layerPeaks.clear();
        channelPeaks.clear();
    }

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
                          const MixerModel* mixer = nullptr,
                          const TempoModel* tempoModel = nullptr);

    std::shared_ptr<const PublishedSchedule> getSchedule() const;

    /** Phase 70: the audio thread's read of the same thing.

        `getSchedule()` takes a spin lock and returns a shared_ptr COPY — which
        meant every audio callback spun for the lock, and worse, that copy was
        sometimes the last reference: the message thread had already published a
        newer schedule and dropped its own, so the destructor of the whole
        PublishedSchedule (dozens of notes, and a shared_ptr per plugin insert)
        ran on the audio thread.

        This returns a bare pointer off one acquire load. The object stays alive
        because the publisher keeps the last two schedules in `scheduleRetired`
        before letting go of them — one block of lag is all the audio thread
        ever has, so two is generous.

        Never hold the result across a publish, and never store it: it is valid
        for the duration of the block that read it, not longer. */
    const PublishedSchedule* getScheduleRaw() const noexcept
    {
        return publishedSchedule.load (std::memory_order_acquire);
    }

    //-- Transport ------------------------------------------------------------
    /** The transport counts in BEATS, not seconds.

        Beats are the invariant. At 120 BPM beat 8 is 4 seconds; at 60 BPM it is
        8 seconds; but the playhead is on beat 8 either way, and every note is
        still exactly where the user drew it. Storing seconds here would make a
        tempo change teleport the playhead onto a different musical moment —
        the kind of bug that only shows up while dragging the BPM field. */
    void   setPositionBeats (double beats);
    double getPositionBeats() const noexcept { return positionBeats.load (std::memory_order_relaxed); }

    /** Seconds are always derived from the current tempo map, never stored. */
    double getPosition() const;
    void   setPosition (double seconds);

    /** Seconds per beat AT THE PLAYHEAD. Ask it twice during an accelerando and
        you get two different answers — that is the point of Phase 80. */
    double getSecondsPerBeat() const;

    /** Beat <-> second through the published map, for callers that need to place
        some OTHER beat (a tail, a marker, an export end) on the wall clock.
        Falls back to the default tempo before the first schedule exists. */
    double secondsAtBeat (double beat) const;
    double beatAtSeconds (double seconds) const;
    double getLengthBeats() const noexcept   { return lengthBeats.load (std::memory_order_relaxed); }
    double getLengthSeconds() const noexcept { return lengthSeconds.load (std::memory_order_relaxed); }

    //-- Phase 87: the loop region -------------------------------------------
    /** Message thread -> audio thread. Three atomics rather than a field on the
        published schedule, because the region changes while the user drags its
        edge and a drag is sixty publishes a second; the schedule is a document
        of what the SONG is, and the loop is what the TRANSPORT is doing.

        Read once per block by the renderer, which is why "the loop moved" takes
        effect on the next callback and never mid-block. */
    void setLoopRegion (const LoopRegion& region) noexcept
    {
        loopStartBeat.store (region.startBeat, std::memory_order_relaxed);
        loopEndBeat.store   (region.endBeat,   std::memory_order_relaxed);
        loopEnabled.store   (region.enabled,   std::memory_order_relaxed);
    }

    LoopRegion getLoopRegion() const noexcept
    {
        LoopRegion region;
        region.enabled   = loopEnabled.load (std::memory_order_relaxed);
        region.startBeat = loopStartBeat.load (std::memory_order_relaxed);
        region.endBeat   = loopEndBeat.load   (std::memory_order_relaxed);
        return region;
    }

    /** Phase 83: how late the currently published console makes the mix.

        Zero for every session without a latency-reporting VST3 insert, which is
        the assertion that keeps the compensation path from silently changing
        the sound of projects that never needed it. */
    int getMaxPdcLatency() const;

    /** Phase 83, self-test only: pretend this strip's inserts cost `samples`.

        The headless harness cannot load a VST3 — it never runs a plugin scan —
        so without this the compensation path would exist without ever being
        rendered, which is precisely the situation this project keeps paying for.
        One array, read at publish, never touched by the audio thread. */
    void setInsertLatencyForTest (int channel, int samples);

    //-- Phase 88: the click track and the count-in ---------------------------
    /** The metronome belongs to the transport, not to the mixer: it is mixed
        per segment inside `renderNextBlock`, so it follows the loop and the
        tempo map, and it is *not* in `renderInto`, so an export can never
        contain it. */
    Metronome&       getMetronome() noexcept       { return metronome; }
    const Metronome& getMetronome() const noexcept { return metronome; }

    /** Message thread. Plays `beats` counts of click BEFORE the transport moves,
        then hands the rest of that block to the normal renderer.

        The alternative — starting the transport and letting the recording
        begin four beats later — puts four beats of moving playhead in front of
        the take and makes `recordStartBeat` a lie. Nothing here advances
        `positionBeats`: a count-in is the beat arriving, not the song moving. */
    void startPreRoll (int beats) noexcept;
    void cancelPreRoll() noexcept;

    bool isPreRolling() const noexcept { return preRollBeatsLeft.load (std::memory_order_relaxed) > 0; }

    /** How many counts are still to come. Drives the "预备 4 3 2 1" readout, so
        it counts down at the beat rather than at the block. */
    int getPreRollBeatsLeft() const noexcept;
    int getPreRollTotalBeats() const noexcept { return preRollBeatsTotal.load (std::memory_order_relaxed); }

    /** Audio thread. Renders the count-in, then — if it ends inside this block —
        renders the remainder as a normal transport block, so the first beat of
        the take is not preceded by up to one buffer of silence. */
    void renderPreRoll (juce::AudioBuffer<float>& output, int numSamples);

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

    /** Phase 71: ends the preview at 4 ms' notice.

        Called when the same gesture is re-routed to a hosted instrument: the
        oscillator voice that was standing in for it must not keep sounding
        underneath the plugin, or "the preview is the patch" would be false for
        as long as the fade lasts. A no-op when nothing is previewing. */
    void stopAudition();

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

    /** How many of the live-plugin slots are still claimed.

        Phase 71: a table entry that outlives the instrument it points at is
        invisible — `hasLiveNotes()` drops to zero either way, because the
        renderer counts only the slots it actually rendered. But the table has
        eight places, so eight such leftovers end live playback for the session.
        This is the only way to see them. */
    int getLivePluginSlotCount() const noexcept
    {
        int count = 0;

        for (const auto& live : livePluginSlots)
            if (live.slotId >= 0)
                ++count;

        return count;
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

    /** Phase 70: the same schedule, published for the audio thread.

        `publishedSchedule` is what `getScheduleRaw()` reads — one atomic load,
        no lock. `schedule` is what the message thread (and the offline
        renderer) keeps a counted reference to, so `getSchedule()` can hand out
        a shared_ptr as before.

        `scheduleRetired` is the safety margin: publishing pushes the outgoing
        schedule in here and drops anything older than two, so a block that is
        mid-render when the swap happens is reading an object that still has an
        owner. The release therefore always happens on the message thread. */
    std::atomic<const PublishedSchedule*> publishedSchedule { nullptr };
    std::vector<std::shared_ptr<const PublishedSchedule>> scheduleRetired;

    /** Phase 70: the one measurement object the audio callback reuses.

        It used to be a local in renderNextBlock, so every callback constructed
        three vectors (three reserves), filled them, and freed them on return —
        a malloc/free storm in the real-time thread. The capacities are bought
        once, when the engine is constructed; from then on the callback only
        calls clear(). */
    RenderStats liveStats;

    std::atomic<double> positionBeats { 0.0 };
    std::atomic<double> lengthBeats   { timelineBeats };
    std::atomic<double> lengthSeconds { timelineBeats * 0.5 };

    // Phase 87: the loop, written whole by the message thread (three relaxed
    // stores) and read once per block. A half-updated triple — new start, old
    // end — lasts only until the next store, and the worst it can do is play a
    // slightly wrong region for one buffer, so it does not need a seqlock.
    std::atomic<bool>   loopEnabled   { false };
    std::atomic<double> loopStartBeat { 0.0 };
    std::atomic<double> loopEndBeat   { 0.0 };

    std::atomic<double> blockStartBeats  { 0.0 };
    std::atomic<double> blockBeats       { 0.0 };
    std::atomic<double> blockStartTimeMs { 0.0 };

    // Phase 88: the click track, and the count-in that precedes a take. The
    // count-in is expressed in BEATS and advanced by the audio thread; nothing
    // about it touches `positionBeats`, which is what makes "the song did not
    // move during the count-in" true by construction rather than by care.
    Metronome            metronome;
    std::atomic<int>     preRollBeatsLeft  { 0 };     // 0 = not counting in
    std::atomic<int>     preRollBeatsTotal { 0 };
    std::atomic<double>  preRollBeat       { 0.0 };   // how far into the count-in

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

    /** Phase 83: the delay memory a strip needs when a slower sibling exists.

        Two lines per strip, and the reason is worth keeping: `entry` holds the
        strip's INPUT (so its own voices can wait for a late child before the
        inserts see them), `exit` holds its finished output (so it can be held
        back again when it is summed into a parent that already started
        earlier). One line cannot serve both, because they are written at two
        different moments of the same block.

        Prepared lazily for the strips in use, like the insert chains: two
        channels of 8192 floats apiece is 64 kB, and the console can address
        128 strips. */
    mutable std::array<pdc::Line, MixerModel::numChannels + 1> pdcEntry;
    mutable std::array<pdc::Line, MixerModel::numChannels + 1> pdcExit;

    /** Phase 83: what the self-test asks a strip to cost. Message thread only,
        and folded into the published figures at publish time — the renderer
        never reads it. */
    std::array<int, MixerModel::numChannels> testInsertLatency {};

    /** Message thread. Buys the delay memory for strips 0..count-1 (and the
        bypass bus). Idempotent; a strip that already has memory keeps it. */
    void preparePdcLines (int count);

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

    /** Phase 71: "cut the preview short". A flag rather than a store into
        `auditionEngaged`, because the callback has to keep rendering long
        enough for the 4 ms ramp to finish — dropping `engaged` first would
        silence the device callback mid-ramp and the preview would click. */
    std::atomic<bool>  auditionStopRequest { false };

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

    /** Phase 71: gives back every live-plugin slot whose instrument is gone.

        Split out of the renderer on purpose: the renderer needs the plugin host
        to do anything, and this question — "does the slot the key went to still
        exist, and is it still a hosted instrument?" — is answered by the
        schedule alone. A key can outlive its instrument (the card was deleted
        mid-note), and the table it left behind keeps one of eight places. */
    void pruneLivePluginSlots (const PublishedSchedule*);
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

    /** Phase 70: where the audio thread parks the buffer it is replacing.

        Assigning straight over `previewVoice.buffer` drops the last reference
        to the outgoing sample whenever the message thread has already let go
        of it, and that is a free of the whole frame array — on the audio
        thread, which is the one thing this file is careful about. Moving it
        here instead means the message thread performs the release, one
        hand-off later, under the same lock. */
    std::shared_ptr<const SampleBuffer> previewRetired;
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

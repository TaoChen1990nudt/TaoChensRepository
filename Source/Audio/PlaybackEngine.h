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

    /** -1 = straight into the master strip, skipping every console channel. */
    int   mixerChannel = -1;

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

    float gainAt (double beat) const noexcept
    {
        const float fader = audible ? gain : 0.0f;

        if (! levelAutomated)
            return fader;

        return fader * automationValueAt (levelPoints, beat);
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
        the notes that belong to me", which is what the group list is for. */
    float gain = 1.0f;
    float pan  = 0.0f;

    float gainAt (double beat) const noexcept
    {
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
    std::shared_ptr<const SampleBuffer> audio;

    double lengthSeconds() const noexcept { return endSeconds - startSeconds; }
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

    /** Phase 18: master automation, snapshotted the same way a layer's is.
        Volume MULTIPLIES the fader and pan ADDS to it — the same arithmetic the
        layer lanes use, so "automate the master" behaves like "automate a
        layer" instead of inventing a second law. */
    bool masterLevelAutomated = false;
    bool masterPanAutomated   = false;
    std::vector<AutomationPoint> masterLevelPoints;
    std::vector<AutomationPoint> masterPanPoints;
};

//==============================================================================
/** Peak measurements from a render. Used by the UI meters and by the headless
    self-test, which checks that a note really did sound on the synth it was
    routed to rather than merely being labelled that way. */
struct RenderStats
{
    float peak = 0.0f;
    std::vector<std::pair<int, float>> slotPeaks;    // slotId  -> peak
    std::vector<std::pair<int, float>> layerPeaks;   // layerIndex -> peak
    std::vector<std::pair<int, float>> channelPeaks; // channel -> peak

    RenderStats();

    void  addPeak (int slotId, float newPeak);
    float peakForSlot (int slotId) const;
    void  addLayerPeak (int layerIndex, float newPeak);
    float peakForLayer (int layerIndex) const;
    void  addChannelPeak (int channel, float newPeak);
    float peakForChannel (int channel) const;
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

    /** Drops hosted instruments' held notes and effects' tails. Called when the
        transport stops or loops: a note-off that never got sent would leave a
        plugin droning into the next playback. */
    void  allNotesOffHosted();    void  decayMeters (float factor);
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
    std::array<std::atomic<float>, MixerModel::numChannels> channelMeters;
    std::array<std::atomic<float>, 2>                masterMeters;

    /** Reused every block: one stereo bus, one slot at a time. Allocated in
        prepareToPlay, never inside the callback. */
    mutable juce::AudioBuffer<float> mixScratch;

    /** Phase 11: per console channel, plus index 8 for "bypasses the console".
        A layer is not a bus any more: a sheet of notes picks an instrument, and
        the instrument is what a channel receives. */
    static constexpr int numBuses = MixerModel::numChannels + 1;
    mutable std::array<juce::AudioBuffer<float>, numBuses> channelBuses;

    /** Insert memory, 8 channels + master. Audio thread only. */
    mutable std::array<fx::ChainState, MixerModel::numChannels + 1> channelChains;

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

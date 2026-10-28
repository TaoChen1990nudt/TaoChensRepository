#include "PlaybackEngine.h"

#include "PluginHost.h"

#include "Model/AutomationStack.h"
#include "Model/Routing.h"

#include <algorithm>
#include <cmath>

namespace atomic
{

namespace
{
    /** A try-lock that may simply not be held.

        The audio thread must never wait for the message thread, but plugin
        instances only exist on the message thread's schedule. So: try to take
        the plugin lock, and if the message thread is busy adding or removing a
        plugin, render this one block without plugin processing. A block of
        dry signal is a far better outcome than a stalled sound card. */
    class OptionalLock
    {
    public:
        explicit OptionalLock (juce::CriticalSection* lockToUse) : lock (lockToUse)
        {
            if (lock != nullptr)
                held = lock->tryEnter();
        }

        ~OptionalLock()
        {
            if (held)
                lock->exit();
        }

        bool isHeld() const noexcept { return held; }

    private:
        juce::CriticalSection* lock = nullptr;
        bool held = false;
    };

    constexpr double kTwoPi           = 6.283185307179586;
    constexpr double kClickFadeSecond = 0.0025;
    constexpr float  kMasterGain      = 0.22f;
    constexpr float  kPiOver4         = 0.7853981633974483f;

    /** Filter range for the automated synth parameter: 80 Hz (closed) up to
        12 kHz (wide open). Exponential, because pitch and brightness are. */
    constexpr float kFilterMinHz = 80.0f;
    constexpr float kFilterRange = 150.0f;

    /** Stands in for "this schedule carries no mixer channels at all" (the
        headless routing tests publish without one). A namespace-level constant,
        not a temporary: the audio thread must not construct a 64-element array
        on the stack of every block. */
    const std::array<fx::Settings, fx::ChainState::maxSlots> kNoInserts {};

    /** Phase 19: the preview envelope. Full level for the hold, then an
        exponential tail short enough to stay out of the way of the next click,
        long enough to hear the pitch. */
    constexpr double kAuditionHold   = 0.28;   // seconds at full level
    constexpr double kAuditionDecay  = 0.16;   // then exponential, this tau
    constexpr double kAuditionLength = 1.10;   // hard cap, seconds

    /** Phase 39: how long a hosted instrument keeps being rendered after the
        last live note-off. A plugin that stops receiving processBlock calls
        freezes mid-tail, and a pad's four-second release is the normal case,
        not the exception. Long enough for any reasonable release, short enough
        that a stray tail does not keep the device callback awake forever. */
    constexpr double kLivePluginTailSeconds = 6.0;

    double pitchToFrequency (int pitch) noexcept
    {
        return 440.0 * std::pow (2.0, ((double) pitch - 69.0) / 12.0);
    }

    /** PolyBLEP residual: removes the worst of the aliasing on the hard edges.
        Ten lines, and it is the difference between "saw" and "harsh buzz". */
    double polyBlep (double t, double dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }

        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return t * t + t + t + 1.0;
        }

        return 0.0;
    }

    /** phase is in cycles (0..1 per period), dt is cycles per sample. */
    float oscillator (Waveform waveform, double phase, double dt) noexcept
    {
        phase -= std::floor (phase);

        switch (waveform)
        {
            case Waveform::sine:
                return (float) std::sin (kTwoPi * phase);

            case Waveform::saw:
                return (float) (2.0 * phase - 1.0 - polyBlep (phase, dt));

            case Waveform::square:
            {
                double value = phase < 0.5 ? 1.0 : -1.0;
                value += polyBlep (phase, dt);

                double shifted = phase + 0.5;

                if (shifted >= 1.0)
                    shifted -= 1.0;

                value -= polyBlep (shifted, dt);
                return (float) value;
            }

            case Waveform::triangle:
                return (float) (0.8 * (4.0 * std::abs (phase - 0.5) - 1.0));
        }

        return 0.0f;
    }
}

//==============================================================================
RenderStats::RenderStats()
{
    slotPeaks.reserve ((size_t) PlaybackEngine::maxMeteredSlots);
    layerPeaks.reserve ((size_t) PlaybackEngine::maxMeteredLayers);
    channelPeaks.reserve ((size_t) MixerModel::numChannels);
}

void RenderStats::addPeak (int slotId, float newPeak)
{
    for (auto& entry : slotPeaks)
    {
        if (entry.first == slotId)
        {
            entry.second = juce::jmax (entry.second, newPeak);
            return;
        }
    }

    // Phase 70: capacity IS the ceiling. Growing past it would allocate on the
    // audio thread; a slot that does not fit into the metering budget simply
    // does not get a meter, which is the same thing the UI does with slot 17.
    if (slotPeaks.size() < slotPeaks.capacity())
        slotPeaks.emplace_back (slotId, newPeak);
}

float RenderStats::peakForSlot (int slotId) const
{
    for (const auto& entry : slotPeaks)
        if (entry.first == slotId)
            return entry.second;

    return 0.0f;
}

void RenderStats::addLayerPeak (int layerIndex, float newPeak)
{
    for (auto& entry : layerPeaks)
    {
        if (entry.first == layerIndex)
        {
            entry.second = juce::jmax (entry.second, newPeak);
            return;
        }
    }

    if (layerPeaks.size() < layerPeaks.capacity())
        layerPeaks.emplace_back (layerIndex, newPeak);
}

float RenderStats::peakForLayer (int layerIndex) const
{
    for (const auto& entry : layerPeaks)
        if (entry.first == layerIndex)
            return entry.second;

    return 0.0f;
}

void RenderStats::addChannelPeak (int channel, float leftPeak, float rightPeak)
{
    for (auto& entry : channelPeaks)
    {
        if (entry.channel == channel)
        {
            entry.left  = juce::jmax (entry.left,  leftPeak);
            entry.right = juce::jmax (entry.right, rightPeak);
            return;
        }
    }

    if (channelPeaks.size() < channelPeaks.capacity())
        channelPeaks.push_back ({ channel, leftPeak, rightPeak });
}

float RenderStats::peakForChannel (int channel) const
{
    for (const auto& entry : channelPeaks)
        if (entry.channel == channel)
            return entry.loudest();

    return 0.0f;
}

float RenderStats::peakForChannelSide (int channel, int side) const
{
    for (const auto& entry : channelPeaks)
        if (entry.channel == channel)
            return side == 0 ? entry.left : entry.right;

    return 0.0f;
}

//==============================================================================
PlaybackEngine::PlaybackEngine()
{
    for (auto& meter : meters)
        meter.store (0.0f);

    for (auto& meter : layerMeters)
        meter.store (0.0f);

    for (auto& meter : channelMetersL)
        meter.store (0.0f);

    for (auto& meter : channelMetersR)
        meter.store (0.0f);

    for (auto& meter : masterMeters)
        meter.store (0.0f);

    mixScratch.setSize (2, 1024);

    // Delay lines must exist before the first render, even if nobody ever calls
    // setSampleRate (the headless tests render straight from a fresh engine).
    // Phase 25: only the strips a schedule actually references get memory —
    // publish() tops this up as the console grows.
    prepareChannelChains (MixerModel::defaultChannels);
}

void PlaybackEngine::prepareChannelChains (int count)
{
    const int wanted = juce::jlimit (0, (int) MixerModel::numChannels, count);

    for (int i = chainsReady.load (std::memory_order_relaxed); i < wanted; ++i)
        channelChains[(size_t) i].prepare (sampleRate);

    if (wanted > chainsReady.load (std::memory_order_relaxed))
        chainsReady.store (wanted, std::memory_order_release);

    if (! masterChainReady.load (std::memory_order_relaxed))
    {
        channelChains[(size_t) MixerModel::masterChannel].prepare (sampleRate);
        masterChainReady.store (true, std::memory_order_release);
    }
}

void PlaybackEngine::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0)
        return;

    sampleRate = newSampleRate;

    // Insert memory is sized by sample rate (delay buffers), so it is rebuilt
    // here — on the message thread — never in the callback. Phase 25: only the
    // chains that were ever prepared, which keeps the cost proportional to the
    // strips in use instead of the 127-strip ceiling.
    const int ready = chainsReady.load (std::memory_order_relaxed);

    for (int i = 0; i < ready; ++i)
        channelChains[(size_t) i].prepare (sampleRate);

    if (masterChainReady.load (std::memory_order_relaxed))
        channelChains[(size_t) MixerModel::masterChannel].prepare (sampleRate);
}

void PlaybackEngine::prepareToPlay (double newSampleRate, int expectedBlockSize)
{
    setSampleRate (newSampleRate);

    const int required = juce::jmax (1024, expectedBlockSize);

    if (mixScratch.getNumSamples() < required || mixScratch.getNumChannels() < 2)
        mixScratch.setSize (2, required, false, false, false);

    // Hosted plugins are prepared for PluginHost::maxBlockSize, so a window
    // longer than the device block (an offline probe, a long renderWindow) is
    // chunked rather than handed to a plugin that never sized for it.
    const int pluginBlock = juce::jmax (PluginHost::maxBlockSize, required);

    if (pluginBuffer.getNumSamples() < pluginBlock || pluginBuffer.getNumChannels() < 2)
        pluginBuffer.setSize (2, pluginBlock, false, false, false);

    pluginMidi.ensureSize (8192);

    // Phase 39: the live plugin path renders in chunks of at most this, so the
    // scratch only ever has to hold one chunk — and it is sized HERE, on the
    // message thread, because a buffer that resizes itself inside the audio
    // callback is an allocation with a real-time deadline.
    const int liveBlock = juce::jmax (PluginHost::maxBlockSize, required);

    if (livePluginBuffer.getNumSamples() < liveBlock || livePluginBuffer.getNumChannels() < 2)
        livePluginBuffer.setSize (2, liveBlock, false, false, false);

    liveMidi.ensureSize (2048);

    for (auto& bus : channelBuses)
        if (bus.getNumSamples() < required || bus.getNumChannels() < 2)
            bus.setSize (2, required, false, false, false);

    resetMixState();
}

void PlaybackEngine::setPositionBeats (double beats)
{
    positionBeats.store (juce::jlimit (0.0, juce::jmax (0.0, getLengthBeats()), beats),
                         std::memory_order_relaxed);
}

double PlaybackEngine::getSecondsPerBeat() const
{
    if (auto current = getSchedule())
        return current->secondsPerBeat;

    return 60.0 / defaultBpm;
}

double PlaybackEngine::getPosition() const
{
    return getPositionBeats() * getSecondsPerBeat();
}

void PlaybackEngine::setPosition (double seconds)
{
    setPositionBeats (seconds / juce::jmax (1.0e-6, getSecondsPerBeat()));
}

//==============================================================================
void PlaybackEngine::publishSchedule (const LayerStack& stack, const RackModel& rack,
                                      double bpm, double requestedLengthBeats,
                                      const MixerModel* mixer)
{
    auto fresh = std::make_shared<PublishedSchedule>();

    const double tempo          = juce::jlimit (10.0, 400.0, bpm);
    const double secondsPerBeat = 60.0 / tempo;

    fresh->secondsPerBeat = secondsPerBeat;

    //-- Solo is resolved ONCE, here ------------------------------------------
    // The audio thread gets booleans, not flags. Two independent solo groups:
    // layers and synths, because they answer different questions ("which sheet
    // do I want to hear" vs "which instrument").
    bool anyLayerSolo = false;
    bool anySlotSolo  = false;

    for (int i = 0; i < stack.getNumLayers(); ++i)
        if (auto* layer = stack.getLayer (i))
            if (layer->isSoloed())
                anyLayerSolo = true;

    for (int i = 0; i < rack.getNumSlots(); ++i)
        if (auto* slot = rack.getSlot (i))
            if (slot->soloed)
                anySlotSolo = true;

    //-- Layers ---------------------------------------------------------------
    for (int layerIndex = 0; layerIndex < stack.getNumLayers(); ++layerIndex)
    {
        auto* layer = stack.getLayer (layerIndex);

        if (layer == nullptr)
            continue;

        LayerMix mix;
        mix.layerIndex        = layerIndex;
        mix.audible           = ! layer->isMuted() && (! anyLayerSolo || layer->isSoloed());
        mix.gain              = layer->getGainLinear();
        mix.pan               = layer->getPan();

        // Both lanes are frozen, not just the one the panel happens to show.
        const auto& volumeLane = layer->automationFor (LayerAutomationTarget::volume);
        const auto& panLane    = layer->automationFor (LayerAutomationTarget::pan);

        mix.volumeAutomated = volumeLane.isEnabled();
        mix.panAutomated    = panLane.isEnabled();
        mix.volumePoints    = mix.volumeAutomated ? volumeLane.getPoints() : std::vector<AutomationPoint> {};
        mix.panPoints       = mix.panAutomated    ? panLane.getPoints()    : std::vector<AutomationPoint> {};

        fresh->layers.push_back (std::move (mix));
    }

    //-- Console channels ----------------------------------------------------
    // Phase 11: a channel belongs to the mixer, not to a layer. Its inserts are
    // frozen whether or not anything is routed here; whether it is *processed*
    // is decided at render time by "did any voice arrive".
    //
    // Phase 23: the bus slots exist in the engine whether or not a mixer was
    // handed in, so they are published unconditionally. A schedule without a
    // mixer then renders exactly the pre-Phase-23 way: eight strips, all going
    // to the master, in index order.
    //
    // Phase 25: the live count, not the 127-strip ceiling. Publishing 127
    // channels would make the render loop and the topology sort pay for strips
    // nobody created, and `channels` is indexed BY CHANNEL NUMBER, so the array
    // has to be exactly as long as the console anyway.
    const int liveChannels = mixer != nullptr ? mixer->getChannelCount()
                                              : MixerModel::defaultChannels;

    // The chains these channels will run in the callback have to exist first.
    prepareChannelChains (liveChannels);

    for (int channel = 0; channel < liveChannels; ++channel)
    {
        ChannelMix mix;
        mix.channel = channel;

        if (mixer != nullptr)
        {
            mix.inserts = mixer->getInserts (channel);
            mix.output  = mixer->getChannelOutput (channel);
            mix.mono    = mixer->isChannelMono (channel);
        }

        fresh->channels.push_back (std::move (mix));
    }

    //-- Phase 23: the summing order ----------------------------------------
    // A strip that feeds another strip has to be summed BEFORE its destination
    // runs its own inserts, otherwise the signal would land in a bus that has
    // already been read. Sorting here (message thread, once per edit) keeps the
    // audio thread to a flat loop over a list it does not have to understand.
    {
        std::array<bool, MixerModel::numChannels> placed {};

        const auto feeds = [&fresh] (int source, int destination)
        {
            return source != destination
                     && juce::isPositiveAndBelow (source, (int) fresh->channels.size())
                     && fresh->channels[(size_t) source].output == destination;
        };

        for (int step = 0; step < liveChannels; ++step)
        {
            int pick = -1;

            for (int candidate = 0; candidate < liveChannels && pick < 0; ++candidate)
            {
                if (placed[(size_t) candidate])
                    continue;

                bool ready = true;

                for (int other = 0; other < liveChannels && ready; ++other)
                    if (! placed[(size_t) other] && feeds (other, candidate))
                        ready = false;

                if (ready)
                    pick = candidate;
            }

            // A loop cannot be rendered, so nothing in it is ever "ready". The
            // model refuses to build one; this is what keeps a hand-edited file
            // from stalling the publish instead of merely sounding odd.
            if (pick < 0)
                for (int candidate = 0; candidate < liveChannels && pick < 0; ++candidate)
                    if (! placed[(size_t) candidate])
                        pick = candidate;

            placed[(size_t) pick] = true;
            fresh->channelOrder.push_back (pick);
        }
    }

    //-- Synth slots ----------------------------------------------------------
    for (int i = 0; i < rack.getNumSlots(); ++i)
    {
        auto* slot = rack.getSlot (i);

        if (slot == nullptr)
            continue;

        SlotMix mix;
        mix.slotId   = slot->id;
        mix.pluginId = slot->pluginId;
        mix.sampler  = slot->isSampler();
        mix.muted    = slot->muted;
        mix.soloed   = slot->soloed;
        mix.audible  = ! slot->muted && (! anySlotSolo || slot->soloed);
        mix.gain    = slot->getGainLinear();
        mix.pan     = slot->pan;
        mix.waveform = slot->waveform;

        // Out-of-range values are stored as -1 by the model, but a hand-built
        // schedule (or a future file) may not be, so the publish step is the
        // last place that guarantees "a bus index that exists".
        mix.mixerChannel = juce::isPositiveAndBelow (slot->mixerChannel, MixerModel::numChannels)
                             ? slot->mixerChannel
                             : -1;

        const auto& filterLane = slot->automationFor (SlotAutomationTarget::filter);
        const auto& levelLane  = slot->automationFor (SlotAutomationTarget::level);

        mix.filterAutomated = filterLane.isEnabled();
        mix.levelAutomated  = levelLane.isEnabled();
        mix.filterPoints    = mix.filterAutomated ? filterLane.getPoints() : std::vector<AutomationPoint> {};
        mix.levelPoints     = mix.levelAutomated  ? levelLane.getPoints()  : std::vector<AutomationPoint> {};

        // Phase 43: pan / mute / solo. Three more frozen curves, the same shape
        // as the two above — the audio thread never looks at the model.
        const auto& panLane  = slot->automationFor (SlotAutomationTarget::pan);
        const auto& muteLane = slot->automationFor (SlotAutomationTarget::mute);
        const auto& soloLane = slot->automationFor (SlotAutomationTarget::solo);

        mix.panAutomated  = panLane.isEnabled();
        mix.muteAutomated = muteLane.isEnabled();
        mix.soloAutomated = soloLane.isEnabled();
        mix.panPoints     = mix.panAutomated  ? panLane.getPoints()  : std::vector<AutomationPoint> {};
        mix.mutePoints    = mix.muteAutomated ? muteLane.getPoints() : std::vector<AutomationPoint> {};
        mix.soloPoints    = mix.soloAutomated ? soloLane.getPoints() : std::vector<AutomationPoint> {};

        // A solo curve makes "who is soloed" move with the playhead, so the
        // renderer cannot use the constant it used to cache. The publish step
        // still freezes the curves; the renderer asks `isSoloedAt` per block.
        fresh->slots.push_back (std::move (mix));
    }

    //-- Notes ----------------------------------------------------------------
    double contentBeats = 0.0;

    for (int layerIndex = 0; layerIndex < stack.getNumLayers(); ++layerIndex)
    {
        auto* layer = stack.getLayer (layerIndex);

        if (layer == nullptr)
            continue;

        // Phase 16: groups before their notes, so the audio thread can look a
        // group up while it renders the notes that point at it.
        for (const auto& group : layer->getGroups())
        {
            GroupMix mix;
            mix.groupId = group.getId();
            mix.gain    = group.getGainLinear();
            mix.pan     = group.getPan();
            mix.muted   = group.isMuted();      // Phase 35: folded into gainAt()

            const auto& lane = group.getLevelAutomation();
            mix.levelAutomated = lane.isEnabled();

            if (mix.levelAutomated)
                mix.levelPoints = lane.getPoints();

            // Phase 20: the pan lane is frozen the same way. Two lanes, one
            // rule: whatever the model says at publish time is what the block
            // renders, and the audio thread never looks at the group.
            const auto& panLane = group.getPanAutomation();
            mix.panAutomated = panLane.isEnabled();

            if (mix.panAutomated)
                mix.panPoints = panLane.getPoints();

            fresh->groups.push_back (std::move (mix));

            //-- Phase 24: the group's plugin lane ---------------------------
            // A group still has no signal path of its own, so this lane cannot
            // be "the group's parameter". It writes a parameter of the synth
            // the group is MOUNTED on, over the beats the group covers — which
            // is what "自动化编组挂载的音源" has to mean. The target synth is
            // resolved exactly like its notes' output is, so the group cannot
            // drive a plugin that none of its members play through.
            if (group.drivesPluginParam() && group.getPluginAutomation().isEnabled())
            {
                const int groupSlot = group.followsLayerOutput()
                                        ? routing::layerSlotId (*layer, rack)
                                        : (rack.findSlot (group.getOutputSlotId()) != nullptr
                                             ? group.getOutputSlotId()
                                             : routing::layerSlotId (*layer, rack));

                const auto* mount = rack.findSlot (groupSlot);

                if (mount != nullptr && mount->pluginId > 0)
                {
                    double first = 0.0, last = 0.0;
                    bool   any  = false;

                    for (const auto& member : layer->getNotes())
                    {
                        if (member.groupId != group.getId())
                            continue;

                        first = any ? juce::jmin (first, member.startBeat) : member.startBeat;
                        last  = any ? juce::jmax (last,  member.endBeat())  : member.endBeat();
                        any   = true;
                    }

                    if (any && last > first)
                    {
                        PluginParamCurve curve;
                        curve.pluginId   = mount->pluginId;
                        curve.paramIndex = group.getPluginParam();
                        curve.startBeat  = first;
                        curve.endBeat    = last;
                        curve.priority   = 1;                  // the coarser curve
                        curve.points     = group.getPluginAutomation().getPoints();
                        fresh->pluginParams.push_back (std::move (curve));
                    }
                }
            }
        }

        for (const auto& note : layer->getNotes())
        {
            // THE routing decision. Made once, here, for every note.
            const auto route = routing::resolve (*layer, note, rack);

            if (! route.isValid())
                continue;                    // empty rack: nothing to play into

            const auto* slot = rack.findSlot (route.slotId);

            if (slot == nullptr)
                continue;

            ScheduledNote scheduled;
            scheduled.slotId       = slot->id;
            scheduled.waveform     = slot->waveform;
            scheduled.gain         = juce::jlimit (0.0f, 1.0f, note.velocity);
            scheduled.pitch        = note.pitch;
            scheduled.layerIndex   = layerIndex;
            scheduled.noteId       = note.id;
            scheduled.startSeconds = note.startBeat * secondsPerBeat;
            scheduled.endSeconds   = juce::jmax (scheduled.startSeconds + 0.01,
                                                 note.endBeat() * secondsPerBeat);
            scheduled.envelope     = note.envelope;      // snapshot by value
            scheduled.overridden   = route.overridden;
            scheduled.modTarget    = note.modTarget;
            scheduled.groupId      = note.groupId;
            scheduled.groupOccurrence = note.groupOccurrence;

            // Tabulate the pitch integral once, here, so the audio thread only
            // ever does a table lookup. See ScheduledNote::pitchIntegral for why
            // a pitch curve needs an integral at all.
            if (note.modTarget == NoteModTarget::pitch && note.envelope.isEnabled())
            {
                constexpr int steps = ScheduledNote::pitchIntegralSteps;

                scheduled.pitchIntegral.assign ((size_t) steps + 1, 0.0f);

                auto bendAt = [&note] (float u)
                {
                    // env 1.0 -> no bend, env 0.0 -> one octave down.
                    return std::pow (2.0f, note.envelope.valueAt (u) - 1.0f) - 1.0f;
                };

                for (int i = 1; i <= steps; ++i)
                {
                    const float u0 = (float) (i - 1) / (float) steps;
                    const float u1 = (float) i / (float) steps;

                    // Trapezoid: 1/64 is small enough that the error is far
                    // below a cent, and it keeps this a single pass.
                    scheduled.pitchIntegral[(size_t) i] =
                        scheduled.pitchIntegral[(size_t) i - 1]
                          + 0.5f * (bendAt (u0) + bendAt (u1)) / (float) steps;
                }
            }

            contentBeats = juce::jmax (contentBeats, note.endBeat());
            fresh->notes.push_back (scheduled);

            //-- Phase 24: the note's envelope driving a plugin parameter ----
            // The note's curve is normalised; the schedule speaks absolute
            // beats. Converting here (once) instead of on the audio thread is
            // the same trade the pitch integral makes: pay it at publish time.
            if (note.drivesPluginParam() && note.envelope.isEnabled()
                  && slot->pluginId > 0)
            {
                PluginParamCurve curve;
                curve.pluginId   = slot->pluginId;
                curve.paramIndex = note.modPluginParam;
                curve.startBeat  = note.startBeat;
                curve.endBeat    = note.endBeat();
                curve.priority   = 2;                  // the finer curve; multiplies #1

                for (const auto& p : note.envelope.getPoints())
                    curve.points.push_back ({ note.startBeat
                                                + (double) p.time * juce::jmax (1.0e-6, note.lengthBeats),
                                              p.value });

                fresh->pluginParams.push_back (std::move (curve));
            }
        }

        //-- Phase 17: audio clips ------------------------------------------
        // Phase 27: a clip resolves through routing::resolveSample, which is
        // the only place "which sampler plays this, and through which bus" is
        // decided. It deliberately does NOT consult the clip's group: a group's
        // mount is a statement about its NOTES, and a clip that changed its
        // audio because somebody re-mounted the group would be exactly the
        // surprise this phase set out to avoid.
        for (const auto& clip : layer->getSamples())
        {
            const auto res = routing::resolveSample (*layer, clip, rack);

            if (res.missingAudio)
                continue;               // missing file: a hole in the audio, not a crash

            if (res.slotId < 0)
                continue;               // nowhere to come out of

            ScheduledClip scheduled;
            scheduled.slotId       = res.slotId;
            scheduled.layerIndex   = layerIndex;
            scheduled.clipId       = clip.id;
            scheduled.startSeconds = clip.startBeat * secondsPerBeat;
            scheduled.endSeconds   = juce::jmax (scheduled.startSeconds + 0.01,
                                                 clip.endBeat() * secondsPerBeat);
            scheduled.gain         = gain::fromDb (clip.gainDb);
            scheduled.audio        = res.audio;

            // Phase 29: the window, in seconds. A clip whose offset points
            // past the end of its file is left alone — the render loop stops
            // at the last frame anyway, and clamping here would silently move
            // the audio the user is about to trim.
            scheduled.sourceOffsetSeconds = juce::jmax (0.0, clip.sourceOffsetSeconds);

            // Phase 49: the block's volume curve, normalised over the block
            // like a note's envelope. The x-axis becomes seconds here, once —
            // the render loop measures elapsed time in seconds and must not
            // know what a beat is. A curve with fewer than two points cannot
            // interpolate and is left as unity rather than half-published.
            if (clip.volumeCurve.isEnabled())
            {
                const double blockSeconds = juce::jmax (1.0e-6,
                                                        clip.lengthBeats * secondsPerBeat);

                for (const auto& p : clip.volumeCurve.getPoints())
                    scheduled.gainCurve.push_back ({ (double) p.time * blockSeconds, p.value });

                scheduled.gainAutomated = scheduled.gainCurve.size() >= 2;
            }

            // Phase 50: the pan curve rides the same conversion. A ping-pong
            // clip is the reason this exists; keeping its x in seconds means the
            // render loop's only clock is `elapsed`, exactly like the gain
            // curve, and neither curve has to know what a beat is.
            if (clip.panCurve.isEnabled())
            {
                const double blockSeconds = juce::jmax (1.0e-6,
                                                        clip.lengthBeats * secondsPerBeat);

                for (const auto& p : clip.panCurve.getPoints())
                    scheduled.panCurve.push_back ({ (double) p.time * blockSeconds,
                                                    juce::jlimit (0.0f, 1.0f, p.value) });

                scheduled.panAutomated = scheduled.panCurve.size() >= 2;
            }

            scheduled.reversed = clip.reversed;

            // Phase 31: the stretch ratio is "what the file would take" over
            // "what the block gives it". Computed from the SAME resolver the
            // block's drawing uses (routing::resolveSample), so a clip mounted
            // on a sampler stretches against the file that actually plays.
            //
            // Measured after the start offset, not against the whole file: a
            // head trim moves the window without changing how much audio is
            // squeezed into it, and a ratio that moved with it would detune the
            // block every time its left edge was nudged.
            const double windowSeconds = juce::jmax (1.0e-4, clip.lengthBeats * secondsPerBeat);
            const double naturalSeconds = juce::jmax (0.0, res.audio->lengthSeconds()
                                                            - scheduled.sourceOffsetSeconds);

            scheduled.stretch = clipstretch::Playback::make (clip.stretchMode,
                                                             naturalSeconds / windowSeconds,
                                                             clip.pitchRangeSemitones);

            // Phase 35: membership, for the group's trim only. Nothing above
            // this line consulted it — `res` was resolved from the mount and the
            // layer, never from the group — so adding this field cannot change
            // what plays, only how loud.
            scheduled.groupId = clip.groupId;
            scheduled.groupOccurrence = clip.groupOccurrence;

            contentBeats = juce::jmax (contentBeats, clip.endBeat());
            fresh->clips.push_back (std::move (scheduled));
        }
    }

    std::sort (fresh->notes.begin(), fresh->notes.end(),
               [] (const ScheduledNote& a, const ScheduledNote& b)
               { return a.startSeconds < b.startSeconds; });

    std::sort (fresh->clips.begin(), fresh->clips.end(),
               [] (const ScheduledClip& a, const ScheduledClip& b)
               { return a.startSeconds < b.startSeconds; });

    // Phase 24: ascending priority, so the audio thread applies the whole list
    // in order. Phase 36: the order no longer decides a winner — curves on the
    // same parameter are multiplied — but a stable order still means two runs
    // over the same song produce the same schedule, which is what makes an
    // offline export comparable to a live render.
    // Phase 42: the SLOT's own plugin lane.
    //
    // It is the coarsest curve there is — the whole project, at the level of
    // "the synth" — where a group's curve covers its members and a note's
    // envelope covers the note. All three multiply on the way out
    // (automation::stackPluginParam), so a slot at 0.5 under a group at 0.5 is a
    // quarter of the parameter's range: the same arithmetic the level stack has
    // used since Phase 36.
    //
    // Published here, after the loops above, because the window it covers is the
    // project's length — and that is not known until every note and clip has
    // been seen.
    {
        const double curveLength = juce::jmax (1.0, contentBeats);

        for (int i = 0; i < rack.getNumSlots(); ++i)
        {
            const auto* slot = rack.getSlot (i);

            if (slot == nullptr || slot->pluginId <= 0 || ! slot->drivesPluginParam())
                continue;

            const auto& lane = slot->getPluginAutomation();

            if (! lane.isEnabled() || lane.getPoints().empty())
                continue;

            PluginParamCurve curve;
            curve.pluginId   = slot->pluginId;
            curve.paramIndex = slot->getPluginParam();
            curve.startBeat  = 0.0;
            curve.endBeat    = curveLength;
            curve.priority   = 0;
            curve.points     = lane.getPoints();

            fresh->pluginParams.push_back (std::move (curve));
        }
    }

    std::stable_sort (fresh->pluginParams.begin(), fresh->pluginParams.end(),
                      [] (const PluginParamCurve& a, const PluginParamCurve& b)
                      { return a.priority < b.priority; });

    //-- Master strip ---------------------------------------------------------
    if (mixer != nullptr)
    {
        fresh->masterInserts = mixer->getInserts (MixerModel::masterChannel);
        fresh->masterGain    = mixer->getMasterGainLinear();
        fresh->masterPan     = mixer->getMasterPan();
        fresh->masterMono    = mixer->isMasterMono();

        const auto& levelLane = mixer->automationFor (0);
        const auto& panLane   = mixer->automationFor (1);
        const auto& muteLane  = mixer->automationFor (2);

        fresh->masterLevelAutomated = levelLane.isEnabled();
        fresh->masterPanAutomated   = panLane.isEnabled();
        fresh->masterMuteAutomated  = muteLane.isEnabled();
        fresh->masterLevelPoints    = levelLane.getPoints();
        fresh->masterPanPoints      = panLane.getPoints();
        fresh->masterMutePoints     = muteLane.getPoints();
    }

    // Never shorter than the project grid, never shorter than the content, and
    // never zero — a zero-length timeline would make the transport divide by it.
    fresh->contentBeats = contentBeats;
    fresh->lengthBeats = juce::jmax (juce::jmax (timelineBeats, requestedLengthBeats),
                                     juce::jmax (contentBeats, 1.0));

    const double newLengthBeats = fresh->lengthBeats;
    const double newLengthSecs  = fresh->lengthSeconds();

    {
        // The lock covers shared_ptr assignments and nothing else. No note is
        // copied and no audio is computed while it is held.
        const juce::SpinLock::ScopedLockType guard (scheduleLock);

        // Phase 70: park the outgoing schedule before dropping the reference to
        // it. The audio thread reads a bare pointer now, so whoever was looking
        // at the previous one has no count of its own — this list is what keeps
        // it alive until the block that read it has finished. Two is the margin:
        // the audio thread can only ever be one publish behind.
        scheduleRetired.push_back (std::move (schedule));
        schedule = std::move (fresh);

        while (scheduleRetired.size() > 2)
            scheduleRetired.erase (scheduleRetired.begin());
    }

    publishedSchedule.store (schedule.get(), std::memory_order_release);

    lengthBeats.store (newLengthBeats, std::memory_order_relaxed);
    lengthSeconds.store (newLengthSecs, std::memory_order_relaxed);

    // A tempo or length change must not leave the playhead past the end.
    if (getPositionBeats() > newLengthBeats)
        setPositionBeats (0.0);
}

std::shared_ptr<const PublishedSchedule> PlaybackEngine::getSchedule() const
{
    const juce::SpinLock::ScopedLockType guard (scheduleLock);
    return schedule;
}

void PlaybackEngine::renderInto (const PublishedSchedule& current, double startSeconds, int numSamples,
                                 juce::AudioBuffer<float>& output, RenderStats* stats) const
{
    output.clear();

    if (numSamples <= 0 || sampleRate <= 0.0 || output.getNumChannels() <= 0)
        return;

    if (mixScratch.getNumChannels() < 2)
        mixScratch.setSize (2, juce::jmax (1024, numSamples), false, false, false);

    if (mixScratch.getNumSamples() < numSamples)
        mixScratch.setSize (2, numSamples, false, false, false);

    const int    numChannels = output.getNumChannels();
    const double spb         = juce::jmax (1.0e-4, current.secondsPerBeat);
    const double blockEnd    = startSeconds + (double) numSamples / sampleRate;
    const double startBeat   = startSeconds / spb;
    const double endBeat     = blockEnd / spb;

    //-- Hosted plugins: one try-lock for the whole block ---------------------
    // Held across every plugin call so an instance cannot be destroyed between
    // the instrument stage and the insert stage.
    OptionalLock pluginGuard (pluginHost != nullptr ? &pluginHost->getLock() : nullptr);
    const bool pluginsLive = pluginGuard.isHeld();

    if (pluginsLive && pluginPanicRequested.exchange (false))
        pluginHost->resetAllInstancesWhileLocked();

    //-- Layer ramps for this block ------------------------------------------
    // Evaluated twice (in and out) instead of per sample: automation moves
    // slowly compared to a buffer, and a linear ramp between the two ends is
    // both cheaper and smoother than a staircase of block-sized steps.
    std::array<LayerBlock, maxLayersPerBlock> layerBlocks {};
    const int numLayerMixes = juce::jmin ((int) current.layers.size(), maxLayersPerBlock);

    for (int i = 0; i < numLayerMixes; ++i)
    {
        const auto& mix = current.layers[(size_t) i];
        auto& block = layerBlocks[(size_t) i];

        block.layerIndex = mix.layerIndex;
        block.gain0 = mix.gainAt (startBeat);
        block.gain1 = mix.gainAt (endBeat);
        block.pan0  = mix.panAt (startBeat);
        block.pan1  = mix.panAt (endBeat);
    }

    const float invNumSamples = 1.0f / (float) numSamples;

    //-- Phase 16: group level ramps for this block --------------------------
    // Evaluated in and out, exactly like the layer faders: one curve lookup per
    // group per block instead of one per note per sample.
    std::array<GroupBlock, maxGroupMixesPerBlock> groupBlocks {};
    const int numGroupMixes = juce::jmin ((int) current.groups.size(), maxGroupMixesPerBlock);

    for (int i = 0; i < numGroupMixes; ++i)
    {
        const auto& mix = current.groups[(size_t) i];

        groupBlocks[(size_t) i].groupId = mix.groupId;
        groupBlocks[(size_t) i].gain0   = mix.gainAt (startBeat);
        groupBlocks[(size_t) i].gain1   = mix.gainAt (endBeat);
        groupBlocks[(size_t) i].pan0    = mix.panAt (startBeat);
        groupBlocks[(size_t) i].pan1    = mix.panAt (endBeat);
    }

    auto groupBlockFor = [&groupBlocks, numGroupMixes] (int groupId) -> const GroupBlock*
    {
        if (groupId < 0)
            return nullptr;

        for (int i = 0; i < numGroupMixes; ++i)
            if (groupBlocks[(size_t) i].groupId == groupId)
                return &groupBlocks[(size_t) i];

        return nullptr;
    };

    //-- Phase 11: buses are console channels, not layers --------------------
    // Index 0..7 is a channel; index 8 is "this synth bypasses the console and
    // goes straight to the master strip". A layer is a sheet of notes: it picks
    // an instrument, and the instrument is what a channel receives.
    std::array<bool, numBuses> busUsed {};

    for (auto& bus : channelBuses)
    {
        if (bus.getNumChannels() < 2 || bus.getNumSamples() < numSamples)
            bus.setSize (2, numSamples, false, false, false);

        // Always clear: setSize(..., avoidReallocating) leaves the memory as it
        // was, and a channel bus that starts with last block's (or worse, the
        // allocator's) samples is how a mix ends up with a 1e30 peak.
        bus.clear (0, numSamples);
    }

    // Layer peaks are measured at the voice stage now that layers have no bus
    // of their own. The layer panel's meter must still mean "this sheet is
    // producing sound".
    std::array<float, maxMeteredLayers> layerVoicePeak {};

    auto busIndexFor = [] (int mixerChannel)
    {
        return juce::isPositiveAndBelow (mixerChannel, MixerModel::numChannels)
                 ? mixerChannel
                 : MixerModel::numChannels;
    };

    //-- Phase 43: who is soloed, for THIS block ------------------------------
    // Solo is the only control whose answer is not local: it decides whether a
    // slot that is soloing nothing is heard at all. While solo was a boolean
    // that question had one answer and publish time could freeze it; a solo
    // CURVE makes it a function of the playhead, so it is asked once per block.
    //
    // One loop, not two: when no solo lane exists `isSoloedAt` is the same
    // boolean the publish step read, so the branch would buy nothing but a
    // second path to disagree with this one.
    bool anySoloNow = false;

    for (const auto& s : current.slots)
        if (s.isSoloedAt (startBeat))
        {
            anySoloNow = true;
            break;
        }

    //-- Stage 1: voices -> per-channel stereo bus ---------------------------
    // A note contributes to exactly one channel (its layer) and one synth (its
    // resolved output). The channel bus has to exist independently of the synth
    // bus, because a delay on channel 3 must not delay channel 4's material
    // that happens to share the same synth.
    for (const auto& slot : current.slots)
    {
        // A hosted instrument renders its own voices in Stage 1b; running the
        // internal oscillator for it as well would double every note.
        if (slot.pluginId > 0)
            continue;

        if (! slot.isAudibleAt (startBeat, anySoloNow))
            continue;

        const float slotGain0 = slot.gainAt (startBeat);
        const float slotGain1 = slot.gainAt (endBeat);

        // Phase 43: the strip's pan, sampled at both ends of the block so a pan
        // curve sweeps the way the level curve already ramps.
        const float slotPan0 = slot.panAt (startBeat);
        const float slotPan1 = slot.panAt (endBeat);

        if (slotGain0 <= 0.0f && slotGain1 <= 0.0f)
            continue;                           // muted synth

        // The synth filter runs on the mono voice signal, before panning. It is
        // a linear one-pole with continuous state, so filtering each note in
        // turn and summing gives the same result as filtering the sum.
        const bool  filterOn = slot.filterAutomated;
        float       filterPole = 1.0f;

        if (filterOn)
        {
            const float openness = slot.filterOpennessAt (startBeat);
            const float cutoff   = kFilterMinHz * std::pow (kFilterRange, openness);
            filterPole = std::exp (-(float) kTwoPi * cutoff / (float) sampleRate);
        }

        auto  filterIndex = (size_t) juce::jlimit (0, maxMeteredSlots - 1, (int) (&slot - current.slots.data()));
        float filterZ = slotFilters[filterIndex].zL;

        bool  anyVoice = false;
        float voicePeak = 0.0f;

        for (const auto& note : current.notes)
        {
            // Phase 27: a sampler slot has no oscillator. Its job is the clip
            // loop further down; running voices through it would mean a note
            // routed there by accident makes a saw wave out of a slot the user
            // believes only holds a file.
            if (slot.sampler)
                break;

            if (note.slotId != slot.slotId)
                continue;

            if (note.startSeconds >= blockEnd)
                break;                          // sorted by start time

            if (note.endSeconds <= startSeconds)
                continue;

            if (note.gain <= 0.0f || note.endSeconds <= note.startSeconds)
                continue;

            const int layerIndex = note.layerIndex;

            if (! juce::isPositiveAndBelow (layerIndex, numLayerMixes))
                continue;

            const auto& lb = layerBlocks[(size_t) layerIndex];

            if (lb.gain0 <= 0.0f && lb.gain1 <= 0.0f)
                continue;                       // muted, un-soloed, or fader at -inf

            // Phase 16: the group trim. A note that is not in a group gets 1.0,
            // so the maths below is the same for grouped and ungrouped notes.
            const auto* gb = groupBlockFor (note.groupId);
            const float groupGain0 = gb != nullptr ? gb->gain0 : 1.0f;
            const float groupGain1 = gb != nullptr ? gb->gain1 : 1.0f;

            if (groupGain0 <= 0.0f && groupGain1 <= 0.0f)
                continue;                       // the group curve has faded it out

            const int first = juce::jmax (0, (int) std::ceil ((note.startSeconds - startSeconds) * sampleRate - 1.0e-9));
            const int last  = juce::jmin (numSamples, (int) std::ceil ((note.endSeconds - startSeconds) * sampleRate));

            if (last <= first)
                continue;

            // Phase is in CYCLES, so it is frequency * seconds. `increment` is
            // cycles per sample and is only the step size for the anti-alias
            // correction — multiplying it by elapsed seconds was the Phase 3 bug
            // that made every note a slow ramp instead of a tone.
            const double frequency = pitchToFrequency (note.pitch);
            const double increment = frequency / sampleRate;
            const double length    = note.endSeconds - note.startSeconds;

            // Pan is the layer's pan PLUS the synth's pan PLUS the group's PLUS
            // whatever the note's own curve adds — constant-power. The stages are
            // summed rather than multiplied because a pan is an angle: adding two
            // half-lefts should not cancel out. Phase 20 makes the group's term a
            // ramp (pan0..pan1) so a group pan curve sweeps instead of stepping
            // per block.
            //
            // Phase 36: the sum is `automation::stackPan`, the same function the
            // overlay painter calls. The macro term is the layer and synth pans
            // added together because those two are one layer of the stack — "how
            // far off centre is the mix" — and the arithmetic is associative
            // either way, so merging them costs nothing and keeps the stack at
            // three terms everywhere.
            const float groupPan0 = gb != nullptr ? gb->pan0 : 0.0f;
            const float groupPan1 = gb != nullptr ? gb->pan1 : 0.0f;

            const float macroPan0 = lb.pan0 + slotPan0;
            const float macroPan1 = lb.pan1 + slotPan1;

            const float p0 = automation::stackPan (macroPan0, groupPan0, 0.0f);
            const float p1 = automation::stackPan (macroPan1, groupPan1, 0.0f);

            const float panL0 = std::cos ((p0 + 1.0f) * kPiOver4);
            const float panR0 = std::sin ((p0 + 1.0f) * kPiOver4);
            const float panL1 = std::cos ((p1 + 1.0f) * kPiOver4);
            const float panR1 = std::sin ((p1 + 1.0f) * kPiOver4);

            auto& bus = channelBuses[(size_t) busIndexFor (slot.mixerChannel)];
            float* busL = bus.getWritePointer (0);
            float* busR = bus.getWritePointer (1);

            // Hoisted out of the sample loop: which parameter this note's curve
            // is allowed to touch is a property of the note, not of the sample.
            const bool curveOn   = note.envelope.isEnabled();
            const bool modLevel  = curveOn && note.modTarget == NoteModTarget::level;
            const bool modPan    = curveOn && note.modTarget == NoteModTarget::pan;
            const bool modPitch  = curveOn && note.modTarget == NoteModTarget::pitch
                                      && ! note.pitchIntegral.empty();

            for (int i = first; i < last; ++i)
            {
                const double elapsed   = (startSeconds + (double) i / sampleRate) - note.startSeconds;
                const double remaining = length - elapsed;

                const float u   = (float) juce::jlimit (0.0, 1.0, elapsed / length);

                // THE micro envelope, still bound to the note by value. What it
                // modulates is the note's own choice (Phase 8).
                const float env = curveOn ? note.envelope.valueAt (u) : 1.0f;

                // 2.5 ms de-click at both ends, so a flat curve cannot start or
                // stop on a step.
                const float fade = (float) juce::jlimit (0.0, 1.0,
                                      juce::jmin (elapsed, remaining) / kClickFadeSecond);

                const float t = (float) i * invNumSamples;

                // THE macro envelopes: the layer fader and the synth fader,
                // both ramped across the block.
                const float layerGain = lb.gain0 + (lb.gain1 - lb.gain0) * t;
                const float slotGain  = slotGain0 + (slotGain1 - slotGain0) * t;
                const float groupGain = groupGain0 + (groupGain1 - groupGain0) * t;

                float amp   = modLevel ? env : 1.0f;
                float panL  = panL0 + (panL1 - panL0) * t;
                float panR  = panR0 + (panR1 - panR0) * t;
                double phase = frequency * elapsed;

                if (modPan)
                {
                    // Same rule as the macro pan: angles add, they do not
                    // multiply. 0.5 in the curve means "no offset".
                    //
                    // Phase 36: and the addition is `stackPan`'s, with the note
                    // as its own term. A curve sitting at 0.5 contributes 0, so
                    // an armed-but-flat note pan cannot move a panned group.
                    const float spread = juce::jlimit (0.0f, 1.0f, env) * 2.0f - 1.0f;
                    const float pp     = automation::stackPan (macroPan0, groupPan0, spread);

                    panL = std::cos ((pp + 1.0f) * kPiOver4);
                    panR = std::sin ((pp + 1.0f) * kPiOver4);
                }
                else if (modPitch)
                {
                    // The tabulated integral, in cycles: frequency * length is
                    // how many cycles one octave of bend is worth over this note.
                    phase += frequency * length * (double) note.pitchIntegralAt (u);
                }

                // Phase 36: the three layers, through the one law. The macro
                // term is layer × synth because those two are the same layer of
                // the stack; the group and the note are the other two. Before
                // this line existed the multiplication was spelled out here and
                // again in the clip loop, which is exactly how two renderers
                // drift apart.
                const float stackGain = automation::stackLevel (layerGain * slotGain,
                                                                groupGain, amp);

                float mono = oscillator (note.waveform, phase, increment)
                               * stackGain * fade * note.gain;

                if (filterOn)
                {
                    filterZ = (1.0f - filterPole) * mono + filterPole * filterZ;
                    mono = filterZ;
                }

                busL[i] += mono * panL;
                busR[i] += mono * panR;

                voicePeak = juce::jmax (voicePeak, std::abs (mono));
            }

            anyVoice = true;
            busUsed[(size_t) busIndexFor (slot.mixerChannel)] = true;

            if (juce::isPositiveAndBelow (layerIndex, maxMeteredLayers))
                layerVoicePeak[(size_t) layerIndex] = juce::jmax (layerVoicePeak[(size_t) layerIndex],
                                                                  voicePeak);
        }

        //-- Phase 17: audio clips on this synth ----------------------------
        // Rendered after the notes, into the same channel bus, so a clip obeys
        // the layer fader, the synth fader and the channel's inserts exactly
        // like a note does. Two things it deliberately does NOT get:
        //
        //   * the synth filter. Its one-pole has state that belongs to the
        //     oscillator voices; sharing it would corrupt them, and running a
        //     second instance just to mangle someone's recorded audio is not
        //     what "the synth's filter" means.
        //   * constant-power panning. A stereo file already has an image, so
        //     the pan is applied as a balance: hard left keeps the left channel
        //     and fades the right, instead of collapsing the image and
        //     re-spreading it.
        //
        // Phase 35: what it DOES get, and did not before, is the group trim.
        // A fader, a pan curve and a mute are statements about "my members", and
        // a block can be a member. The pan joins the balance above rather than
        // becoming a second law, so "pan the group" sounds like "pan the layer"
        // on the same material.
        for (const auto& clip : current.clips)
        {
            if (clip.slotId != slot.slotId)
                continue;

            if (clip.audio == nullptr)
                continue;

            if (clip.startSeconds >= blockEnd)
                break;                      // sorted by start time

            if (clip.endSeconds <= startSeconds)
                continue;

            if (clip.gain <= 0.0f)
                continue;

            const int layerIndex = clip.layerIndex;

            if (! juce::isPositiveAndBelow (layerIndex, numLayerMixes))
                continue;

            const auto& lb = layerBlocks[(size_t) layerIndex];

            if (lb.gain0 <= 0.0f && lb.gain1 <= 0.0f)
                continue;

            // Phase 35: the group trim, looked up exactly the way a note does it
            // — same array, same lambda, same frozen values. A clip that is not
            // in a group gets 1.0, so the arithmetic below is unchanged for the
            // fifty-percent case where nobody ever groups a block.
            //
            // The mute is already inside these numbers (GroupMix::gainAt), which
            // is deliberate: this loop must not be able to disagree with the
            // voice loop about what a muted group means.
            const auto* gb = groupBlockFor (clip.groupId);
            const float groupGain0 = gb != nullptr ? gb->gain0 : 1.0f;
            const float groupGain1 = gb != nullptr ? gb->gain1 : 1.0f;

            if (groupGain0 <= 0.0f && groupGain1 <= 0.0f)
                continue;                       // muted, or the curve faded it out

            const float groupPan0 = gb != nullptr ? gb->pan0 : 0.0f;
            const float groupPan1 = gb != nullptr ? gb->pan1 : 0.0f;

            const int first = juce::jmax (0, (int) std::ceil ((clip.startSeconds - startSeconds) * sampleRate - 1.0e-9));
            const int last  = juce::jmin (numSamples, (int) std::ceil ((clip.endSeconds - startSeconds) * sampleRate));

            if (last <= first)
                continue;

            const auto& buffer     = *clip.audio;
            const int   srcFrames  = buffer.numFrames();
            const int   srcChannels = buffer.channels;

            if (srcFrames <= 0 || srcChannels <= 0 || buffer.sampleRate <= 0.0)
                continue;

            const double length = clip.endSeconds - clip.startSeconds;

            // Phase 36: and the macro term is the same sum the voice loop uses
            // — layer pan plus synth pan, added to the group's — because a block
            // and a note must not disagree about where "the layer is panned"
            // puts them.
            const float p0 = automation::stackPan (lb.pan0 + slotPan0, groupPan0, 0.0f);
            const float p1 = automation::stackPan (lb.pan1 + slotPan1, groupPan1, 0.0f);


            auto& bus = channelBuses[(size_t) busIndexFor (slot.mixerChannel)];
            float* busL = bus.getWritePointer (0);
            float* busR = bus.getWritePointer (1);

            float clipPeak = 0.0f;

            for (int i = first; i < last; ++i)
            {
                const double elapsed   = (startSeconds + (double) i / sampleRate) - clip.startSeconds;
                const double remaining = length - elapsed;

                // Phase 31: one call, and it is still a pure function of the
                // clock. `clipstretch::read` decides between a single read head
                // (原速 / 变速变调, where speed == pitch) and the two-head grain
                // crossfade (只变速 / 只变调), from numbers frozen at publish
                // time. Nothing here remembers a read position, so a seek or an
                // offline export cannot drift away from playback.
                //
                // Notice what is GONE: the old `break` when the read position
                // ran past the file. A stretched clip legitimately reaches the
                // end at a different moment than the block does, and the read
                // returns silence past it either way.
                // Phase 50: a reversed block asks the same reader for the
                // mirror of this moment. `length - elapsed` keeps the block a
                // window onto the SAME audio — the head of the file now arrives
                // last — and because the read is stateless, nothing about it
                // depends on which direction we walked in.
                const double readElapsed = clip.reversed ? juce::jmax (0.0, length - elapsed)
                                                         : juce::jmax (0.0, elapsed);

                const auto frame = clipstretch::read (buffer, clip.stretch,
                                                      clip.sourceOffsetSeconds,
                                                      readElapsed,
                                                      clipstretch::defaultGrainSeconds);

                const float left  = frame.left;
                const float right = frame.right;

                const float t = (float) i * invNumSamples;

                // Phase 49: the block's own volume curve, x in seconds since
                // the block's start. Multiplied on top of the de-click ramp
                // below rather than replacing it: a curve that starts at zero
                // and the anti-click ramp both end at silence, and keeping the
                // ramp means an "all the way down" curve is still click-free.
                float fadeGain = 1.0f;

                if (clip.gainAutomated)
                    fadeGain = automationValueAt (clip.gainCurve, juce::jmax (0.0, elapsed));

                // Phase 50: pan is the fader sum PLUS the block's own curve,
                // then clamped once. The old code pre-computed two balance
                // endpoints and lerped between them; that is the same thing
                // only while nothing else moves the pan, and a curve does. One
                // expression, one clamp, one law.
                float clipPan = p0 + (p1 - p0) * t;

                if (clip.panAutomated)
                    clipPan += automationValueAt (clip.panCurve, juce::jmax (0.0, elapsed)) * 2.0f - 1.0f;

                clipPan = juce::jlimit (-1.0f, 1.0f, clipPan);

                const float balL = juce::jmin (1.0f, 1.0f - clipPan);
                const float balR = juce::jmin (1.0f, 1.0f + clipPan);

                // Phase 36: the three layers through the one law. A block has no
                // note envelope, so its note term is the identity — which is not
                // a special case here but the definition of an absent layer. The
                // macro term still carries both faders, so a clip keeps obeying
                // the layer and the synth exactly as it did.
                const float stackGain = automation::stackLevel ((lb.gain0 + (lb.gain1 - lb.gain0) * t)
                                                                  * (slotGain0 + (slotGain1 - slotGain0) * t),
                                                                groupGain0 + (groupGain1 - groupGain0) * t,
                                                                1.0f);

                const float gain = clip.gain * fadeGain
                                     * stackGain
                                     * (float) juce::jlimit (0.0, 1.0,
                                                            juce::jmin (elapsed, remaining) / kClickFadeSecond);

                busL[i] += left  * gain * balL;
                busR[i] += right * gain * balR;

                clipPeak = juce::jmax (clipPeak, juce::jmax (std::abs (left), std::abs (right)));
            }

            voicePeak = juce::jmax (voicePeak, clipPeak * clip.gain);
            anyVoice  = true;
            busUsed[(size_t) busIndexFor (slot.mixerChannel)] = true;

            if (juce::isPositiveAndBelow (layerIndex, maxMeteredLayers))
                layerVoicePeak[(size_t) layerIndex] = juce::jmax (layerVoicePeak[(size_t) layerIndex],
                                                                  clipPeak * clip.gain);
        }

        if (! anyVoice)
            continue;

        slotFilters[filterIndex].zL = filterZ;

        if (stats != nullptr)
            stats->addPeak (slot.slotId, voicePeak);
    }

    //-- Stage 1b: hosted instruments ----------------------------------------
    // They render their own stereo bus from MIDI, so they cannot go through the
    // per-voice stage above. Phase 11: their bus is the console channel their
    // slot feeds (or the direct bus), so an insert on that channel reaches a
    // hosted instrument too — it used to jump straight to the master and skip
    // the console entirely.
    if (pluginsLive)
        renderHostedInstruments (current, startSeconds, numSamples, busUsed, stats);

    for (int i = 0; i < maxMeteredLayers; ++i)
        if (layerVoicePeak[(size_t) i] > 0.0f && stats != nullptr)
            stats->addLayerPeak (i, layerVoicePeak[(size_t) i]);

    //-- Stage 2: console channels, then the sum ------------------------------
    // Insert processing happens on a bus, never per voice: a delay or a filter
    // has memory that must see the whole channel, not one note at a time. A
    // channel nobody feeds is skipped — an insert sitting on an unrouted channel
    // must not colour the mix (and must not advance its delay line either).
    //
    // Phase 23: the strips are walked in `channelOrder`, not 0..7. A strip that
    // feeds another strip is summed into its destination's bus BEFORE that
    // destination reaches this loop, so a group channel hears its children. The
    // order was decided at publish time; this loop never looks at the graph.
    const int orderSize = current.channelOrder.empty() ? (int) current.channels.size()
                                                       : (int) current.channelOrder.size();

    for (int step = 0; step < orderSize; ++step)
    {
        const int channel = current.channelOrder.empty() ? step
                                                        : current.channelOrder[(size_t) step];

        if (! juce::isPositiveAndBelow (channel, MixerModel::numChannels))
            continue;

        if (! busUsed[(size_t) channel])
            continue;

        auto& bus   = channelBuses[(size_t) channel];
        auto& chain = channelChains[(size_t) channel];

        const auto& inserts = juce::isPositiveAndBelow (channel, (int) current.channels.size())
                                ? current.channels[(size_t) channel].inserts
                                : kNoInserts;

        if (pluginsLive)
            processInsertChain (inserts, bus, numSamples, chain);
        else
            chain.process (inserts, bus, numSamples, sampleRate);

        // Phase 25: mono monitoring, straight after the inserts. Here and not
        // before them because the chain is set up for a stereo signal — summing
        // first would feed every reverb and delay a mono source, which is a
        // different sound, not a different way of listening to this one.
        if (juce::isPositiveAndBelow (channel, (int) current.channels.size())
              && current.channels[(size_t) channel].mono)
        {
            auto* left  = bus.getWritePointer (0);
            auto* right = bus.getNumChannels() > 1 ? bus.getWritePointer (1) : left;

            for (int i = 0; i < numSamples; ++i)
            {
                const float sum = (left[i] + right[i]) * 0.5f;
                left[i]  = sum;
                right[i] = sum;
            }
        }

        const int destination = juce::isPositiveAndBelow (channel, (int) current.channels.size())
                                  ? current.channels[(size_t) channel].output
                                  : MixerModel::masterChannel;

        if (juce::isPositiveAndBelow (destination, MixerModel::numChannels))
        {
            // Feeds another strip: sum into its bus and mark it as fed, so a
            // destination whose only source is a child still runs its inserts.
            auto& target = channelBuses[(size_t) destination];

            for (int ch = 0; ch < 2; ++ch)
                target.addFrom (ch, 0, bus, ch, 0, numSamples);

            busUsed[(size_t) destination] = true;
        }
        else
        {
            for (int ch = 0; ch < numChannels; ++ch)
                output.addFrom (ch, 0, bus, juce::jmin (ch, 1), 0, numSamples);
        }

        if (stats != nullptr)
            stats->addChannelPeak (channel, bus.getMagnitude (0, 0, numSamples),
                                   bus.getMagnitude (juce::jmin (1, bus.getNumChannels() - 1),
                                                     0, numSamples));
    }

    // Everything that bypasses the console lands here: no inserts, no channel
    // fader, straight into the master strip.
    if (busUsed[(size_t) MixerModel::numChannels])
    {
        auto& direct = channelBuses[(size_t) MixerModel::numChannels];

        for (int ch = 0; ch < numChannels; ++ch)
            output.addFrom (ch, 0, direct, juce::jmin (ch, 1), 0, numSamples);
    }

    //-- Stage 3: master inserts + master fader -------------------------------
    auto& masterChain = channelChains[(size_t) MixerModel::masterChannel];

    if (pluginsLive)
        processInsertChain (current.masterInserts, output, numSamples, masterChain);
    else
        masterChain.process (current.masterInserts, output, numSamples, sampleRate);

    // Balance-style master pan: unity in the centre, so a master fader at 0 dB
    // does not quietly cost 3 dB the way a constant-power law would.
    //
    // Phase 18: the fader value is the START of the block and the curve gives
    // the END of it, so the block is ramped instead of stepped. A 1024-sample
    // step on a moving master fader is audible as zipper noise on a fade.
    const double masterEndBeat = startBeat + (double) numSamples / sampleRate / spb;

    // Phase 43: the master's mute lane is a GATE on the same fade, read at both
    // ends of the block like the level curve above — so switching it off mid
    // fade lands where the fade was, not at full level.
    const auto masterMuteGate = [&] (double beat)
    {
        if (! current.masterMuteAutomated)
            return 1.0f;

        return automationValueAt (current.masterMutePoints, beat) < 0.5f ? 0.0f : 1.0f;
    };

    const float level0 = current.masterGain
                           * masterMuteGate (startBeat)
                           * (current.masterLevelAutomated
                                ? automationValueAt (current.masterLevelPoints, startBeat) : 1.0f);
    const float level1 = current.masterGain
                           * masterMuteGate (masterEndBeat)
                           * (current.masterLevelAutomated
                                ? automationValueAt (current.masterLevelPoints, masterEndBeat) : 1.0f);

    const auto panFromCurve = [&] (double beat)
    {
        if (! current.masterPanAutomated)
            return current.masterPan;

        // The lane is 0..1; +/-1 is what a pan law speaks. Same mapping the
        // layer lanes use.
        return juce::jlimit (-1.0f, 1.0f,
                             current.masterPan + (automationValueAt (current.masterPanPoints, beat) * 2.0f - 1.0f));
    };

    const float pan0 = panFromCurve (startBeat);
    const float pan1 = panFromCurve (masterEndBeat);

    const float masterL0 = level0 * juce::jmin (1.0f, 1.0f - pan0);
    const float masterR0 = level0 * juce::jmin (1.0f, 1.0f + pan0);
    const float masterL1 = level1 * juce::jmin (1.0f, 1.0f - pan1);
    const float masterR1 = level1 * juce::jmin (1.0f, 1.0f + pan1);

    if (numSamples > 1)
    {
        if (numChannels > 0)
            output.applyGainRamp (0, 0, numSamples, masterL0, masterL1);

        if (numChannels > 1)
            output.applyGainRamp (1, 0, numSamples, masterR0, masterR1);
    }
    else
    {
        if (numChannels > 0)
            output.applyGain (0, 0, numSamples, masterL0);

        if (numChannels > 1)
            output.applyGain (1, 0, numSamples, masterR0);
    }

    //-- Phase 26: master mono monitoring --------------------------------------
    // Last, not next to the inserts. A channel applies mono at the point its
    // signal leaves the strip; the master's exit is here, after the inserts and
    // after the fader/pan ramp. Summing earlier would leave the pan law working
    // on a signal that no longer has a side, and the meter would then disagree
    // with what the device is actually being fed.
    if (current.masterMono && numChannels > 1)
    {
        auto* left  = output.getWritePointer (0, 0);
        auto* right = output.getWritePointer (1, 0);

        for (int i = 0; i < numSamples; ++i)
        {
            const float sum = (left[i] + right[i]) * 0.5f;
            left[i]  = sum;
            right[i] = sum;
        }
    }

    if (stats != nullptr)
        stats->peak = juce::jmax (stats->peak, output.getMagnitude (0, 0, numSamples));
}

//==============================================================================
void PlaybackEngine::renderHostedInstruments (const PublishedSchedule& current, double startSeconds,
                                              int numSamples,
                                              std::array<bool, MixerModel::numChannels + 1>& busUsed,
                                              RenderStats* stats) const
{
    if (pluginHost == nullptr || numSamples <= 0)
        return;

    const double spb       = juce::jmax (1.0e-4, current.secondsPerBeat);
    const double startBeat = startSeconds / spb;
    const double endBeat   = (startSeconds + (double) numSamples / sampleRate) / spb;

    // Never hand a plugin more samples than its prepared scratch can hold.
    const int chunkMax = juce::jmax (1, juce::jmin (pluginHost->getRenderBlockSize(),
                                                    pluginBuffer.getNumSamples()));

    if (pluginBuffer.getNumChannels() < 2)
        return;

    // Phase 43: the same per-block solo question the oscillator stage asks, for
    // the same reason — a solo curve moves with the playhead.
    bool anySoloNow = false;

    for (const auto& s : current.slots)
        if (s.isSoloedAt (startBeat))
        {
            anySoloNow = true;
            break;
        }

    for (const auto& slot : current.slots)
    {
        if (slot.pluginId <= 0 || ! slot.isAudibleAt (startBeat, anySoloNow))
            continue;

        auto* plugin = pluginHost->getInstanceWhileLocked (slot.pluginId);

        if (plugin == nullptr)
            continue;

        const float gain0 = slot.gainAt (startBeat);
        const float gain1 = slot.gainAt (endBeat);

        if (gain0 <= 0.0f && gain1 <= 0.0f)
            continue;                           // muted synth

        const int pluginChannels = juce::jlimit (1, 2, plugin->getTotalNumOutputChannels());

        // Phase 11: the plugin's output joins the bus its slot feeds, exactly
        // like the internal oscillator's would. Straight to the master when the
        // synth is not routed to a console channel.
        const int busIndex = juce::isPositiveAndBelow (slot.mixerChannel, MixerModel::numChannels)
                               ? slot.mixerChannel
                               : MixerModel::numChannels;

        auto& destination = channelBuses[(size_t) busIndex];

        if (destination.getNumChannels() < 2 || destination.getNumSamples() < numSamples)
            destination.setSize (2, numSamples, false, false, false);

        const int outChannels = juce::jmin (2, destination.getNumChannels());

        // Balance, not constant power: the plugin already produced a stereo
        // image, and squaring it away would break it. Phase 43: the pan comes
        // from `panAt`, so a pan curve is heard here too — sampled once per
        // block, because a plugin's output is gained as a whole and a per-sample
        // ramp would mean touching its buffer twice.
        const float balanceL = juce::jmin (1.0f, 1.0f - slot.panAt (startBeat));
        const float balanceR = juce::jmin (1.0f, 1.0f + slot.panAt (startBeat));

        float slotPeak = 0.0f;

        for (int chunkStart = 0; chunkStart < numSamples; )
        {
            const int chunkLen = juce::jmin (chunkMax, numSamples - chunkStart);

            pluginMidi.clear();

            for (const auto& note : current.notes)
            {
                if (note.slotId != slot.slotId)
                    continue;

                const double onOffset  = (note.startSeconds - startSeconds) * sampleRate;
                const double offOffset = (note.endSeconds   - startSeconds) * sampleRate;

                const int on  = (int) std::llround (onOffset)  - chunkStart;
                const int off = (int) std::llround (offOffset) - chunkStart;

                // Phase 70: judge the ROUNDED sample offsets, not the doubles.
                // A note-off landing exactly on the block boundary used to fall
                // through both blocks — this one refused it because
                // `off < chunkLen` was false, and the next one skipped it
                // because `offOffset <= chunkStart` was true. A hosted plugin
                // then never got its note-off and the note rang until the next
                // stop. Rounding first lets one comparison decide both sides:
                // here the tail is `chunkLen`, one block later it is `0`.
                if (off < 0 || on >= chunkLen)
                    continue;

                // A muted layer has to be silent on a plugin too, and a plugin
                // bus cannot be muted per layer — so its notes simply never get
                // a note-on. The note-off is still sent in case one was already
                // sounding when the layer was muted.
                bool layerAudible = true;

                for (const auto& mix : current.layers)
                    if (mix.layerIndex == note.layerIndex)
                        layerAudible = mix.audible;

                if (on >= 0 && on < chunkLen && layerAudible && note.gain > 0.0f)
                {
                    // Phase 16: a hosted instrument has no bus we can trim per
                    // group, so the group curve is sampled at the note's own
                    // start and folded into its velocity. Coarser than the
                    // internal oscillator's per-sample trim (which is the price
                    // of not owning the plugin's output) but honest: the amount
                    // you hear is the curve's value when the note begins.
                    float velocity = note.gain;

                    if (note.groupId >= 0)
                        for (const auto& mix : current.groups)
                            if (mix.groupId == note.groupId && mix.levelAutomated)
                                velocity *= automationValueAt (mix.levelPoints,
                                                               note.startSeconds / current.secondsPerBeat);

                    pluginMidi.addEvent (juce::MidiMessage::noteOn (1, note.pitch,
                                                                    juce::jlimit (0.0f, 1.0f, velocity)),
                                         on);
                }

                if (off >= 0 && off < chunkLen)
                    pluginMidi.addEvent (juce::MidiMessage::noteOff (1, note.pitch), off);
            }

            // Phase 39: keys held on this instrument go into the SAME MIDI
            // buffer as the timeline's notes. The alternative — a second
            // processBlock just for the live voice — would run the plugin's own
            // DSP twice per block, and "the LFO is twice as fast while the
            // transport rolls" is a bug nobody would ever track down.
            if (chunkStart == 0)
                for (int e = 0; e < pendingLiveCount; ++e)
                {
                    const auto& event = pendingLive[(size_t) e];

                    if (event.slotId != slot.slotId)
                        continue;

                    if (event.on)
                        pluginMidi.addEvent (juce::MidiMessage::noteOn (1, event.pitch,
                                                                        event.velocity), 0);
                    else
                        pluginMidi.addEvent (juce::MidiMessage::noteOff (1, event.pitch), 0);
                }

            // Plugins are not required to clear their output, and a leftover
            // from the last chunk is exactly the kind of noise nobody can
            // explain later.
            pluginBuffer.clear (0, chunkLen);

            // Phase 24: any curve that writes one of this instrument's
            // parameters lands here — after the MIDI for this chunk is built,
            // before it is processed, so the parameter and the notes it shapes
            // belong to the same chunk.
            applyHostedParamAutomation (*plugin, current, slot.pluginId,
                                        (startSeconds + (double) chunkStart / sampleRate) / spb,
                                        (startSeconds + (double) (chunkStart + chunkLen) / sampleRate) / spb);

            juce::AudioBuffer<float> view (pluginBuffer.getArrayOfWritePointers(),
                                           pluginChannels, 0, chunkLen);

            plugin->processBlock (view, pluginMidi);

            const float t    = (float) chunkStart / (float) numSamples;
            const float gain = gain0 + (gain1 - gain0) * t;

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int   source  = juce::jmin (ch, pluginChannels - 1);
                const float balance = (ch == 0 ? balanceL : balanceR);

                destination.addFrom (ch, chunkStart, view, source, 0, chunkLen, gain * balance);
            }

            slotPeak = juce::jmax (slotPeak, view.getMagnitude (0, chunkLen));

            chunkStart += chunkLen;
        }

        busUsed[(size_t) busIndex] = true;

        // Phase 39: this plugin has already been processed in this callback, so
        // renderLive must not process it again — see the live MIDI merge above.
        if (numTransportPluginSlots < maxTransportPluginSlots)
            transportPluginSlots[(size_t) numTransportPluginSlots++] = slot.slotId;

        if (stats != nullptr)
            stats->addPeak (slot.slotId, slotPeak);
    }
}

//==============================================================================
void PlaybackEngine::applyHostedParamAutomation (juce::AudioProcessor& plugin,
                                                 const PublishedSchedule& current,
                                                 int pluginInstanceId,
                                                 double chunkStartBeat,
                                                 double chunkEndBeat) const
{
    const auto& parameters = plugin.getParameters();

    if (parameters.isEmpty())
        return;

    //-- Phase 36: collect this chunk's live curves, then merge per parameter --
    //
    // Before this phase the loop wrote each curve's value straight into the
    // parameter, so two curves aimed at the same parameter overwrote each other
    // and `priority` decided which one the user heard. That is not a fader, it
    // is a coin toss with a documented winner: a group dragging a cutoff while a
    // note shapes the same cutoff should produce the product of both, not one of
    // them.
    //
    // So: gather first (a fixed array, because the audio thread does not
    // allocate), then for each distinct parameter multiply together everything
    // that points at it and write the result ONCE. One `setValueNotifyingHost`
    // per parameter per chunk also means a plugin's editor repaints once, not
    // once per note.
    //
    // 64 is a ceiling, not an expectation: overlapping curves in one chunk means
    // notes that start within a few milliseconds of each other, and a song with
    // more than 64 of those in one buffer has already stopped being legible.
    // Anything past the ceiling is dropped rather than written unmerged, because
    // an unmerged write is the old overwrite bug wearing a different hat.
    static constexpr int maxLiveCurves = 64;
    const PluginParamCurve* live[maxLiveCurves] {};
    int liveCount = 0;

    for (const auto& curve : current.pluginParams)
    {
        if (curve.pluginId != pluginInstanceId || curve.points.empty())
            continue;

        // Overlap, not containment: a note shorter than a chunk still gets its
        // curve written, because the chunk it lives inside is a chunk it
        // intersects. Evaluating at the chunk's start would otherwise write the
        // value from before the note began.
        if (curve.endBeat < chunkStartBeat || curve.startBeat > chunkEndBeat)
            continue;

        if (! juce::isPositiveAndBelow (curve.paramIndex, parameters.size()))
            continue;

        if (liveCount < maxLiveCurves)
            live[liveCount++] = &curve;
    }

    for (int i = 0; i < liveCount; ++i)
    {
        const int index = live[i]->paramIndex;

        // Only the first curve naming this parameter does the work; the rest are
        // folded into its product below. Without this, N curves on one parameter
        // would write it N times.
        bool handled = false;

        for (int j = 0; j < i && ! handled; ++j)
            handled = live[j]->paramIndex == index;

        if (handled)
            continue;

        auto* parameter = parameters.getUnchecked (index);

        if (parameter == nullptr)
            continue;

        // The product, through the one law. A layer with no live curve is the
        // identity, which is why a single curve passes through unchanged and
        // "group 0.5 × note 0.5" is 0.25 rather than 1.0.
        float want = 1.0f;

        for (int k = 0; k < liveCount; ++k)
        {
            if (live[k]->paramIndex != index)
                continue;

            const double at = juce::jlimit (live[k]->startBeat, live[k]->endBeat, chunkStartBeat);

            want = automation::stackPluginParam (want,
                                                 automationValueAt (live[k]->points, at));
        }

        want = juce::jlimit (0.0f, 1.0f, want);

        // A notification per parameter per block would make a plugin repaint its
        // editor at block rate for values that did not move. Only write real
        // changes; a step smaller than the parameter's own resolution is not a
        // change.
        if (std::abs (parameter->getValue() - want) < 1.0e-4f)
            continue;

        parameter->setValueNotifyingHost (want);
    }
}

//==============================================================================
void PlaybackEngine::processInsertChain (const std::array<fx::Settings, fx::ChainState::maxSlots>& settings,
                                         juce::AudioBuffer<float>& buffer, int numSamples,
                                         fx::ChainState& chain) const
{
    // Slot order is the whole point: a native delay before a hosted reverb has
    // to sound different from the other way round.
    for (int i = 0; i < fx::ChainState::maxSlots; ++i)
    {
        const auto& setting = settings[(size_t) i];

        if (! setting.active())
            continue;

        if (setting.type == fx::Type::vst)
            processHostedEffect (setting.pluginId, buffer, numSamples);
        else
            chain.processSlot (i, setting, buffer, numSamples, sampleRate);
    }
}

void PlaybackEngine::processHostedEffect (int pluginInstanceId, juce::AudioBuffer<float>& buffer,
                                          int numSamples) const
{
    if (pluginHost == nullptr || pluginInstanceId <= 0 || numSamples <= 0)
        return;

    auto* plugin = pluginHost->getInstanceWhileLocked (pluginInstanceId);

    if (plugin == nullptr)
        return;         // the plugin was removed: the insert goes quietly inert

    const int channels = juce::jmin (2, buffer.getNumChannels());

    if (channels <= 0)
        return;

    const int chunkMax = juce::jmax (1, juce::jmin (pluginHost->getRenderBlockSize(),
                                                    pluginBuffer.getNumSamples()));

    for (int offset = 0; offset < numSamples; )
    {
        const int len = juce::jmin (chunkMax, numSamples - offset);

        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), channels, offset, len);

        pluginMidi.clear();
        plugin->processBlock (view, pluginMidi);

        offset += len;
    }
}

//==============================================================================
void PlaybackEngine::allNotesOffHosted()
{
    if (pluginHost == nullptr)
        return;

    // Try it now, and leave a flag in case the audio thread is mid-block: the
    // next block it renders will do it instead. Never blocks, never loses.
    {
        const juce::ScopedTryLock guard (pluginHost->getLock());

        if (guard.isLocked())
        {
            pluginHost->resetAllInstancesWhileLocked();
            return;
        }
    }

    pluginPanicRequested.store (true);
}

//==============================================================================
void PlaybackEngine::renderNextBlock (juce::AudioBuffer<float>& output, int numSamples)
{
    auto current = getScheduleRaw();

    if (current == nullptr || numSamples <= 0)
    {
        output.clear();
        return;
    }

    output.clear();

    const double spb  = juce::jmax (1.0e-4, current->secondsPerBeat);
    const double beatsPerSample = 1.0 / (sampleRate * spb);
    const double length = juce::jmax (1.0, current->lengthBeats);

    double posBeats = positionBeats.load (std::memory_order_relaxed);

    if (posBeats >= length)
        posBeats = 0.0;

    // Published before rendering: the UI extrapolates from this pair, so it
    // must not point at a block that has not been produced yet.
    blockStartBeats.store (posBeats, std::memory_order_relaxed);
    blockBeats.store ((double) numSamples * beatsPerSample, std::memory_order_relaxed);
    blockStartTimeMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);

    // Phase 70: reuse the one member instead of building a fresh set of vectors
    // in every callback. clear() keeps the capacity, so the writers below only
    // touch memory that was bought when the engine was constructed.
    liveStats.clear();
    RenderStats& stats = liveStats;

    int offset    = 0;
    int remaining = numSamples;

    while (remaining > 0)
    {
        // How many of the remaining samples fall before the loop point. A block
        // that straddles the end of the timeline is split rather than truncated:
        // otherwise every lap would swallow up to one buffer of audio.
        const double samplesLeft = (length - posBeats) / beatsPerSample;
        int segment = remaining;

        if (samplesLeft < (double) remaining)
            segment = juce::jlimit (1, remaining, (int) std::floor (samplesLeft));

        // A view onto the device buffer: no copy, and renderInto clears only
        // the slice it is about to write.
        juce::AudioBuffer<float> slice (output.getArrayOfWritePointers(),
                                        output.getNumChannels(), offset, segment);

        renderInto (*current, posBeats * spb, segment, slice, &stats);

        offset    += segment;
        remaining -= segment;
        posBeats  += (double) segment * beatsPerSample;

        if (posBeats >= length)
            posBeats = 0.0;
    }

    positionBeats.store (posBeats, std::memory_order_relaxed);

    // Meters rise instantly and fall slowly.
    for (auto& meter : meters)
        meter.store (meter.load (std::memory_order_relaxed) * 0.80f);

    for (auto& meter : layerMeters)
        meter.store (meter.load (std::memory_order_relaxed) * 0.80f);

    for (auto& meter : channelMetersL)
        meter.store (meter.load (std::memory_order_relaxed) * 0.80f);

    for (auto& meter : channelMetersR)
        meter.store (meter.load (std::memory_order_relaxed) * 0.80f);

    for (const auto& entry : stats.slotPeaks)
    {
        const int index = juce::jlimit (0, maxMeteredSlots - 1, entry.first % maxMeteredSlots);
        auto& meter = meters[(size_t) index];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), entry.second),
                     std::memory_order_relaxed);
    }

    for (const auto& entry : stats.layerPeaks)
    {
        if (! juce::isPositiveAndBelow (entry.first, maxMeteredLayers))
            continue;

        auto& meter = layerMeters[(size_t) entry.first];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), entry.second),
                     std::memory_order_relaxed);
    }

    for (const auto& entry : stats.channelPeaks)
    {
        if (! juce::isPositiveAndBelow (entry.channel, MixerModel::numChannels))
            continue;

        auto& meterL = channelMetersL[(size_t) entry.channel];
        auto& meterR = channelMetersR[(size_t) entry.channel];

        meterL.store (juce::jmax (meterL.load (std::memory_order_relaxed), entry.left),
                      std::memory_order_relaxed);
        meterR.store (juce::jmax (meterR.load (std::memory_order_relaxed), entry.right),
                      std::memory_order_relaxed);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        if (ch >= output.getNumChannels())
            break;

        auto& meter = masterMeters[(size_t) ch];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed),
                                 output.getMagnitude (ch, 0, numSamples)),
                     std::memory_order_relaxed);
    }
}

//==============================================================================
void PlaybackEngine::auditionNote (int pitch, float velocity, int slotId, bool retrigger)
{
    // Payload first, serial last: the release store is the publish point.
    auditionPitch.store     (juce::jlimit (0, 127, pitch), std::memory_order_relaxed);
    auditionVelocity.store  (juce::jlimit (0.0f, 1.0f, velocity), std::memory_order_relaxed);
    auditionSlotId.store    (slotId, std::memory_order_relaxed);
    auditionRetrigger.store (retrigger, std::memory_order_relaxed);
    auditionEngaged.store   (true, std::memory_order_relaxed);

    auditionSerial.fetch_add (1, std::memory_order_release);
}

void PlaybackEngine::stopAudition()
{
    auditionStopRequest.store (true, std::memory_order_relaxed);
}

void PlaybackEngine::renderAudition (juce::AudioBuffer<float>& output, int numSamples)
{
    if (numSamples <= 0 || output.getNumChannels() <= 0 || sampleRate <= 0.0)
        return;

    // Phase 71: jump the envelope past its hold so the ordinary 4 ms ramp takes
    // it to zero — the same edge as every other end of the preview.

    const int serial = auditionSerial.load (std::memory_order_acquire);

    if (serial != auditionVoice.serial)
    {
        const int   pitch    = auditionPitch.load (std::memory_order_relaxed);
        const int   slotId   = auditionSlotId.load (std::memory_order_relaxed);
        const float velocity = auditionVelocity.load (std::memory_order_relaxed);
        const bool  retrigger = auditionRetrigger.load (std::memory_order_relaxed);

        // "Same gesture, new pitch": only when the previous request is still
        // sounding on the same synth. Anything else is a fresh preview.
        const bool glide = ! retrigger && auditionVoice.active && auditionVoice.slotId == slotId;

        auditionVoice.serial   = serial;
        auditionVoice.slotId   = slotId;
        auditionVoice.pitch    = pitch;
        auditionVoice.velocity = velocity;

        // A glide keeps the envelope where it is (capped inside the sustain
        // window) instead of restarting it: that is the difference between a
        // slide and a machine gun.
        auditionVoice.played = glide ? juce::jmin (auditionVoice.played, kAuditionHold * 0.4)
                                     : 0.0;

        // What the synth sounds like. The schedule is the only place the audio
        // thread is allowed to look, and the publish step froze the waveform
        // there for exactly this. No such slot means a plain sine at unity.
        auditionVoice.waveform = Waveform::sine;
        auditionVoice.gain     = 1.0f;
        auditionVoice.pan      = 0.0f;

        if (auto current = getScheduleRaw())
            for (const auto& slot : current->slots)
                if (slot.slotId == slotId)
                {
                    auditionVoice.waveform = slot.waveform;
                    auditionVoice.gain     = slot.audible ? slot.gain : 0.0f;
                    auditionVoice.pan      = slot.pan;
                    break;
                }

        auditionVoice.active = true;
        auditionEngaged.store (true, std::memory_order_relaxed);
        auditionConsumed.store (serial, std::memory_order_release);
    }

    // Phase 71: handled AFTER the request above, so a stop that arrives while a
    // request is still queued wins — otherwise the preview would start at full
    // level and only then be told to stop, which is the opposite of "cut it".
    if (auditionStopRequest.exchange (false, std::memory_order_relaxed)
          && auditionVoice.active)
        auditionVoice.played = juce::jmax (auditionVoice.played, kAuditionLength);

    if (! auditionVoice.active)
        return;

    auto current = getScheduleRaw();

    const float masterGain = current != nullptr ? current->masterGain : kMasterGain;
    const float masterPan  = current != nullptr ? current->masterPan  : 0.0f;

    // Same balance law as the master strip, so the preview sits where the mix
    // sits: unity in the centre, one side faded at the extremes.
    const float pan   = juce::jlimit (-1.0f, 1.0f, masterPan + auditionVoice.pan);
    const float levelL = masterGain * juce::jmin (1.0f, 1.0f - pan);
    const float levelR = masterGain * juce::jmin (1.0f, 1.0f + pan);

    const double frequency = pitchToFrequency (auditionVoice.pitch);
    const double increment = frequency / sampleRate;

    // 4 ms one-pole on the amplitude. Re-triggering a note whose phase is where
    // the last one left off is a step in the waveform, and this is what turns
    // that step into something you cannot hear as a click.
    const float smoothing = (float) (1.0 - std::exp (-1.0 / (0.004 * sampleRate)));

    float* left  = output.getWritePointer (0);
    float* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const double t = auditionVoice.played;

        float target = 0.0f;

        if (t < kAuditionLength)
            target = auditionVoice.velocity * auditionVoice.gain
                       * std::exp (-(float) juce::jmax (0.0, t - kAuditionHold) / (float) kAuditionDecay);

        auditionVoice.amplitude += (target - auditionVoice.amplitude) * smoothing;

        const float mono = oscillator (auditionVoice.waveform, auditionVoice.phase, increment)
                             * auditionVoice.amplitude;

        left[i]  += mono * levelL;

        if (right != nullptr)
            right[i] += mono * levelR;

        peak = juce::jmax (peak, std::abs (mono));

        auditionVoice.phase  += increment;
        auditionVoice.played += 1.0 / sampleRate;
    }

    // The meters are the only feedback the user gets that a preview happened, so
    // it has to reach them even though no transport block contained it.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& meter = masterMeters[(size_t) ch];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), peak * masterGain),
                     std::memory_order_relaxed);
    }

    if (auditionVoice.played > kAuditionLength + 0.20 && std::abs (auditionVoice.amplitude) < 1.0e-4f)
    {
        auditionVoice.active    = false;
        auditionVoice.played    = 0.0;
        auditionVoice.amplitude = 0.0f;
        auditionEngaged.store (false, std::memory_order_relaxed);
    }
}

//==============================================================================
//  Phase 39: live performance — a key held down, not a note on the timeline
//==============================================================================
void PlaybackEngine::pushLiveEvent (const LiveEvent& event)
{
    // Producers only. Two of them exist (the MIDI thread and the message
    // thread) and AbstractFifo is single-producer, so they take turns. The
    // audio thread's reader takes no lock at all.
    const juce::SpinLock::ScopedLockType guard (liveProducerLock);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    liveFifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 + size2 <= 0)
        return;                       // queue full: a blocked producer, not a crash

    liveQueue[(size_t) (size1 > 0 ? start1 : start2)] = event;
    liveFifo.finishedWrite (1);
    liveQueued.fetch_add (1, std::memory_order_relaxed);
}

void PlaybackEngine::noteOnLive (int slotId, int pitch, float velocity)
{
    LiveEvent event;
    event.slotId   = slotId;
    event.pitch    = juce::jlimit (0, 127, pitch);
    event.velocity = juce::jlimit (0.0f, 1.0f, velocity);
    event.on       = true;

    pushLiveEvent (event);
}

void PlaybackEngine::noteOffLive (int pitch)
{
    LiveEvent event;
    event.pitch = juce::jlimit (0, 127, pitch);
    event.on    = false;

    // slotId stays -1: the note-off is matched by pitch, and by the time it is
    // applied the queue's reader looks the held voice up itself.
    pushLiveEvent (event);
}

void PlaybackEngine::allLiveNotesOff()
{
    // ONE event, not a read of the voice pool. The callers are a settings click
    // and a source switch — message thread — and the pool belongs to the audio
    // thread. The block that applies this expands it into the note-offs that
    // the plugins need and the releases the oscillators need.
    LiveEvent event;
    event.allOff = true;

    pushLiveEvent (event);
}

//==============================================================================
void PlaybackEngine::beginLiveBlock()
{
    pendingLiveCount = 0;
    numTransportPluginSlots = 0;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    liveFifo.prepareToRead (liveFifo.getNumReady(), start1, size1, start2, size2);

    for (int i = 0; i < size1; ++i)
        if (pendingLiveCount < maxLivePerBlock)
            pendingLive[(size_t) pendingLiveCount++] = liveQueue[(size_t) (start1 + i)];

    for (int i = 0; i < size2; ++i)
        if (pendingLiveCount < maxLivePerBlock)
            pendingLive[(size_t) pendingLiveCount++] = liveQueue[(size_t) (start2 + i)];

    const int taken = size1 + size2;
    liveFifo.finishedRead (taken);
    liveQueued.fetch_sub (juce::jmin (taken, liveQueued.load (std::memory_order_relaxed)),
                          std::memory_order_relaxed);

    for (int i = 0; i < pendingLiveCount; ++i)
        applyLiveEvent (pendingLive[(size_t) i]);
}

//==============================================================================
PlaybackEngine::LiveVoice* PlaybackEngine::findLiveVoice (int pitch, bool includeReleased)
{
    for (auto& voice : liveVoices)
        if (voice.active && voice.pitch == pitch && (includeReleased || ! voice.released))
            return &voice;

    return nullptr;
}

int PlaybackEngine::freeLiveVoiceSlot() const
{
    // Prefer a silent one, then the quietest released one, and only then take
    // a sounding voice away. Stealing the oldest note is what every synth does
    // when the pool is full; stealing a loud one while a silent one exists is
    // what makes a keyboard feel broken.
    int   bestIndex = -1;
    float quietest  = 2.0f;

    for (int i = 0; i < maxLiveVoices; ++i)
    {
        const auto& voice = liveVoices[(size_t) i];

        if (! voice.active)
            return i;

        if (voice.released && voice.amp < quietest)
        {
            quietest  = voice.amp;
            bestIndex = i;
        }
    }

    return bestIndex >= 0 ? bestIndex : 0;
}

PlaybackEngine::LivePluginSlot* PlaybackEngine::findLivePluginSlot (int slotId)
{
    for (auto& live : livePluginSlots)
        if (live.slotId == slotId)
            return &live;

    return nullptr;
}

void PlaybackEngine::pruneLivePluginSlots (const PublishedSchedule* current)
{
    if (current == nullptr)
        return;

    int engaged = 0;

    for (auto& live : livePluginSlots)
    {
        if (live.slotId < 0)
            continue;

        bool stillHosted = false;

        for (const auto& slot : current->slots)
            if (slot.slotId == live.slotId && slot.pluginId > 0)
            {
                stillHosted = true;
                break;
            }

        // Gone, or finished ringing: either way the place in the table goes
        // back. The second half is what the renderer used to do on its own, and
        // it still does — this just makes sure it happens even when there is no
        // plugin host to render with.
        if (! stillHosted || (live.numHeld == 0 && live.tailFrames <= 0))
        {
            live.slotId     = -1;
            live.numHeld    = 0;
            live.tailFrames = 0;
            continue;
        }

        ++engaged;
    }

    livePluginEngaged.store (engaged, std::memory_order_relaxed);
}

//==============================================================================
void PlaybackEngine::applyLiveEvent (LiveEvent& event)
{
    //-- "Let go of everything" ----------------------------------------------
    if (event.allOff)
    {
        for (auto& voice : liveVoices)
            if (voice.active)
                voice.released = true;

        int active = 0;

        for (const auto& voice : liveVoices)
            if (voice.active)
                ++active;

        liveVoicesActive.store (active, std::memory_order_relaxed);

        // A hosted instrument needs a real note-off per key, so the held list is
        // expanded into ordinary note-off events — the same ones a released key
        // would have produced, appended to this block's list so they are applied
        // (and sent) by the normal path a moment later.
        for (auto& live : livePluginSlots)
        {
            if (live.slotId < 0)
                continue;

            for (int i = 0; i < live.numHeld; ++i)
                if (pendingLiveCount < maxLivePerBlock)
                {
                    LiveEvent off;
                    off.slotId = live.slotId;
                    off.pitch  = live.held[(size_t) i];
                    off.on     = false;

                    pendingLive[(size_t) pendingLiveCount++] = off;
                }

            live.numHeld = 0;
        }

        // Nothing is held any more, so nothing may still claim a synth: the
        // table is the map a note-off reads, and a map that keeps pointing at
        // synths nobody is holding is a map a later key can be matched against
        // by accident.
        for (auto& held : liveHeldNotes)
        {
            held.pitch  = -1;
            held.slotId = -1;
        }

        return;
    }

    // A note-off never knew which synth started it — the key only knows its
    // pitch, and by the time it is released the spotlight may have moved. The
    // table the note-on wrote is what puts the release back on the right synth;
    // without it, a hosted instrument would keep the note down for ever while
    // the oscillator voice politely stopped.
    if (! event.on)
        for (auto& held : liveHeldNotes)
            if (held.pitch == event.pitch)
            {
                event.slotId = held.slotId;
                held.pitch   = -1;
                held.slotId  = -1;
                break;
            }

    // Where a note-on goes is written down HERE, before the two kinds of synth
    // split apart.
    //
    // Phase 41: this line used to sit at the bottom of the oscillator branch,
    // which meant a hosted instrument — the one that returns before reaching it
    // — never recorded where its note went. Its note-off then arrived with no
    // slot id, matched nothing, and was dropped: every key left the VST holding
    // its note, and the synth droned until the session ended. The rule is
    // "every note-on remembers its synth", so it belongs where every note-on
    // passes, not inside one of the two branches that should be remembering it.
    if (event.on)
        for (auto& held : liveHeldNotes)
            if (held.pitch == event.pitch || held.pitch < 0)
            {
                held.pitch  = event.pitch;
                held.slotId = event.slotId;
                break;
            }

    //-- The release always happens, whatever the schedule says now ----------
    // A synth that was deleted mid-note must still let go of the note.
    if (! event.on)
    {
        for (auto& voice : liveVoices)
            if (voice.active && ! voice.released && voice.pitch == event.pitch)
                voice.released = true;

        int active = 0;

        for (const auto& voice : liveVoices)
            if (voice.active)
                ++active;

        liveVoicesActive.store (active, std::memory_order_relaxed);
    }

    auto current = getScheduleRaw();

    const SlotMix* slot = nullptr;

    if (current != nullptr)
        for (const auto& candidate : current->slots)
            if (candidate.slotId == event.slotId)
            {
                slot = &candidate;
                break;
            }

    //-- A hosted instrument: this is MIDI, not a voice ------------------------
    if (slot != nullptr && slot->pluginId > 0)
    {
        auto* live = findLivePluginSlot (event.slotId);

        if (live == nullptr && event.on)
            for (auto& candidate : livePluginSlots)
                if (candidate.slotId < 0)
                {
                    candidate.slotId     = event.slotId;
                    candidate.numHeld    = 0;
                    candidate.tailFrames = 0;
                    live = &candidate;
                    break;
                }

        if (live == nullptr)
            return;

        if (event.on)
        {
            if (live->numHeld < (int) live->held.size())
                live->held[(size_t) live->numHeld++] = event.pitch;

            live->tailFrames = (int) (sampleRate * kLivePluginTailSeconds);
        }
        else
        {
            for (int i = 0; i < live->numHeld; ++i)
                if (live->held[(size_t) i] == event.pitch)
                {
                    for (int j = i; j < live->numHeld - 1; ++j)
                        live->held[(size_t) j] = live->held[(size_t) j + 1];

                    --live->numHeld;
                    break;
                }
        }

        int engaged = 0;

        for (const auto& candidate : livePluginSlots)
            if (candidate.slotId >= 0)
                ++engaged;

        livePluginEngaged.store (engaged, std::memory_order_relaxed);
        return;
    }

    if (! event.on)
        return;

    // A slot that holds one file (a sampler) or does not exist has nothing to
    // play. Inventing a sine for it would make "no target synth" a lie, and the
    // settings page says exactly that when there is none.
    if (slot == nullptr || slot->sampler)
        return;

    //-- An oscillator: a real voice with a beginning and an end --------------
    // Same pitch pressed again: retrigger in place. A second voice at the same
    // pitch would just be the first one, 20 % louder.
    auto* voice = findLiveVoice (event.pitch, true);

    if (voice == nullptr)
        voice = &liveVoices[(size_t) freeLiveVoiceSlot()];

    const bool continuing = voice->active && ! voice->released && voice->pitch == event.pitch;

    voice->slotId   = event.slotId;
    voice->pitch    = event.pitch;
    voice->velocity = event.velocity;
    voice->waveform = slot->waveform;

    const double liveBeat = positionBeats.load (std::memory_order_relaxed);

    // Phase 43: `gainAt` is the fader × the mute gate × the level curve, and it
    // no longer knows about solo. A key struck on a synth that is silent because
    // another one is soloed still has to be silent, so `audible` is applied here
    // — it is the publish-time answer to exactly that question.
    voice->gain     = slot->audible ? slot->gainAt (liveBeat) : 0.0f;
    voice->pan      = slot->panAt (liveBeat);

    if (! continuing)
    {
        voice->phase = 0.0;
        voice->amp   = 0.0f;              // ramps up: a key is never a step
    }

    voice->released = false;
    voice->active   = true;

    // The pitch → synth memory was written above, for both kinds of synth.

    int active = 0;

    for (const auto& counting : liveVoices)
        if (counting.active)
            ++active;

    liveVoicesActive.store (active, std::memory_order_relaxed);
}

//==============================================================================
void PlaybackEngine::renderLive (juce::AudioBuffer<float>& output, int numSamples)
{
    if (numSamples <= 0 || output.getNumChannels() <= 0 || sampleRate <= 0.0)
        return;

    auto current = getScheduleRaw();

    // Phase 71: before anything is rendered, drop the keys whose instrument no
    // longer exists. Their table entries are what made eight deleted
    // instruments the end of live playback.
    pruneLivePluginSlots (current);

    const float masterGain = current != nullptr ? current->masterGain : kMasterGain;
    const float masterPan  = current != nullptr ? current->masterPan  : 0.0f;

    if (liveVoicesActive.load (std::memory_order_relaxed) > 0)
        renderLiveOscillators (current, masterGain, masterPan, output, numSamples);

    if (livePluginEngaged.load (std::memory_order_relaxed) > 0)
        renderLivePlugins (current, masterGain, output, numSamples);
}

void PlaybackEngine::renderLiveOscillators (const PublishedSchedule* current, float masterGain,
                                            float masterPan, juce::AudioBuffer<float>& output,
                                            int numSamples)
{
    const float masterAngle = juce::jlimit (-1.0f, 1.0f, masterPan);
    const float masterL     = masterGain * juce::jmin (1.0f, 1.0f - masterAngle);
    const float masterR     = masterGain * juce::jmin (1.0f, 1.0f + masterAngle);

    // 3 ms in, 60 ms out. Short enough that a key feels immediate, long enough
    // that neither edge is a click — the same job the audition voice's 4 ms
    // one-pole does, split into the two directions a held key actually has.
    const float attack  = (float) (1.0 - std::exp (-1.0 / (0.003 * sampleRate)));
    const float release = (float) (1.0 - std::exp (-1.0 / (0.060 * sampleRate)));

    float* left  = output.getWritePointer (0);
    float* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

    const double beat = positionBeats.load (std::memory_order_relaxed);
    int   active = 0;
    float peak   = 0.0f;

    for (auto& voice : liveVoices)
    {
        if (! voice.active)
            continue;

        // Re-read the synth's own strip every block: moving that fader with a
        // chord held down has to be heard, exactly like it is on the timeline.
        if (current != nullptr)
            for (const auto& slot : current->slots)
                if (slot.slotId == voice.slotId)
                {
                    voice.gain     = slot.audible ? slot.gainAt (beat) : 0.0f;
                    voice.pan      = slot.panAt (beat);
                    voice.waveform = slot.waveform;
                    break;
                }

        const float angle = juce::jlimit (-1.0f, 1.0f, voice.pan);
        const float levelL = voice.gain * juce::jmin (1.0f, 1.0f - angle) * masterL;
        const float levelR = voice.gain * juce::jmin (1.0f, 1.0f + angle) * masterR;

        const double increment = pitchToFrequency (voice.pitch) / sampleRate;
        const float  target    = voice.released ? 0.0f : 1.0f;
        const float  coef      = voice.released ? release : attack;

        for (int i = 0; i < numSamples; ++i)
        {
            voice.amp += (target - voice.amp) * coef;

            const float mono = oscillator (voice.waveform, voice.phase, increment)
                                 * voice.amp * voice.velocity;

            left[i] += mono * levelL;

            if (right != nullptr)
                right[i] += mono * levelR;

            peak = juce::jmax (peak, std::abs (mono));

            voice.phase += increment;
        }

        if (voice.released && voice.amp < 1.0e-4f)
        {
            voice.active   = false;
            voice.released = false;
            voice.amp      = 0.0f;
            voice.phase    = 0.0;
            continue;
        }

        ++active;
    }

    liveVoicesActive.store (active, std::memory_order_relaxed);

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& meter = masterMeters[(size_t) ch];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), peak * masterGain),
                     std::memory_order_relaxed);
    }
}

void PlaybackEngine::renderLivePlugins (const PublishedSchedule* current, float masterGain,
                                        juce::AudioBuffer<float>& output, int numSamples)
{
    if (pluginHost == nullptr || current == nullptr)
        return;

    const int chunkMax = juce::jmax (1, juce::jmin (pluginHost->getRenderBlockSize(),
                                                    livePluginBuffer.getNumSamples()));

    if (livePluginBuffer.getNumChannels() < 2)
        return;

    const double beat = positionBeats.load (std::memory_order_relaxed);
    int engaged = 0;

    for (auto& live : livePluginSlots)
    {
        if (live.slotId < 0)
            continue;

        if (live.numHeld == 0 && live.tailFrames <= 0)
        {
            live.slotId = -1;             // finished: the slot is reusable
            continue;
        }

        const SlotMix* slot = nullptr;

        for (const auto& candidate : current->slots)
            if (candidate.slotId == live.slotId)
            {
                slot = &candidate;
                break;
            }

        auto* plugin = slot != nullptr && slot->pluginId > 0
                         ? pluginHost->getInstanceWhileLocked (slot->pluginId)
                         : nullptr;

        // Phase 71: the instrument this key went to is gone — its slot was
        // deleted, re-mounted, or its instance unloaded. Nothing will ever
        // render the note again, so the entry has to be given back: leaving it
        // here burned one of the eight live slots for the rest of the session,
        // and a key pressed later could match it by accident.
        if (slot == nullptr || slot->pluginId <= 0 || plugin == nullptr)
        {
            live.slotId     = -1;
            live.numHeld    = 0;
            live.tailFrames = 0;
            continue;
        }

        ++engaged;

        bool alreadyRendered = false;

        for (int i = 0; i < numTransportPluginSlots; ++i)
            if (transportPluginSlots[(size_t) i] == live.slotId)
                alreadyRendered = true;

        // The transport rendered this plugin in this same callback. Sending it
        // a second processBlock would not double its output — it would double
        // the rate its own DSP runs at, and that is a bug nobody would guess.
        if (alreadyRendered)
            continue;

        const int channels = juce::jlimit (1, 2, plugin->getTotalNumOutputChannels());
        const float gain   = (slot->audible ? slot->gainAt (beat) : 0.0f) * masterGain;
        const float balanceL = juce::jmin (1.0f, 1.0f - slot->panAt (beat));
        const float balanceR = juce::jmin (1.0f, 1.0f + slot->panAt (beat));

        float peak = 0.0f;

        for (int offset = 0; offset < numSamples; )
        {
            const int len = juce::jmin (chunkMax, numSamples - offset);

            juce::AudioBuffer<float> view (livePluginBuffer.getArrayOfWritePointers(),
                                           channels, 0, len);
            view.clear();

            liveMidi.clear();

            // Only on the first chunk: the block's events belong at its start,
            // and sending them once per chunk would play the note several times.
            if (offset == 0)
                for (int e = 0; e < pendingLiveCount; ++e)
                {
                    if (pendingLive[(size_t) e].slotId != live.slotId)
                        continue;

                    const auto& event = pendingLive[(size_t) e];

                    if (event.on)
                        liveMidi.addEvent (juce::MidiMessage::noteOn (1, event.pitch, event.velocity), 0);
                    else
                        liveMidi.addEvent (juce::MidiMessage::noteOff (1, event.pitch), 0);
                }

            plugin->processBlock (view, liveMidi);

            for (int ch = 0; ch < channels && ch < output.getNumChannels(); ++ch)
            {
                const float level = gain * (ch == 0 ? balanceL : balanceR);
                output.addFrom (ch, offset, view, ch, 0, len, level);
                peak = juce::jmax (peak, view.getMagnitude (ch, 0, len) * std::abs (level));
            }

            offset += len;
        }

        live.tailFrames = juce::jmax (0, live.tailFrames - numSamples);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& meter = masterMeters[(size_t) ch];
            meter.store (juce::jmax (meter.load (std::memory_order_relaxed), peak),
                         std::memory_order_relaxed);
        }
    }

    livePluginEngaged.store (engaged, std::memory_order_relaxed);
}

//==============================================================================
void PlaybackEngine::previewSample (std::shared_ptr<const SampleBuffer> buffer, double seconds)
{
    if (buffer == nullptr || buffer->numFrames() <= 0)
    {
        stopSamplePreview();
        return;
    }

    // Payload under the lock, serial after it: the release store is the publish
    // point, exactly like the note preview above.
    {
        const juce::SpinLock::ScopedLockType lock (previewLock);

        // Phase 70: this is where the previous sample dies — message thread,
        // which is allowed to free eight megabytes. The audio thread only ever
        // parks its outgoing buffer here.
        previewRetired.reset();

        previewPending = std::move (buffer);
    }

    previewSeconds.store (juce::jlimit (0.1, 60.0, seconds), std::memory_order_relaxed);
    previewEngaged.store (true, std::memory_order_relaxed);

    previewSerial.fetch_add (1, std::memory_order_release);
}

void PlaybackEngine::stopSamplePreview()
{
    previewEngaged.store (false, std::memory_order_relaxed);

    // Phase 70: the voice ramps down over 4 ms and stops itself (it reads
    // `engaged` every sample now). This is the message thread, so it is also
    // the right place to release the buffer the audio thread parked.
    {
        const juce::SpinLock::ScopedLockType lock (previewLock);
        previewRetired.reset();
    }

    // Deliberately does NOT bump the serial: that would mean "here is a new
    // thing to play".
}

void PlaybackEngine::renderSamplePreview (juce::AudioBuffer<float>& output, int numSamples)
{
    if (numSamples <= 0 || output.getNumChannels() <= 0 || sampleRate <= 0.0)
        return;

    const int  serial  = previewSerial.load (std::memory_order_acquire);
    const bool engaged = previewEngaged.load (std::memory_order_relaxed);

    if (! engaged && ! previewVoice.active && serial == previewVoice.serial)
        return;

    if (serial != previewVoice.serial)
    {
        const juce::SpinLock::ScopedTryLockType lock (previewLock);

        if (! lock.isLocked())
            return;             // message thread is mid-handoff; try next block

        // Phase 70: park the outgoing buffer instead of letting the assignment
        // release it. When the message thread has already dropped its own
        // reference, this was the last one — so the free of the whole sample
        // happened here, on the audio thread. The message thread frees it on
        // the next hand-off.
        previewRetired      = std::move (previewVoice.buffer);

        previewVoice.serial   = serial;
        previewVoice.buffer   = previewPending;
        previewVoice.position = 0.0;
        previewVoice.gain     = 0.0f;
        previewVoice.active   = previewVoice.buffer != nullptr;

        if (const auto* buffer = previewVoice.buffer.get())
        {
            // Never play more than what is there, and never more than the
            // requested window: a two-second snare gets two seconds of preview
            // and a three-second sample gets three.
            const double wanted = previewSeconds.load (std::memory_order_relaxed)
                                    * buffer->sampleRate;

            previewVoice.frames = juce::jmin ((double) buffer->numFrames(), wanted);
        }
        else
        {
            previewVoice.frames = 0.0;
        }

        previewConsumed.store (serial, std::memory_order_release);

        if (! previewVoice.active)
            return;
    }

    if (! previewVoice.active)
        return;

    const auto* buffer = previewVoice.buffer.get();

    if (buffer == nullptr || buffer->channels <= 0 || buffer->frames.empty())
    {
        previewVoice.active = false;
        return;
    }

    auto current = getScheduleRaw();

    const float masterGain = current != nullptr ? current->masterGain : kMasterGain;
    const float masterPan  = current != nullptr ? current->masterPan  : 0.0f;

    // Same balance law as the master strip and the note preview: a sample that
    // bypasses it would be the only sound in the app that ignores the master.
    const float levelL = masterGain * juce::jmin (1.0f, 1.0f - masterPan);
    const float levelR = masterGain * juce::jmin (1.0f, 1.0f + masterPan);

    // Source frames per output frame. 44.1k material on a 48k device needs
    // resampling or the preview plays at the wrong pitch — and pitch is most of
    // what a user is judging when they audition a sample.
    const double step = buffer->sampleRate > 0.0 ? buffer->sampleRate / sampleRate : 1.0;

    const float  smoothing  = (float) (1.0 - std::exp (-1.0 / (0.004 * sampleRate)));
    const double fadeFrames = 0.012 * buffer->sampleRate;    // 12 ms at each end

    float* left  = output.getWritePointer (0);
    float* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

    const int    channels  = buffer->channels;
    const double numFrames = (double) buffer->numFrames();

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        if (! previewVoice.active || previewVoice.position >= previewVoice.frames
              || previewVoice.position >= numFrames)
        {
            previewVoice.active = false;
            break;
        }

        // Phase 70: a stop is a request and the voice has to honour it. This
        // loop used to ignore `engaged` entirely — `stopSamplePreview()` only
        // cleared the flag, so a 60-second audition played on to the end and
        // hasSamplePreview() kept answering true for the whole minute.
        const bool stopRequested = ! engaged;

        float target = stopRequested ? 0.0f : 1.0f;

        // Fade the tail out instead of letting the last frame end on a step.
        if (! stopRequested)
        {
            const double remaining = previewVoice.frames - previewVoice.position;

            if (remaining < fadeFrames)
                target = (float) juce::jmax (0.0, remaining / fadeFrames);
        }

        previewVoice.gain += (target - previewVoice.gain) * smoothing;

        // The same 4 ms ramp that ends a sample naturally also ends a stopped
        // one: once it is inaudible, there is nothing left to play.
        if (stopRequested && previewVoice.gain < 1.0e-4f)
        {
            previewVoice.gain   = 0.0f;
            previewVoice.active = false;
            break;
        }

        const int frame = (int) previewVoice.position;

        const float l = buffer->frames[(size_t) frame * (size_t) channels];
        const float r = channels > 1 ? buffer->frames[(size_t) frame * (size_t) channels + 1] : l;

        left[i] += l * previewVoice.gain * levelL;

        if (right != nullptr)
            right[i] += r * previewVoice.gain * levelR;

        peak = juce::jmax (peak, std::abs (l) * previewVoice.gain);

        previewVoice.position += step;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& meter = masterMeters[(size_t) ch];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), peak * masterGain),
                     std::memory_order_relaxed);
    }

    if (! previewVoice.active)
        previewEngaged.store (false, std::memory_order_relaxed);
}

//==============================================================================
RenderStats PlaybackEngine::renderOffline (double seconds) const
{
    RenderStats stats;

    auto current = getSchedule();

    if (current == nullptr)
        return stats;

    const int numSamples = juce::jmax (1, (int) std::llround (seconds * sampleRate));
    juce::AudioBuffer<float> buffer (2, numSamples);
    renderInto (*current, 0.0, numSamples, buffer, &stats);
    return stats;
}

void PlaybackEngine::renderWindow (double startSeconds, int numSamples,
                                   juce::AudioBuffer<float>& output, RenderStats* stats) const
{
    auto current = getScheduleRaw();

    if (current == nullptr || numSamples <= 0)
        return;

    if (output.getNumChannels() < 1 || output.getNumSamples() != numSamples)
        output.setSize (2, numSamples);

    renderInto (*current, startSeconds, numSamples, output, stats);
}

void PlaybackEngine::renderScheduleSpan (const PublishedSchedule& current, double startSeconds,
                                         int numSamples, juce::AudioBuffer<float>& destination,
                                         int destinationOffset, RenderStats* stats) const
{
    if (numSamples <= 0
         || destinationOffset < 0
         || destination.getNumChannels() < 1
         || destinationOffset + numSamples > destination.getNumSamples())
        return;

    // A view onto the destination: no copy, and renderInto clears exactly the
    // slice it is about to fill.
    juce::AudioBuffer<float> slice (destination.getArrayOfWritePointers(),
                                    destination.getNumChannels(),
                                    destinationOffset, numSamples);

    renderInto (current, startSeconds, numSamples, slice, stats);
}

//==============================================================================
float PlaybackEngine::getSlotPeak (int slotId) const
{
    if (slotId < 0)
        return 0.0f;

    const int index = juce::jlimit (0, maxMeteredSlots - 1, slotId % maxMeteredSlots);
    return meters[(size_t) index].load (std::memory_order_relaxed);
}

float PlaybackEngine::getLayerPeak (int layerIndex) const
{
    if (! juce::isPositiveAndBelow (layerIndex, maxMeteredLayers))
        return 0.0f;

    return layerMeters[(size_t) layerIndex].load (std::memory_order_relaxed);
}

float PlaybackEngine::getChannelPeak (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, MixerModel::numChannels))
        return 0.0f;

    return juce::jmax (channelMetersL[(size_t) channel].load (std::memory_order_relaxed),
                       channelMetersR[(size_t) channel].load (std::memory_order_relaxed));
}

float PlaybackEngine::getChannelPeakSide (int channel, int side) const
{
    if (! juce::isPositiveAndBelow (channel, MixerModel::numChannels))
        return 0.0f;

    return (side == 0 ? channelMetersL : channelMetersR)[(size_t) channel]
             .load (std::memory_order_relaxed);
}

float PlaybackEngine::getMasterPeak (int channel) const
{
    return masterMeters[(size_t) juce::jlimit (0, 1, channel)].load (std::memory_order_relaxed);
}

void PlaybackEngine::decayMeters (int numSamples)
{
    // 50 dB per second, over exactly this block. The number is a feel decision
    // (fast enough that a stopped song reads as "off", slow enough to see a
    // transient), the FORM is the part that matters: a fall expressed in the
    // block's own duration is the same fall at any sample rate or buffer size.
    const double dt = sampleRate > 0.0
                        ? (double) juce::jmax (0, numSamples) / sampleRate
                        : 0.0;

    const float factor = (float) std::pow (10.0, -2.5 * dt);   // -50 dB/s

    for (auto& meter : meters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : layerMeters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : channelMetersL)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : channelMetersR)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : masterMeters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);
}

bool PlaybackEngine::hasMeters() const noexcept
{
    // The floor is the same one the window treats as "empty", so "the bars have
    // settled" and "nobody needs to repaint them" are the same answer.
    auto anyAbove = [] (const auto& set) -> bool
    {
        for (const auto& meter : set)
            if (meter.load (std::memory_order_relaxed) > 1.0e-4f)
                return true;

        return false;
    };

    return anyAbove (meters) || anyAbove (layerMeters)
             || anyAbove (channelMetersL) || anyAbove (channelMetersR)
             || anyAbove (masterMeters);
}

void PlaybackEngine::resetMeters()
{
    for (auto& meter : meters)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : layerMeters)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : channelMetersL)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : channelMetersR)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : masterMeters)
        meter.store (0.0f, std::memory_order_relaxed);
}

void PlaybackEngine::resetMixState()
{
    resetMeters();

    for (auto& state : slotFilters)
    {
        state.zL = 0.0f;
        state.zR = 0.0f;
    }

    // A bounce must not inherit a delay tail from whatever was playing live.
    for (auto& chain : channelChains)
        chain.reset();
}

//==============================================================================
int PlaybackEngine::countNotesRoutedTo (int slotId) const
{
    auto current = getScheduleRaw();

    if (current == nullptr)
        return 0;

    int count = 0;

    for (const auto& note : current->notes)
        if (note.slotId == slotId)
            ++count;

    return count;
}

int PlaybackEngine::countOverridesRoutedTo (int slotId) const
{
    auto current = getScheduleRaw();

    if (current == nullptr)
        return 0;

    int count = 0;

    for (const auto& note : current->notes)
        if (note.slotId == slotId && note.overridden)
            ++count;

    return count;
}

} // namespace atomic

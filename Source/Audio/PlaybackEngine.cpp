#include "PlaybackEngine.h"

#include "PluginHost.h"

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

    layerPeaks.emplace_back (layerIndex, newPeak);
}

float RenderStats::peakForLayer (int layerIndex) const
{
    for (const auto& entry : layerPeaks)
        if (entry.first == layerIndex)
            return entry.second;

    return 0.0f;
}

void RenderStats::addChannelPeak (int channel, float newPeak)
{
    for (auto& entry : channelPeaks)
    {
        if (entry.first == channel)
        {
            entry.second = juce::jmax (entry.second, newPeak);
            return;
        }
    }

    channelPeaks.emplace_back (channel, newPeak);
}

float RenderStats::peakForChannel (int channel) const
{
    for (const auto& entry : channelPeaks)
        if (entry.first == channel)
            return entry.second;

    return 0.0f;
}

//==============================================================================
PlaybackEngine::PlaybackEngine()
{
    for (auto& meter : meters)
        meter.store (0.0f);

    for (auto& meter : layerMeters)
        meter.store (0.0f);

    for (auto& meter : channelMeters)
        meter.store (0.0f);

    for (auto& meter : masterMeters)
        meter.store (0.0f);

    mixScratch.setSize (2, 1024);

    // Delay lines must exist before the first render, even if nobody ever calls
    // setSampleRate (the headless tests render straight from a fresh engine).
    for (auto& chain : channelChains)
        chain.prepare (sampleRate);
}

void PlaybackEngine::setSampleRate (double newSampleRate)
{
    if (newSampleRate <= 0.0)
        return;

    sampleRate = newSampleRate;

    // Insert memory is sized by sample rate (delay buffers), so it is rebuilt
    // here — on the message thread — never in the callback.
    for (auto& chain : channelChains)
        chain.prepare (sampleRate);
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
    if (mixer != nullptr)
    {
        for (int channel = 0; channel < MixerModel::numChannels; ++channel)
        {
            ChannelMix mix;
            mix.channel = channel;
            mix.inserts = mixer->getInserts (channel);
            fresh->channels.push_back (std::move (mix));
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
        }

        //-- Phase 17: audio clips ------------------------------------------
        // A clip follows its LAYER's output. It has no note to carry an
        // override and no group to be mounted through, so routing::layerSlotId
        // is the whole rule here — and it is still the same resolver, so a
        // deleted synth falls back exactly like a note would.
        const int clipSlot = routing::layerSlotId (*layer, rack);

        if (clipSlot >= 0)
        {
            for (const auto& clip : layer->getSamples())
            {
                if (clip.audio == nullptr)
                    continue;               // missing file: a hole in the audio, not a crash

                ScheduledClip scheduled;
                scheduled.slotId       = clipSlot;
                scheduled.layerIndex   = layerIndex;
                scheduled.clipId       = clip.id;
                scheduled.startSeconds = clip.startBeat * secondsPerBeat;
                scheduled.endSeconds   = juce::jmax (scheduled.startSeconds + 0.01,
                                                     clip.endBeat() * secondsPerBeat);
                scheduled.gain         = gain::fromDb (clip.gainDb);
                scheduled.audio        = clip.audio;

                contentBeats = juce::jmax (contentBeats, clip.endBeat());
                fresh->clips.push_back (std::move (scheduled));
            }
        }
    }

    std::sort (fresh->notes.begin(), fresh->notes.end(),
               [] (const ScheduledNote& a, const ScheduledNote& b)
               { return a.startSeconds < b.startSeconds; });

    std::sort (fresh->clips.begin(), fresh->clips.end(),
               [] (const ScheduledClip& a, const ScheduledClip& b)
               { return a.startSeconds < b.startSeconds; });

    //-- Master strip ---------------------------------------------------------
    if (mixer != nullptr)
    {
        fresh->masterInserts = mixer->getInserts (MixerModel::masterChannel);
        fresh->masterGain    = mixer->getMasterGainLinear();
        fresh->masterPan     = mixer->getMasterPan();

        const auto& levelLane = mixer->automationFor (0);
        const auto& panLane   = mixer->automationFor (1);

        fresh->masterLevelAutomated = levelLane.isEnabled();
        fresh->masterPanAutomated   = panLane.isEnabled();
        fresh->masterLevelPoints    = levelLane.getPoints();
        fresh->masterPanPoints      = panLane.getPoints();
    }

    // Never shorter than the project grid, never shorter than the content, and
    // never zero — a zero-length timeline would make the transport divide by it.
    fresh->contentBeats = contentBeats;
    fresh->lengthBeats = juce::jmax (juce::jmax (timelineBeats, requestedLengthBeats),
                                     juce::jmax (contentBeats, 1.0));

    const double newLengthBeats = fresh->lengthBeats;
    const double newLengthSecs  = fresh->lengthSeconds();

    {
        // The lock covers a shared_ptr assignment and nothing else. No note is
        // copied and no audio is computed while it is held.
        const juce::SpinLock::ScopedLockType guard (scheduleLock);
        schedule = std::move (fresh);
    }

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

        if (! slot.audible)
            continue;

        const float slotGain0 = slot.gainAt (startBeat);
        const float slotGain1 = slot.gainAt (endBeat);

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

            // Pan is the layer's pan PLUS the synth's pan PLUS the group's,
            // constant-power. The stages are summed rather than multiplied
            // because a pan is an angle: adding two half-lefts should not
            // cancel out. Phase 20 makes the group's term a ramp (pan0..pan1)
            // so a group pan curve sweeps instead of stepping per block.
            const float groupPan0 = gb != nullptr ? gb->pan0 : 0.0f;
            const float groupPan1 = gb != nullptr ? gb->pan1 : 0.0f;

            const float p0 = juce::jlimit (-1.0f, 1.0f, lb.pan0 + slot.pan + groupPan0);
            const float p1 = juce::jlimit (-1.0f, 1.0f, lb.pan1 + slot.pan + groupPan1);

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
                    const float spread = juce::jlimit (0.0f, 1.0f, env) * 2.0f - 1.0f;
                    const float pp     = juce::jlimit (-1.0f, 1.0f, p0 + spread);

                    panL = std::cos ((pp + 1.0f) * kPiOver4);
                    panR = std::sin ((pp + 1.0f) * kPiOver4);
                }
                else if (modPitch)
                {
                    // The tabulated integral, in cycles: frequency * length is
                    // how many cycles one octave of bend is worth over this note.
                    phase += frequency * length * (double) note.pitchIntegralAt (u);
                }

                float mono = oscillator (note.waveform, phase, increment)
                               * amp * fade * note.gain * layerGain * slotGain * groupGain;

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

            const float p0 = juce::jlimit (-1.0f, 1.0f, lb.pan0 + slot.pan);
            const float p1 = juce::jlimit (-1.0f, 1.0f, lb.pan1 + slot.pan);

            const float balL0 = juce::jmin (1.0f, 1.0f - p0);
            const float balR0 = juce::jmin (1.0f, 1.0f + p0);
            const float balL1 = juce::jmin (1.0f, 1.0f - p1);
            const float balR1 = juce::jmin (1.0f, 1.0f + p1);

            auto& bus = channelBuses[(size_t) busIndexFor (slot.mixerChannel)];
            float* busL = bus.getWritePointer (0);
            float* busR = bus.getWritePointer (1);

            const float* src = buffer.frames.data();

            float clipPeak = 0.0f;

            for (int i = first; i < last; ++i)
            {
                const double elapsed   = (startSeconds + (double) i / sampleRate) - clip.startSeconds;
                const double remaining = length - elapsed;

                // Linear interpolation. The clip is placed on a beat grid, not a
                // sample grid, so its read position is almost never an integer;
                // at unity rate this is a bit-exact copy, and at other rates a
                // first-order resample is the honest 1:1 trade for staying
                // stateless.
                const double srcPos = elapsed * buffer.sampleRate;

                if (srcPos >= (double) (srcFrames - 1))
                    break;

                const int   i0   = (int) srcPos;
                const float frac = (float) (srcPos - (double) i0);

                const float* a = src + (size_t) i0 * (size_t) srcChannels;
                // One frame of look-ahead, clamped at the tail so the last
                // frame repeats instead of reading past the buffer.
                const float* b = (i0 + 1 < srcFrames) ? a + (size_t) srcChannels : a;

                const float left  = a[0] + (b[0] - a[0]) * frac;
                const float right = srcChannels > 1 ? (a[1] + (b[1] - a[1]) * frac) : left;

                const float t = (float) i * invNumSamples;
                const float gain = clip.gain
                                     * (lb.gain0 + (lb.gain1 - lb.gain0) * t)
                                     * (slotGain0 + (slotGain1 - slotGain0) * t)
                                     * (float) juce::jlimit (0.0, 1.0,
                                                            juce::jmin (elapsed, remaining) / kClickFadeSecond);

                busL[i] += left  * gain * (balL0 + (balL1 - balL0) * t);
                busR[i] += right * gain * (balR0 + (balR1 - balR0) * t);

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
    for (int channel = 0; channel < MixerModel::numChannels; ++channel)
    {
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

        for (int ch = 0; ch < numChannels; ++ch)
            output.addFrom (ch, 0, bus, juce::jmin (ch, 1), 0, numSamples);

        if (stats != nullptr)
            stats->addChannelPeak (channel, bus.getMagnitude (0, 0, numSamples));
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

    const float level0 = current.masterGain
                           * (current.masterLevelAutomated
                                ? automationValueAt (current.masterLevelPoints, startBeat) : 1.0f);
    const float level1 = current.masterGain
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

    for (const auto& slot : current.slots)
    {
        if (slot.pluginId <= 0 || ! slot.audible)
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
        // image, and squaring it away would break it.
        const float balanceL = juce::jmin (1.0f, 1.0f - slot.pan);
        const float balanceR = juce::jmin (1.0f, 1.0f + slot.pan);

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

                if (offOffset <= (double) chunkStart || onOffset >= (double) (chunkStart + chunkLen))
                    continue;

                // A muted layer has to be silent on a plugin too, and a plugin
                // bus cannot be muted per layer — so its notes simply never get
                // a note-on. The note-off is still sent in case one was already
                // sounding when the layer was muted.
                bool layerAudible = true;

                for (const auto& mix : current.layers)
                    if (mix.layerIndex == note.layerIndex)
                        layerAudible = mix.audible;

                const int on  = (int) std::llround (onOffset)  - chunkStart;
                const int off = (int) std::llround (offOffset) - chunkStart;

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

            // Plugins are not required to clear their output, and a leftover
            // from the last chunk is exactly the kind of noise nobody can
            // explain later.
            pluginBuffer.clear (0, chunkLen);

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

        if (stats != nullptr)
            stats->addPeak (slot.slotId, slotPeak);
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
    auto current = getSchedule();

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

    RenderStats stats;

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

    for (auto& meter : channelMeters)
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
        if (! juce::isPositiveAndBelow (entry.first, MixerModel::numChannels))
            continue;

        auto& meter = channelMeters[(size_t) entry.first];
        meter.store (juce::jmax (meter.load (std::memory_order_relaxed), entry.second),
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

void PlaybackEngine::renderAudition (juce::AudioBuffer<float>& output, int numSamples)
{
    if (numSamples <= 0 || output.getNumChannels() <= 0 || sampleRate <= 0.0)
        return;

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

        if (auto current = getSchedule())
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

    if (! auditionVoice.active)
        return;

    auto current = getSchedule();

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
        previewPending = std::move (buffer);
    }

    previewSeconds.store (juce::jlimit (0.1, 60.0, seconds), std::memory_order_relaxed);
    previewEngaged.store (true, std::memory_order_relaxed);

    previewSerial.fetch_add (1, std::memory_order_release);
}

void PlaybackEngine::stopSamplePreview()
{
    previewEngaged.store (false, std::memory_order_relaxed);

    // The voice notices on its next block. Deliberately does NOT bump the
    // serial: that would mean "here is a new thing to play".
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

    auto current = getSchedule();

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

        float target = 1.0f;

        // Fade the tail out instead of letting the last frame end on a step.
        const double remaining = previewVoice.frames - previewVoice.position;

        if (remaining < fadeFrames)
            target = (float) juce::jmax (0.0, remaining / fadeFrames);

        previewVoice.gain += (target - previewVoice.gain) * smoothing;

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
    auto current = getSchedule();

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

    return channelMeters[(size_t) channel].load (std::memory_order_relaxed);
}

float PlaybackEngine::getMasterPeak (int channel) const
{
    return masterMeters[(size_t) juce::jlimit (0, 1, channel)].load (std::memory_order_relaxed);
}

void PlaybackEngine::decayMeters (float factor)
{
    for (auto& meter : meters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : layerMeters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : channelMeters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);

    for (auto& meter : masterMeters)
        meter.store (meter.load (std::memory_order_relaxed) * factor, std::memory_order_relaxed);
}

void PlaybackEngine::resetMeters()
{
    for (auto& meter : meters)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : layerMeters)
        meter.store (0.0f, std::memory_order_relaxed);

    for (auto& meter : channelMeters)
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
    auto current = getSchedule();

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
    auto current = getSchedule();

    if (current == nullptr)
        return 0;

    int count = 0;

    for (const auto& note : current->notes)
        if (note.slotId == slotId && note.overridden)
            ++count;

    return count;
}

} // namespace atomic

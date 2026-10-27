#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <memory>

namespace atomic
{
namespace fx
{

//==============================================================================
/** The insert set. Small and deliberate: each one is audible in a second, and
    none of them needs a state that the block renderer cannot freeze. */
enum class Type
{
    none,
    lowPass,
    drive,
    delay,
    air,
    vst,        // a hosted VST3; the DSP lives in PluginHost, not here
    eq,         // Phase 46: tilt EQ — appended AFTER vst, never before it:
                // the project file stores the raw int, and old sessions carry
                // 5 = vst. Reordering would turn every saved VST3 into an EQ.
    comp,       // Phase 46: level-dependent gain rider
    reverb      // Phase 46: 4 combs + 2 allpasses per channel
};

inline int numTypes() noexcept { return 9; }

/** The built-in effects, for the "add insert" menu. `vst` is not in this list
    because it is chosen from the plugin submenu instead. NOTE: vst sits in the
    MIDDLE of the enum (value 5, frozen by old project files), so every menu
    that walks the enum must skip it explicitly — `type < numBuiltInTypes()`
    would have served 6/7/8 as vst. */
inline int numBuiltInTypes() noexcept { return 8; }

inline juce::String typeName (Type type)
{
    switch (type)
    {
        case Type::lowPass: return "LOW PASS";
        case Type::drive:   return "DRIVE";
        case Type::delay:   return "DELAY";
        case Type::air:     return "AIR";
        case Type::vst:     return "VST3";
        case Type::eq:      return "EQ";
        case Type::comp:    return "COMP";
        case Type::reverb:  return "REVERB";
        case Type::none:
        default:            return "EMPTY";
    }
}

inline Type typeFromInt (int value) noexcept
{
    return (value >= 0 && value < numTypes()) ? (Type) value : Type::none;
}

/** Label for the two knobs, so the rack does not show "P1 / P2". */
inline const char* paramName (Type type) noexcept
{
    switch (type)
    {
        case Type::lowPass: return "CUT";
        case Type::drive:   return "DRV";
        case Type::delay:   return "TIME";
        case Type::air:     return "AIR";
        case Type::vst:     return "VST";
        case Type::eq:      return "TILT";   // 0.5 = 平, 拉向两端 = 偏低 / 偏高
        case Type::comp:    return "AMT";    // 越大阈值越低、比率越硬
        case Type::reverb:  return "SIZE";   // 房间大小, 湿量固定
        case Type::none:
        default:            return "—";
    }
}

//==============================================================================
/** One insert, as stored in the model.

    A VST3 insert keeps its identity in `plugin` — a shared description, so the
    project file can write the plugin down and a later load can find it again —
    and its live instance in `pluginId`, which is a key into PluginHost. The
    instance itself is never stored here: the audio thread may be running these
    and the model is edited on the message thread.
*/
struct Settings
{
    Type  type    = Type::none;
    float p1      = 0.5f;      // 0..1, meaning depends on the type
    float p2      = 0.45f;     // 0..1, feedback for delay / unused elsewhere
    bool  enabled = true;

    int    pluginId   = 0;                                    // PluginHost instance
    juce::String pluginName;                                  // for the rack label
    std::shared_ptr<const juce::PluginDescription> plugin;    // identity for save/load

    /** An insert only counts as active if its DSP can actually run. A VST3 that
        failed to load keeps its name but goes quiet instead of silently
        pretending to process. */
    bool active() const noexcept
    {
        return enabled && type != Type::none && (type != Type::vst || pluginId > 0);
    }
};

/** What the mixer rack prints in the insert's type cell. */
inline juce::String displayName (const Settings& s)
{
    if (s.type == Type::vst)
        return s.pluginName.isNotEmpty() ? s.pluginName : juce::String ("VST3");

    return typeName (s.type);
}

//==============================================================================
/** Per-slot DSP memory. Lives in the engine, never in the model: the message
    thread may change an insert while the audio thread is filtering with it. */
struct SlotState
{
    float lpL = 0.0f, lpR = 0.0f;
    float hpL = 0.0f, hpR = 0.0f;

    /** Delay memory. Allocated once in prepare(); the audio thread only ever
        indexes it. Sized for the longest delay the UI can ask for. */
    juce::AudioBuffer<float> delayBuffer;
    int writePos = 0;

    /** Phase 46 reverb: 12 delay lines — per channel 4 combs + 2 allpasses.
        Lengths live in verbLen (computed in prepare from the sample rate), the
        audio thread only walks positions. R channel lines are detuned ~1.3%
        against L so the stereo image opens. */
    static constexpr int verbLinesPerChannel = 6;
    juce::AudioBuffer<float> verbBuffer;
    std::array<int,  verbLinesPerChannel * 2> verbLen {};
    std::array<int,  verbLinesPerChannel * 2> verbPos {};
    std::array<float, verbLinesPerChannel * 2> verbDamp {};

    /** Phase 46 compressor: one linked-stereo envelope, one smoothed gain. */
    float compEnv  = 0.0f;
    float compGain = 1.0f;
};

//==============================================================================
struct ChainState
{
    static constexpr int maxSlots = 8;

    std::array<SlotState, maxSlots> slots;

    /** Called from the message thread (device start / export setup). */
    void prepare (double sampleRate)
    {
        const int maxDelaySamples = juce::jmax (1, (int) std::ceil (sampleRate * 0.22));
        // Reverb's longest comb, 30.75 ms, detuned 1.3% on the right channel.
        const int maxVerbSamples  = juce::jmax (1, (int) std::ceil (sampleRate * 0.032));

        // Left channel lines, then the same set 1.3% longer for the right.
        constexpr double lineSecs[SlotState::verbLinesPerChannel * 2] =
        {
            0.02531, 0.02694, 0.02896, 0.03075, 0.00477, 0.00731,   // L: 4 combs + 2 allpasses
            0.02531 * 1.013, 0.02694 * 1.013, 0.02896 * 1.013,
            0.03075 * 1.013, 0.00477 * 1.013, 0.00731 * 1.013       // R: detuned for stereo width
        };

        for (auto& slot : slots)
        {
            slot.delayBuffer.setSize (2, maxDelaySamples, false, true, false);
            slot.delayBuffer.clear();
            slot.writePos = 0;
            slot.lpL = slot.lpR = slot.hpL = slot.hpR = 0.0f;

            slot.verbBuffer.setSize (verbLines(), maxVerbSamples, false, true, false);
            slot.verbBuffer.clear();

            for (int i = 0; i < verbLines(); ++i)
            {
                slot.verbLen[(size_t) i]  = juce::jlimit (1, maxVerbSamples,
                                                          (int) std::ceil (lineSecs[i] * sampleRate));
                slot.verbPos[(size_t) i]  = 0;
                slot.verbDamp[(size_t) i] = 0.0f;
            }

            slot.compEnv  = 0.0f;
            slot.compGain = 1.0f;
        }
    }

    static constexpr int verbLines() noexcept { return SlotState::verbLinesPerChannel * 2; }

    void reset()
    {
        for (auto& slot : slots)
        {
            slot.delayBuffer.clear();
            slot.writePos = 0;
            slot.lpL = slot.lpR = slot.hpL = slot.hpR = 0.0f;

            slot.verbBuffer.clear();
            slot.verbPos.fill (0);
            slot.verbDamp.fill (0.0f);
            slot.compEnv  = 0.0f;
            slot.compGain = 1.0f;
        }
    }

    /** Audio thread. Applies every active built-in insert in order, in place.
        Hosted VST3 inserts are skipped here — they need the PluginHost, so the
        engine drives those itself, slot by slot, through processSlot(). */
    void process (const std::array<Settings, maxSlots>& settings,
                  juce::AudioBuffer<float>& buffer,
                  int numSamples,
                  double sampleRate)
    {
        for (int i = 0; i < maxSlots; ++i)
            processSlot (i, settings[(size_t) i], buffer, numSamples, sampleRate);
    }

    /** Audio thread. One slot, one effect, in place. Order is the caller's
        business: the engine walks slots 0..7 so a native effect and a hosted
        one interleave exactly as they are listed. */
    void processSlot (int index, const Settings& setting,
                      juce::AudioBuffer<float>& buffer,
                      int numSamples,
                      double sampleRate)
    {
        if (! setting.active() || setting.type == Type::vst)
            return;

        if (juce::jmin (2, buffer.getNumChannels()) <= 0 || numSamples <= 0)
            return;

        auto& state = slots[(size_t) juce::jlimit (0, maxSlots - 1, index)];

        switch (setting.type)
        {
            case Type::lowPass:  applyLowPass  (state, setting, buffer, numSamples, sampleRate); break;
            case Type::drive:    applyDrive    (state, setting, buffer, numSamples);             break;
            case Type::delay:    applyDelay    (state, setting, buffer, numSamples, sampleRate); break;
            case Type::air:      applyAir      (state, setting, buffer, numSamples, sampleRate); break;
            case Type::eq:       applyEq       (state, setting, buffer, numSamples, sampleRate); break;
            case Type::comp:     applyComp     (state, setting, buffer, numSamples, sampleRate); break;
            case Type::reverb:   applyReverb   (state, setting, buffer, numSamples);             break;
            case Type::vst:
            case Type::none:
            default:             break;
        }
    }

private:
    static void applyLowPass (SlotState& state, const Settings& s,
                              juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        // 120 Hz .. ~18 kHz, exponential so the knob feels musical at both ends.
        const float cutoff = 120.0f * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, s.p1) * 7.2f);
        const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                            * juce::jmin (cutoff, (float) sampleRate * 0.45f)
                                            / (float) sampleRate);

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            state.lpL += a * (l[n] - state.lpL);
            l[n] = state.lpL;

            if (r != nullptr)
            {
                state.lpR += a * (r[n] - state.lpR);
                r[n] = state.lpR;
            }
        }
    }

    static void applyDrive (SlotState& state, const Settings& s,
                            juce::AudioBuffer<float>& buffer, int numSamples)
    {
        juce::ignoreUnused (state);

        const float drive = 1.0f + juce::jlimit (0.0f, 1.0f, s.p1) * 11.0f;
        const float norm  = 1.0f / std::tanh (drive);

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int n = 0; n < numSamples; ++n)
                data[n] = std::tanh (data[n] * drive) * norm * 0.92f;
        }
    }

    static void applyDelay (SlotState& state, const Settings& s,
                            juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        const int maxDelay = juce::jmax (1, state.delayBuffer.getNumSamples() - 1);
        const int delay    = juce::jlimit (1, maxDelay,
                                           (int) ((0.035 + juce::jlimit (0.0f, 1.0f, s.p1) * 0.18)
                                                    * sampleRate));
        const float feedback = juce::jlimit (0.0f, 0.75f, s.p2 * 0.75f);
        const float mix      = 0.32f;

        const int channels = juce::jmin (2, buffer.getNumChannels(),
                                         state.delayBuffer.getNumChannels());

        for (int n = 0; n < numSamples; ++n)
        {
            const int readPos = (state.writePos + maxDelay + 1 - delay) % juce::jmax (1, maxDelay + 1);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                auto* line = state.delayBuffer.getWritePointer (ch);

                const float delayed = line[readPos];
                line[state.writePos] = data[n] + delayed * feedback;
                data[n] = data[n] * (1.0f - mix) + delayed * mix;
            }

            state.writePos = (state.writePos + 1) % juce::jmax (1, maxDelay + 1);
        }
    }

    static void applyAir (SlotState& state, const Settings& s,
                          juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * 2600.0f
                                            / (float) sampleRate);
        const float amount = 0.6f + juce::jlimit (0.0f, 1.0f, s.p1) * 2.4f;

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            state.hpL += a * (l[n] - state.hpL);
            l[n] += amount * (l[n] - state.hpL);

            if (r != nullptr)
            {
                state.hpR += a * (r[n] - state.hpR);
                r[n] += amount * (r[n] - state.hpR);
            }
        }
    }

    //==========================================================================
    //  Phase 46 — the mixing-chain trio: EQ, compressor, reverb.

    /** Tilt EQ. One knob, both ends: pulling p1 down leans the balance toward
        the lows, pushing it up leans toward the highs, centre is bit-flat. The
        pivot sits at 800 Hz; the one-pole split is cheap and the shape is
        musical, which matters more here than a 4-band state-variable notch. */
    static void applyEq (SlotState& state, const Settings& s,
                         juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        const float k = (juce::jlimit (0.0f, 1.0f, s.p1) - 0.5f);   // -0.5 .. +0.5
        const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * 800.0f
                                            / (float) sampleRate);

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            const float x = l[n];
            state.lpL += a * (x - state.lpL);
            l[n] = x + k * ((x - state.lpL) - state.lpL);   // + highs, - lows

            if (r != nullptr)
            {
                const float xr = r[n];
                state.lpR += a * (xr - state.lpR);
                r[n] = xr + k * ((xr - state.lpR) - state.lpR);
            }
        }
    }

    /** Compressor. p1 is one loudness knob: it lowers the threshold (-6 .. -26 dB)
        AND hardens the ratio (2:1 .. 8:1), because that is how a producer reaches
        for it. Linked-stereo envelope (max of both channels — never shift the
        image), peak envelope with 5 ms attack / 150 ms release, and a fixed
        gentle makeup so louder settings do not also end up quieter. */
    static void applyComp (SlotState& state, const Settings& s,
                           juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        const float p         = juce::jlimit (0.0f, 1.0f, s.p1);
        const float threshold = 0.5f * std::pow (0.1f, p);
        const float ratio     = 2.0f + p * 6.0f;
        const float makeup    = 1.0f + 0.35f * p;

        const float att = 1.0f - std::exp (-1.0f / (0.005f * (float) sampleRate));
        const float rel = 1.0f - std::exp (-1.0f / (0.150f * (float) sampleRate));

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            const float level = juce::jmax (std::abs (l[n]),
                                            r != nullptr ? std::abs (r[n]) : 0.0f);
            state.compEnv += (level - state.compEnv) * (level > state.compEnv ? att : rel);

            float target = 1.0f;
            if (state.compEnv > threshold && state.compEnv > 1.0e-6f)
                target = (threshold + (state.compEnv - threshold) / ratio) / state.compEnv;

            state.compGain += (target - state.compGain) * (target < state.compGain ? att : rel);

            const float g = state.compGain * makeup;
            l[n] *= g;
            if (r != nullptr) r[n] *= g;
        }
    }

    /** Reverb. Schroeder topology, four combs into two allpasses per channel:
        cheap, decays like a room, and never rings. p1 is room size — it maps
        to comb feedback, which is what the ear reads as "bigger". Wet level is
        fixed at a usable 30% blend; the allpass diffusion hides the comb
        periodicity that a bare comb bank would flutter with. */
    static void applyReverb (SlotState& state, const Settings& s,
                             juce::AudioBuffer<float>& buffer, int numSamples)
    {
        constexpr int lines = SlotState::verbLinesPerChannel;
        const float fb   = 0.70f + juce::jlimit (0.0f, 1.0f, s.p1) * 0.26f;   // 0.70 .. 0.96
        const float damp = 0.30f;
        const float dry  = 0.70f;
        const float wet  = 0.30f * 0.6f;   // four summed combs run hot; scale the bed

        const int channels = juce::jmin (2, buffer.getNumChannels(),
                                         state.verbBuffer.getNumChannels() / lines);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            const int base = ch * lines;

            for (int n = 0; n < numSamples; ++n)
            {
                const float in = data[n];
                float acc = 0.0f;

                for (int i = 0; i < 4; ++i)     // combs, damped feedback
                {
                    const int   len  = state.verbLen[(size_t) (base + i)];
                    auto*       line = state.verbBuffer.getWritePointer (base + i);
                    const int   pos  = state.verbPos[(size_t) (base + i)];
                    const float out  = line[pos];

                    auto& damped = state.verbDamp[(size_t) (base + i)];
                    damped = out * (1.0f - damp) + damped * damp;
                    line[pos] = in + damped * fb;
                    acc += out;
                    state.verbPos[(size_t) (base + i)] = (pos + 1) % len;
                }

                acc *= 0.25f;

                for (int i = 4; i < 6; ++i)     // allpasses, diffusion
                {
                    const int   len    = state.verbLen[(size_t) (base + i)];
                    auto*       line   = state.verbBuffer.getWritePointer (base + i);
                    const int   pos    = state.verbPos[(size_t) (base + i)];
                    const float bufout = line[pos];

                    line[pos] = acc + 0.5f * bufout;
                    acc = bufout - 0.5f * acc;
                    state.verbPos[(size_t) (base + i)] = (pos + 1) % len;
                }

                data[n] = in * dry + acc * wet;
            }
        }
    }
};

} // namespace fx
} // namespace atomic

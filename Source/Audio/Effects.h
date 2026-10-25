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
    vst         // a hosted VST3; the DSP lives in PluginHost, not here
};

inline int numTypes() noexcept { return 6; }

/** The built-in effects, for the "add insert" menu. `vst` is not in this list
    because it is chosen from the plugin submenu instead. */
inline int numBuiltInTypes() noexcept { return 5; }

inline juce::String typeName (Type type)
{
    switch (type)
    {
        case Type::lowPass: return "LOW PASS";
        case Type::drive:   return "DRIVE";
        case Type::delay:   return "DELAY";
        case Type::air:     return "AIR";
        case Type::vst:     return "VST3";
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

        for (auto& slot : slots)
        {
            slot.delayBuffer.setSize (2, maxDelaySamples, false, true, false);
            slot.delayBuffer.clear();
            slot.writePos = 0;
            slot.lpL = slot.lpR = slot.hpL = slot.hpR = 0.0f;
        }
    }

    void reset()
    {
        for (auto& slot : slots)
        {
            slot.delayBuffer.clear();
            slot.writePos = 0;
            slot.lpL = slot.lpR = slot.hpL = slot.hpR = 0.0f;
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
};

} // namespace fx
} // namespace atomic

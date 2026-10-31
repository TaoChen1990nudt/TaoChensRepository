#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "../Util/Text.h"

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
    eq,         // Phase 46: appended AFTER vst, never before it:
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

//==============================================================================
//  Phase 47 — the parameter contract. ONE table, used by the DSP, the editor
//  window, the rack mini-bar and the self-test. Values are stored normalised
//  0..1 in the model (the drag bars stay simple, old files keep loading); the
//  mapping to real units lives HERE, so what the editor prints is what the
//  DSP uses — the P43 lesson (one rule, one place) applied to parameters.

inline constexpr int maxParams = 9;

/** How many knobs this effect has. The editor window sizes itself from this;
    the rack mini-bar shows only params[0]. */
inline int numParams (Type type) noexcept
{
    switch (type)
    {
        case Type::lowPass:
        case Type::drive:
        case Type::air:     return 1;
        case Type::delay:   return 2;
        case Type::eq:      return 9;   // 3 bands x (freq, gain, Q)
        case Type::comp:    return 6;   // thr, ratio, makeup, attack, release, knee
        case Type::reverb:  return 5;   // decay, size, low cut, high cut, diffusion
        case Type::vst:
        case Type::none:
        default:            return 0;
    }
}

/** The value a knob starts at when the effect is mounted. 0.5 is neutral for
    every gain-style knob by construction of the maps below; delay's feedback
    keeps its historic 0.45 so a re-mounted delay sounds like it always did. */
inline float defaultParam (Type type, int index) noexcept
{
    if (type == Type::delay && index == 1)
        return 0.45f;

    return 0.5f;
}

//==============================================================================
//  Parameter maps. Each returns real units; the DSP and paramText() both call
//  these, so a display can never drift away from the sound.

// --- EQ (band 0 low shelf / 1 peaking / 2 high shelf) -----------------------
inline float eqFreq   (int band, float v) noexcept
{
    static constexpr float loFreqs[3] = { 20.0f, 200.0f, 1500.0f };
    static constexpr float hiFreqs[3] = { 600.0f, 5000.0f, 16000.0f };

    return loFreqs[(size_t) juce::jlimit (0, 2, band)]
         * std::pow (hiFreqs[(size_t) juce::jlimit (0, 2, band)]
                       / loFreqs[(size_t) juce::jlimit (0, 2, band)],
                     juce::jlimit (0.0f, 1.0f, v));
}

inline float eqGainDb (float v) noexcept { return -12.0f + 24.0f * juce::jlimit (0.0f, 1.0f, v); }
inline float eqQ      (float v) noexcept { return 0.3f * std::pow (8.0f / 0.3f, juce::jlimit (0.0f, 1.0f, v)); }

// --- COMP --------------------------------------------------------------------
inline float compThresholdDb (float v) noexcept { return -40.0f + 40.0f * juce::jlimit (0.0f, 1.0f, v); }
inline float compRatio       (float v) noexcept { return std::pow (20.0f, juce::jlimit (0.0f, 1.0f, v)); }
inline float compMakeupDb    (float v) noexcept { return 18.0f * juce::jlimit (0.0f, 1.0f, v); }
inline float compAttackSecs  (float v) noexcept { return 0.001f * std::pow (200.0f, juce::jlimit (0.0f, 1.0f, v)); }
inline float compReleaseSecs (float v) noexcept { return 0.020f * std::pow (100.0f, juce::jlimit (0.0f, 1.0f, v)); }
inline float compKneeDb      (float v) noexcept { return 24.0f * juce::jlimit (0.0f, 1.0f, v); }

// --- REVERB ------------------------------------------------------------------
inline float reverbDecaySecs (float v) noexcept { return 0.2f * std::pow (50.0f, juce::jlimit (0.0f, 1.0f, v)); }
inline float reverbSizeX     (float v) noexcept { return 0.2f + 1.6f * juce::jlimit (0.0f, 1.0f, v); }
inline float reverbLowcutHz  (float v) noexcept { return 20.0f * std::pow (40.0f, juce::jlimit (0.0f, 1.0f, v)); }
inline float reverbHighcutHz (float v) noexcept { return 2000.0f * std::pow (9.0f, juce::jlimit (0.0f, 1.0f, v)); }

/** Label for the rack mini-bar, which only ever shows params[0]. */
inline const char* paramName (Type type) noexcept
{
    switch (type)
    {
        case Type::lowPass: return "CUT";
        case Type::drive:   return "DRV";
        case Type::delay:   return "TIME";
        case Type::air:     return "AIR";
        case Type::vst:     return "VST";
        case Type::eq:      return "EQ";
        case Type::comp:    return "THR";
        case Type::reverb:  return "LEN";
        case Type::none:
        default:            return "—";
    }
}

/** Full name of knob `index` — what the editor window prints. The names are
    the user's own words for them, not ours. */
inline juce::String paramName (Type type, int index)
{
    switch (type)
    {
        case Type::lowPass:  return utf8 ("截止");
        case Type::drive:    return utf8 ("驱动");
        case Type::delay:    return index == 0 ? utf8 ("时间") : utf8 ("反馈");
        case Type::air:      return utf8 ("强度");

        case Type::eq:
        {
            static const char* names[9] =
            {
                "低频频率", "中频频率", "高频频率",
                "低频增益", "中频增益", "高频增益",
                "低频Q值", "中频Q值", "高频Q值"
            };
            return utf8 (names[(size_t) juce::jlimit (0, 8, index)]);
        }

        case Type::comp:
        {
            static const char* names[6] =
            {
                "阈值", "压缩比", "增益", "启动时间", "释放时间", "拐点软硬度"
            };
            return utf8 (names[(size_t) juce::jlimit (0, 5, index)]);
        }

        case Type::reverb:
        {
            static const char* names[5] =
            {
                "混响长度", "空间大小", "低切频率", "高切频率", "散射度"
            };
            return utf8 (names[(size_t) juce::jlimit (0, 4, index)]);
        }

        case Type::vst:
        case Type::none:
        default:             return "P" + juce::String (index + 1);
    }
}

/** The number the knob actually means — "0.73" says nothing about how bright
    a filter is. Uses the SAME maps the DSP uses, so the printed value and the
    sound can never drift apart. */
inline juce::String paramText (Type type, int index, float v)
{
    const float x = juce::jlimit (0.0f, 1.0f, v);

    auto hz = [] (float f)
    {
        return f >= 1000.0f ? juce::String (f / 1000.0f, 1) + " kHz"
                            : juce::String (juce::roundToInt (f)) + " Hz";
    };

    auto db = [] (float d)
    {
        return (d >= 0.0f ? "+" : "") + juce::String (d, 1) + " dB";
    };

    switch (type)
    {
        case Type::lowPass:  return hz (120.0f * std::pow (2.0f, x * 7.2f));
        case Type::drive:    return juce::String (1.0f + x * 11.0f, 1) + "x";
        case Type::delay:
            return index == 0 ? juce::String (juce::roundToInt (35.0f + x * 180.0f)) + " ms"
                              : juce::String (juce::roundToInt (x * 100.0f)) + "%";
        case Type::air:      return juce::String (0.6f + x * 2.4f, 1) + "x";

        case Type::eq:
            if (index < 3)  return hz (eqFreq (index, x));
            if (index < 6)  return db (eqGainDb (x));
            return juce::String (eqQ (x), 2);

        case Type::comp:
            switch (index)
            {
                case 0:  return db (compThresholdDb (x));
                case 1:  return juce::String (compRatio (x), 1) + " : 1";
                case 2:  return db (compMakeupDb (x));
                case 3:  return juce::String (compAttackSecs (x) * 1000.0f, 1) + " ms";
                case 4:  return juce::String (compReleaseSecs (x) * 1000.0f, 0) + " ms";
                default: return juce::String (juce::roundToInt (x * 100.0f)) + "%";
            }

        case Type::reverb:
            switch (index)
            {
                case 0:  return juce::String (reverbDecaySecs (x), 1) + " s";
                case 1:  return juce::String (reverbSizeX (x), 2) + "x";
                case 2:  return hz (reverbLowcutHz (x));
                case 3:  return hz (reverbHighcutHz (x));
                default: return juce::String (juce::roundToInt (x * 100.0f)) + "%";
            }

        case Type::vst:
        case Type::none:
        default:             return juce::String (juce::roundToInt (x * 100.0f)) + "%";
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
    bool  enabled = true;

    /** Phase 47: normalised 0..1 knob values, indexed by the tables above.
        Replaces the old p1/p2 pair — a tilt EQ with one value could not become
        the three-band EQ the users asked for. Old project files carried p1/p2;
        the reader maps them to params[0]/params[1]. */
    std::array<float, maxParams> params = []
    {
        std::array<float, maxParams> a {};
        a.fill (0.5f);
        return a;
    }();

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
/** Phase 100 — a detector signal borrowed from another strip.

    Only the compressor reads it, and only its LEVEL: the detector is an
    envelope follower, so what it wants is "how loud is the key this sample",
    not a second audio path to sum. Two pointers, no ownership, and the length
    is the block — the caller has already aligned it, which is the whole point
    of reading it out of the same delay line the console uses for sends.

    A null `left` means "nothing is patched in", which is NOT the same as a
    silent key: a silently-patched key holds the envelope at zero and therefore
    releases the compressor, while nothing patched in means "listen to the
    signal you are compressing". The two produce different sound on purpose. */
struct KeySignal
{
    const float* left  = nullptr;
    const float* right = nullptr;

    bool isLive() const noexcept { return left != nullptr; }
};

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
        against L so the stereo image opens. Phase 47: the SIZE knob multiplies
        a line's effective length, so the buffer is allocated for the largest
        size the map can produce (see prepare). */
    static constexpr int verbLinesPerChannel = 6;
    juce::AudioBuffer<float> verbBuffer;
    std::array<int,  verbLinesPerChannel * 2> verbLen {};
    std::array<int,  verbLinesPerChannel * 2> verbPos {};
    std::array<float, verbLinesPerChannel * 2> verbDamp {};

    /** Phase 47 reverb: one-pole states for the wet-path low/high cut. */
    float verbCutLpL = 0.0f, verbCutLpR = 0.0f;    // lowpass, for the HIGH cut
    float verbCutHpL = 0.0f, verbCutHpR = 0.0f;    // lowpass behind the LOW cut

    /** Phase 47 EQ: three RBJ biquads (low shelf / peaking / high shelf),
        z1/z2 per band per channel. Coefficients are recomputed per block. */
    std::array<std::array<float, 2>, 3> eqZ1 {};
    std::array<std::array<float, 2>, 3> eqZ2 {};

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
        // Reverb's longest comb, 30.75 ms, detuned 1.3% on the right channel,
        // stretched by the largest SIZE the map can produce (1.8x), plus margin.
        const int maxVerbSamples  = juce::jmax (1, (int) std::ceil (sampleRate * 0.032 * 1.85));

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

            slot.verbCutLpL = slot.verbCutLpR = 0.0f;
            slot.verbCutHpL = slot.verbCutHpR = 0.0f;

            for (int b = 0; b < 3; ++b)
            {
                slot.eqZ1[(size_t) b].fill (0.0f);
                slot.eqZ2[(size_t) b].fill (0.0f);
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
            slot.verbCutLpL = slot.verbCutLpR = 0.0f;
            slot.verbCutHpL = slot.verbCutHpR = 0.0f;

            for (int b = 0; b < 3; ++b)
            {
                slot.eqZ1[(size_t) b].fill (0.0f);
                slot.eqZ2[(size_t) b].fill (0.0f);
            }

            slot.compEnv  = 0.0f;
            slot.compGain = 1.0f;
        }
    }

    /** Audio thread. Applies every active built-in insert in order, in place.
        Hosted VST3 inserts are skipped here — they need the PluginHost, so the
        engine drives those itself, slot by slot, through processSlot().

        Phase 100: `key` is the whole chain's sidechain signal, or null. It is
        one per CHAIN, not one per slot, because the key lives on the strip
        (MixerModel::getSidechainSource) and a strip has one detector bus —
        "the compressor listens to channel 1" is a fact about the strip, and
        two compressors on it cannot sensibly listen to two different donors. */
    void process (const std::array<Settings, maxSlots>& settings,
                  juce::AudioBuffer<float>& buffer,
                  int numSamples,
                  double sampleRate,
                  const KeySignal* key = nullptr)
    {
        for (int i = 0; i < maxSlots; ++i)
            processSlot (i, settings[(size_t) i], buffer, numSamples, sampleRate, key);
    }

    /** Audio thread. One slot, one effect, in place. Order is the caller's
        business: the engine walks slots 0..7 so a native effect and a hosted
        one interleave exactly as they are listed. */
    void processSlot (int index, const Settings& setting,
                      juce::AudioBuffer<float>& buffer,
                      int numSamples,
                      double sampleRate,
                      const KeySignal* key = nullptr)
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
            case Type::comp:     applyComp     (state, setting, buffer, numSamples, sampleRate, key); break;
            case Type::reverb:   applyReverb   (state, setting, buffer, numSamples, sampleRate); break;
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
        const float cutoff = 120.0f * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, s.params[(size_t) 0]) * 7.2f);
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

        const float drive = 1.0f + juce::jlimit (0.0f, 1.0f, s.params[(size_t) 0]) * 11.0f;
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
                                           (int) ((0.035 + juce::jlimit (0.0f, 1.0f, s.params[(size_t) 0]) * 0.18)
                                                    * sampleRate));
        const float feedback = juce::jlimit (0.0f, 0.75f, s.params[(size_t) 1] * 0.75f);
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
        const float amount = 0.6f + juce::jlimit (0.0f, 1.0f, s.params[(size_t) 0]) * 2.4f;

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
    //  Phase 46/47 — the mixing chain. EQ grew from a one-knob tilt into the
    //  three-band the users asked for; comp and reverb grew real knobs too.
    //  Every knob maps through the functions at the top of this file — the
    //  editor's printed number and the DSP's number are the same number.

    struct Coeffs
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
    };

    /** RBJ biquad coefficients. `band`: 0 = low shelf, 1 = peaking, 2 = high
        shelf. A 0 dB shelf would still be an allpass, so the caller skips
        bands whose gain is flat — transparency where nothing was asked. */
    static Coeffs rbjCoeffs (int band, float freq, float q, float gainDb, double sampleRate)
    {
        const float A     = std::pow (10.0f, gainDb / 40.0f);
        const float w0    = 2.0f * juce::MathConstants<float>::pi
                              * juce::jlimit (10.0f, (float) sampleRate * 0.45f, freq)
                              / (float) sampleRate;
        const float cw    = std::cos (w0);
        const float sw    = std::sin (w0);
        const float alpha = sw / (2.0f * juce::jmax (0.05f, q));
        const float sqA   = 2.0f * std::sqrt (A) * alpha;   // the shelf term

        Coeffs c;

        if (band == 1)          // peaking
        {
            c.b0 = 1.0f + alpha * A;
            c.b1 = -2.0f * cw;
            c.b2 = 1.0f - alpha * A;
            c.a1 = -2.0f * cw;
            c.a2 = 1.0f - alpha / A;
        }
        else if (band == 0)     // low shelf
        {
            c.b0 =        A * ((A + 1.0f) - (A - 1.0f) * cw + sqA);
            c.b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
            c.b2 =        A * ((A + 1.0f) - (A - 1.0f) * cw - sqA);
            c.a1 =   -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
            c.a2 =        ((A + 1.0f) + (A - 1.0f) * cw - sqA);
        }
        else                    // high shelf
        {
            c.b0 =        A * ((A + 1.0f) + (A - 1.0f) * cw + sqA);
            c.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
            c.b2 =        A * ((A + 1.0f) + (A - 1.0f) * cw - sqA);
            c.a1 =    2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
            c.a2 =        ((A + 1.0f) - (A - 1.0f) * cw - sqA);
        }

        // Normalise. Each shelf's a0 differs, so each is computed explicitly —
        // one shared "a0" expression across the three topologies was how a
        // wrong shelf used to hide.
        if (band == 0)
        {
            const float a0L = (A + 1.0f) + (A - 1.0f) * cw + sqA;
            c.b0 /= a0L; c.b1 /= a0L; c.b2 /= a0L; c.a1 /= a0L; c.a2 /= a0L;
        }
        else if (band == 1)
        {
            const float a0P = 1.0f + alpha / A;
            c.b0 /= a0P; c.b1 /= a0P; c.b2 /= a0P; c.a1 /= a0P; c.a2 /= a0P;
        }
        else
        {
            const float a0H = (A + 1.0f) - (A - 1.0f) * cw + sqA;
            c.b0 /= a0H; c.b1 /= a0H; c.b2 /= a0H; c.a1 /= a0H; c.a2 /= a0H;
        }

        return c;
    }

    /** Three-band EQ: low shelf, peaking, high shelf, in series. Params:
        0/1/2 = band frequencies, 3/4/5 = band gains, 6/7/8 = band Qs. A band
        whose gain sits at 0 dB is bypassed entirely — untouched bands must be
        bit-transparent, or "just a low shelf" would still smear the highs. */
    static void applyEq (SlotState& state, const Settings& s,
                         juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        Coeffs coeffs[3];
        bool   used[3] = { false, false, false };

        for (int band = 0; band < 3; ++band)
        {
            const float gainDb = eqGainDb (s.params[(size_t) (3 + band)]);

            if (std::abs (gainDb) > 0.01f)
            {
                coeffs[band] = rbjCoeffs (band,
                                          eqFreq (band, s.params[(size_t) band]),
                                          eqQ (s.params[(size_t) (6 + band)]),
                                          gainDb, sampleRate);
                used[band] = true;
            }
        }

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            float x[2] = { l[n], r != nullptr ? r[n] : 0.0f };

            for (int band = 0; band < 3; ++band)
            {
                if (! used[band])
                    continue;

                const auto& c = coeffs[band];

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto& z1 = state.eqZ1[(size_t) band][(size_t) ch];
                    auto& z2 = state.eqZ2[(size_t) band][(size_t) ch];

                    const float y = c.b0 * x[ch] + z1;
                    z1 = c.b1 * x[ch] - c.a1 * y + z2;
                    z2 = c.b2 * x[ch] - c.a2 * y;
                    x[ch] = y;
                }
            }

            l[n] = x[0];
            if (r != nullptr) r[n] = x[1];
        }
    }

    /** Compressor. Params: 0 threshold (-40..0 dB), 1 ratio (1..20), 2 makeup
        (0..+18 dB), 3 attack (1..200 ms), 4 release (20..2000 ms), 5 knee
        width (0..24 dB, the soft-knee quadratic from Reiss & McPherson).
        Linked-stereo envelope (max of both channels — never shift the image).

        Phase 100: `key` replaces the DETECTOR's input, not the signal path. The
        audio being compressed is still `buffer`; only "how loud is it right now"
        comes from elsewhere. That is the whole of what a sidechain is, and
        keeping it at this one line means the gain computer, the knee, the
        attack/release and the makeup are shared by both modes instead of
        duplicated into a second function that would drift. */
    static void applyComp (SlotState& state, const Settings& s,
                           juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate,
                           const KeySignal* key = nullptr)
    {
        const float thresholdDb = compThresholdDb (s.params[(size_t) 0]);
        const float ratio       = compRatio (s.params[(size_t) 1]);
        const float makeup      = std::pow (10.0f, compMakeupDb (s.params[(size_t) 2]) / 20.0f);
        const float kneeDb      = compKneeDb (s.params[(size_t) 5]);

        const float att = 1.0f - std::exp (-1.0f / (compAttackSecs (s.params[(size_t) 3]) * (float) sampleRate));
        const float rel = 1.0f - std::exp (-1.0f / (compReleaseSecs (s.params[(size_t) 4]) * (float) sampleRate));

        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        // Resolved once per block, not per sample: the pointer test inside the
        // loop would be the same answer a thousand times.
        const bool  keyed  = key != nullptr && key->isLive();
        const float* keyL  = keyed ? key->left  : nullptr;
        const float* keyR  = keyed ? key->right : nullptr;

        for (int n = 0; n < numSamples; ++n)
        {
            const float level = keyed
                                  ? juce::jmax (std::abs (keyL[n]),
                                                keyR != nullptr ? std::abs (keyR[n]) : 0.0f)
                                  : juce::jmax (std::abs (l[n]),
                                                r != nullptr ? std::abs (r[n]) : 0.0f);
            state.compEnv += (level - state.compEnv) * (level > state.compEnv ? att : rel);

            float target = 1.0f;

            if (state.compEnv > 1.0e-6f)
            {
                const float envDb   = 20.0f * std::log10 (state.compEnv);
                const float over    = envDb - thresholdDb;
                const float halfW   = kneeDb * 0.5f;
                float       outDb   = envDb;

                if (2.0f * over < -kneeDb)
                    outDb = envDb;                                   // below the knee: untouched
                else if (kneeDb > 0.01f && 2.0f * std::abs (over) <= kneeDb)   // inside the knee
                    outDb = envDb + (1.0f / ratio - 1.0f)
                                  * (over + halfW) * (over + halfW) / (2.0f * kneeDb);
                else if (over > 0.0f)
                    outDb = thresholdDb + over / ratio;              // above: full compression

                target = std::pow (10.0f, (outDb - envDb) / 20.0f);
            }

            state.compGain += (target - state.compGain) * (target < state.compGain ? att : rel);

            const float g = state.compGain * makeup;
            l[n] *= g;
            if (r != nullptr) r[n] *= g;
        }
    }

    /** Reverb. Schroeder topology, four combs into two allpasses per channel.
        Params: 0 decay (0.2..10 s, drives comb feedback RT60-style), 1 size
        (0.2..1.8x, multiplies each line's effective length), 2 low cut
        (20..800 Hz), 3 high cut (2..18 kHz), 4 diffusion (allpass coefficient).
        Wet level is fixed at a usable 30% blend; the R channel's lines stay
        1.3% detuned so the image opens. */
    static void applyReverb (SlotState& state, const Settings& s,
                             juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate)
    {
        constexpr int lines = SlotState::verbLinesPerChannel;

        const float decaySecs = reverbDecaySecs (s.params[(size_t) 0]);
        const float sizeX     = reverbSizeX (s.params[(size_t) 1]);
        const float lowcutHz  = reverbLowcutHz (s.params[(size_t) 2]);
        const float highcutHz = reverbHighcutHz (s.params[(size_t) 3]);
        const float diffusion = juce::jlimit (0.0f, 1.0f, s.params[(size_t) 4]);

        const float apG   = 0.25f + 0.45f * diffusion;   // allpass coefficient
        const float damp  = 0.30f;
        const float dry   = 0.70f;
        const float wet   = 0.30f * 0.6f;   // four summed combs run hot; scale the bed
        const float sr    = (float) sampleRate;

        // Wet-path cut filters, one-pole, recomputed per block.
        const float aLp = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                              * juce::jmin (highcutHz, sr * 0.45f) / sr);
        const float aLc = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                              * juce::jmin (lowcutHz, sr * 0.45f) / sr);

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

                for (int i = 0; i < 4; ++i)     // combs, damped feedback, size-scaled
                {
                    const int   len = juce::jlimit (1, state.verbBuffer.getNumSamples(),
                                                    (int) (state.verbLen[(size_t) (base + i)] * sizeX));
                    auto*       line = state.verbBuffer.getWritePointer (base + i);
                    const int   pos  = state.verbPos[(size_t) (base + i)];
                    const float out  = line[pos];

                    // RT60-style feedback: a line of `len` samples repeats
                    // len/sr times per second, so g = 10^(-3 len / (T sr)).
                    const float fb = juce::jlimit (0.0f, 0.98f,
                        std::pow (10.0f, -3.0f * (float) len / (juce::jmax (0.05f, decaySecs) * sr)));

                    auto& damped = state.verbDamp[(size_t) (base + i)];
                    damped = out * (1.0f - damp) + damped * damp;
                    line[pos] = in + damped * fb;
                    acc += out;
                    state.verbPos[(size_t) (base + i)] = (pos + 1) % len;
                }

                acc *= 0.25f;

                for (int i = 4; i < 6; ++i)     // allpasses, diffusion-controlled
                {
                    const int   len    = juce::jlimit (1, state.verbBuffer.getNumSamples(),
                                                       (int) (state.verbLen[(size_t) (base + i)] * sizeX));
                    auto*       line   = state.verbBuffer.getWritePointer (base + i);
                    const int   pos    = state.verbPos[(size_t) (base + i)];
                    const float bufout = line[pos];

                    line[pos] = acc + apG * bufout;
                    acc = bufout - apG * acc;
                    state.verbPos[(size_t) (base + i)] = (pos + 1) % len;
                }

                // Low cut (highpass) then high cut (lowpass) on the wet bed.
                auto& hpl  = (ch == 0) ? state.verbCutHpL : state.verbCutHpR;
                auto& lpHi = (ch == 0) ? state.verbCutLpL : state.verbCutLpR;

                hpl  += aLc * (acc  - hpl);
                acc   = acc - hpl;              // remove what the lowpass holds

                lpHi += aLp * (acc  - lpHi);
                acc   = lpHi;                   // keep only what the lowpass holds

                data[n] = in * dry + acc * wet;
            }
        }
    }
};

} // namespace fx
} // namespace atomic

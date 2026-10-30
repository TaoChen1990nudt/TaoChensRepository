#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

//==============================================================================
/** Phase 93 — how full the audio thread's block is, and how close it came.

    JUCE already measures this: `AudioDeviceManager::getCpuUsage()` is an
    exponential average of `timeInCallback / blockBudget`, and
    `getXRunCount()` is the number of blocks that missed the deadline (the
    device's own count where the driver reports one, JUCE's estimate
    otherwise). Both are the right numbers and neither is reimplemented here.

    What JUCE throws away is the one thing that answers "was that click me or
    the machine":

      * it **clamps** the load to 1.0, so a block that took 250% of its budget
        reads as "100%" and looks exactly like a block that took exactly 100%;
      * it keeps **no peak**, so by the time the user looks at the meter the
        spike is gone.

    So: the average comes from JUCE, the peak comes from here, and the two are
    the same quantity measured the same way — the load is smoothed with the
    same 0.2 coefficient so the two meters move in step and a user cannot see
    them disagree.

    RT-safe: `pushBlock` is arithmetic on eight doubles and a counter. One
    writer (the audio thread), any number of readers.
*/
namespace perf
{

/** Per-block smoothing, deliberately the same 0.2 JUCE's
    AudioProcessLoadMeasurer uses. Two meters on one screen that are the same
    measurement must not drift apart. */
inline constexpr double smoothing = 0.2;

/** How long a peak stays put before it is allowed to fall, in seconds of
    AUDIO (not of wall clock — a stopped transport processes no audio, and a
    peak that decayed while the transport was parked would be gone before the
    user pressed play again). */
inline constexpr double peakHoldSeconds = 1.5;

/** How fast the peak falls once the hold is over: 0.5 means a fully-lit peak
    takes two seconds to reach the floor. Slow enough to read, fast enough that
    a single glitch in a long session is not permanent decoration. */
inline constexpr double peakFallPerSecond = 0.5;

/** The numbers the rule owns. `load` and `peak` are proportions of the block
    budget: 1.0 is "the callback took exactly as long as the block lasts".

    `peak` can exceed 1.0 on purpose — that is the measurement JUCE discards. */
struct Numbers
{
    double load  = 0.0;
    double peak  = 0.0;
    double holdLeft = 0.0;
    int    blocks = 0;
};

/** One block's worth of accounting. Pure: same inputs, same outputs, no
    state outside the argument.

    `budgetMs <= 0` means the engine does not know how long a block is (no
    device open) — the state is then left alone rather than divided by zero.
    A negative `elapsedMs` can only come from a clock that went backwards; it
    is dropped for the same reason. */
inline Numbers advance (Numbers n, double elapsedMs, double budgetMs) noexcept
{
    if (! (budgetMs > 0.0) || ! (elapsedMs >= 0.0))
        return n;

    const double use = elapsedMs / budgetMs;
    const double audioSeconds = budgetMs * 0.001;

    n.blocks += 1;
    n.load += smoothing * (use - n.load);

    if (use >= n.peak)
    {
        // A new high: hold it long enough to be read.
        n.peak     = use;
        n.holdLeft = peakHoldSeconds;
    }
    else if (n.holdLeft > 0.0)
    {
        n.holdLeft = juce::jmax (0.0, n.holdLeft - audioSeconds);
    }
    else
    {
        // Never below what the block we just measured actually used: the peak
        // is an upper bound, and an upper bound under the current value is a
        // lie the meter would tell every time the load rose slowly.
        n.peak = juce::jmax (use, n.peak - peakFallPerSecond * audioSeconds);
    }

    return n;
}

} // namespace perf

//==============================================================================
namespace atomic
{

/** The published side of the numbers: the audio thread writes, the message
    thread reads, nobody locks. */
class LoadMeter
{
public:
    /** Audio thread. `budgetMs` is `numSamples * 1000 / sampleRate`. */
    void pushBlock (double elapsedMs, double budgetMs) noexcept
    {
        state = perf::advance (state, elapsedMs, budgetMs);

        loadPublished.store  ((float) state.load,  std::memory_order_relaxed);
        peakPublished.store  ((float) state.peak,  std::memory_order_relaxed);
        blocksPublished.store (state.blocks,       std::memory_order_relaxed);
    }

    /** Message thread, with the device stopped (or not yet started): `state` is
        plain doubles written by the audio thread, so clearing it while that
        thread is inside `pushBlock` would be a race, not a reset. Called from
        `audioDeviceAboutToStart` and `stop()` — both are outside the callback. */
    void reset() noexcept
    {
        state = {};
        loadPublished.store  (0.0f);
        peakPublished.store  (0.0f);
        blocksPublished.store (0);
    }

    float getLoad()   const noexcept { return loadPublished.load  (std::memory_order_relaxed); }
    float getPeak()   const noexcept { return peakPublished.load  (std::memory_order_relaxed); }
    int   getBlocks() const noexcept { return blocksPublished.load (std::memory_order_relaxed); }

private:
    /** Only the audio thread touches this. The three atomics above are its
        published face. */
    perf::Numbers state;

    std::atomic<float> loadPublished { 0.0f };
    std::atomic<float> peakPublished { 0.0f };
    std::atomic<int>   blocksPublished { 0 };
};

} // namespace atomic

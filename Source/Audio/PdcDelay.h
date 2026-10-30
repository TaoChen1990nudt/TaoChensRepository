#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

//==============================================================================
/** Phase 83 — plugin delay compensation: the arithmetic, and the memory.

    A console has exactly one kind of parallel join: several signals land on the
    same bus at the same instant. Until this phase every path through the
    console had the same latency (zero), so "the same instant" was free. A
    hosted VST3 insert breaks that: a lookahead limiter on the drum bus and
    nothing on the vocal makes the drums late by its lookahead, and no fader
    can put them back. The fix is to hold every *other* path back by the same
    amount, at the point where the paths meet — never at the point where they
    start, because a signal that is delayed before its own channel's inserts is
    delayed in front of work that has not happened yet.

    Everything here is RT-safe once prepared: `Line::prepare` is the only
    allocation, and it is called from the message thread.
*/
namespace pdc
{

/** How far a signal may be held back to meet a slower sibling.

    2048 samples is 42 ms at 48 kHz — an order of magnitude more than anything
    that reports latency through VST3 (a lookahead limiter is 1-5 ms, a
    linear-phase EQ tens of samples). It is also the memory budget: the console
    can address 128 strips, so an unbounded number here is an unbounded array
    of delay lines. A plugin asking for more is clamped, and that clamp is the
    one thing about this design a user could notice. */
constexpr int maxSamples = 2048;

/** One render window, i.e. the longest block the engine will ever hand over
    (PluginHost::maxBlockSize, used for offline windows too).

    The ring must cover the delay AND the block written into it in the same
    call, or a read at `maxSamples` would lap the write head before the write
    of this block finished. */
constexpr int window = 4096;

/** Power of two, so the wrap is a mask instead of a modulo. */
constexpr int ringSize = 8192;

static_assert (ringSize >= maxSamples + window,
               "a read at the maximum delay must not lap this block's writes");
static_assert ((ringSize & (ringSize - 1)) == 0, "ringSize must be a power of two");

//==============================================================================
/** One bus's delay memory: two channels, a write head, and nothing else.

    `push` records this block; `readInto` reads it back `delaySamples` late.
    Push and read are separate calls because the two are not always adjacent:
    the console pushes a finished strip and reads it at each of the join points
    it feeds, and it may feed two of them.
*/
struct Line
{
    /** Message thread. Idempotent: a line is bought once and then reused, so a
        console that grows does not re-buy the strips it already had. */
    void prepare()
    {
        if (size > 0)
            return;

        for (auto& channel : channels)
            channel.assign ((size_t) ringSize, 0.0f);

        writePos = 0;
        size     = ringSize;
    }

    /** Message thread (device start) and audio thread (panic / locate).

        Silencing the ring matters after a locate: 42 ms of the previous
        position is not a tail, it is the wrong music. */
    void reset() noexcept
    {
        for (auto& channel : channels)
            std::fill (channel.begin(), channel.end(), 0.0f);

        writePos = 0;
    }

    bool isReady() const noexcept { return size > 0; }

    /** Audio thread. `source` may be mono; channel 1 then mirrors channel 0,
        which is what the console does with a mono strip everywhere else. */
    void push (const juce::AudioBuffer<float>& source, int numSamples) noexcept
    {
        if (size <= 0)
            return;

        const int count = juce::jmin (numSamples, window);
        const int mask  = size - 1;

        for (int ch = 0; ch < 2; ++ch)
        {
            const int sourceChannel = juce::jmin (ch, source.getNumChannels() - 1);

            if (sourceChannel < 0)
                continue;

            auto& ring = channels[(size_t) ch];
            const auto* in = source.getReadPointer (sourceChannel);

            for (int i = 0; i < count; ++i)
                ring[(size_t) ((writePos + i) & mask)] = in[i];
        }

        writePos = (writePos + count) & (size - 1);
    }

    /** Audio thread. Reads the block that was pushed `delaySamples` ago.

        `add == false` replaces the destination (used for the input alignment,
        which happens in place); `add == true` sums it into a bus that already
        holds the faster siblings. */
    void readInto (juce::AudioBuffer<float>& destination, int delaySamples,
                   int numSamples, bool add) const noexcept
    {
        if (size <= 0)
            return;

        const int count = juce::jmin (numSamples, window);
        const int delay = juce::jlimit (0, maxSamples, delaySamples);
        const int mask  = size - 1;

        // The newest sample written is at writePos-1, so the block that ended
        // `delay` samples ago started at writePos - count - delay.
        int start = (writePos - count - delay) % size;

        if (start < 0)
            start += size;

        for (int ch = 0; ch < destination.getNumChannels(); ++ch)
        {
            // The same fold the console already uses when a stereo bus meets a
            // wider device: channel 0 to the left, everything else to the right.
            const auto& ring = channels[(size_t) juce::jmin (ch, 1)];
            auto* out = destination.getWritePointer (ch);

            for (int i = 0; i < count; ++i)
            {
                const float value = ring[(size_t) ((start + i) & mask)];

                if (add)
                    out[i] += value;
                else
                    out[i] = value;
            }
        }
    }

    std::array<std::vector<float>, 2> channels;
    int size     = 0;
    int writePos = 0;
};

//==============================================================================
/** The whole latency rule, as a pure function — `SelfTest.cpp` states it.

    - `order`    walks the console sources-first (the order the renderer already
                 sums in), so a channel is always solved after everything that
                 feeds it.
    - `own[i]`   is channel i's insert latency, in samples.
    - `output[i]` is the strip it sums into; anything outside 0..count-1 means
                 "straight to the master", i.e. a root.
    - `entry[i]` comes back as the latency of everything arriving at channel i's
                 input: 0 when nothing feeds it, otherwise the slowest child's
                 end. This is also the instant that channel's own voices have to
                 be held back to, because a direct voice arrives at 0.
    - `slide[i]` comes back as how far channel i's *finished* signal is held
                 back on its way to the MASTER (roots). A channel that feeds
                 another strip does not use this number: the delay there is
                 `entry[destination] - (entry[i] + own[i])`, one subtraction the
                 renderer makes when it knows which bus it is summing into —
                 keeping it out of this table is what stops two answers to the
                 same question existing.

    Returns the console's total latency: how late the slowest path is, and
    therefore how late the mix is. Nothing compensates for that (it is the same
    for every path); it is only worth knowing.

    Degenerate case, and the reason this can be shipped at all: no insert
    reports latency ⟹ every `entry` and `slide` is 0 ⟹ the renderer takes the
    exact code path it took before this phase existed.
*/
inline int solve (int count, const int* order, const int* output, const int* own,
                  int* entry, int* slide) noexcept
{
    if (count <= 0)
        return 0;

    for (int i = 0; i < count; ++i)
        entry[i] = 0;

    for (int step = 0; step < count; ++step)
    {
        const int channel = order[step];

        if (! juce::isPositiveAndBelow (channel, count))
            continue;

        int pre = 0;

        for (int other = 0; other < count; ++other)
            if (other != channel && output[other] == channel)
                pre = juce::jmax (pre, entry[other] + own[other]);

        entry[channel] = juce::jmin (pre, maxSamples);
    }

    int total = 0;

    for (int i = 0; i < count; ++i)
        total = juce::jmax (total, entry[i] + own[i]);

    total = juce::jmin (total, maxSamples);

    for (int i = 0; i < count; ++i)
        slide[i] = juce::jmax (0, total - (entry[i] + own[i]));

    return total;
}

} // namespace pdc

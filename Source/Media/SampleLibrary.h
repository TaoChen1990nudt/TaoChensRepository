#pragma once

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace atomic
{

//==============================================================================
/**
    Phase 17: one decoded audio file, frozen.

    Deliberately immutable and shared by `shared_ptr<const SampleBuffer>`. The
    audio thread reads the frames while the user keeps browsing files, so a
    buffer that could grow or be reallocated mid-render would be the same class
    of bug as letting the renderer read a Layer. Load it once, never touch it
    again.

    Stored interleaved because that is what the WAV file gives us and what the
    render loop wants: a clip pulls frame i from both channels at the same time,
    so planar storage would double the cache misses for nothing.

    The peak envelope is computed at load, not at paint: a 3-minute file has
    eight million frames, and the roll asks for its waveform on every repaint.
*/
struct SampleBuffer
{
    /** Buckets in the cached peak envelope. 1024 is finer than any clip will
        ever be drawn on screen, so the waveform never looks blocky, and it is
        small enough to scan per frame while scrolling. */
    static constexpr int peakBuckets = 1024;

    double sampleRate = 44100.0;
    int    channels   = 1;
    std::vector<float> frames;          // interleaved, frames.size() == numFrames * channels

    /** 0..1 per bucket. Empty only for an empty file. */
    std::vector<float> peaks;

    int numFrames() const noexcept
    {
        return channels > 0 ? (int) (frames.size() / (size_t) channels) : 0;
    }

    double lengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? (double) numFrames() / sampleRate : 0.0;
    }

    float peakLevel() const noexcept;
};

//==============================================================================
/** Result of a load attempt. `buffer` is null exactly when `error` is not
    empty — a caller can test one or the other and never gets a half-loaded
    sample. */
struct LoadedSample
{
    std::shared_ptr<const SampleBuffer> buffer;
    juce::String error;

    bool ok() const noexcept { return buffer != nullptr; }
};

/** Reads a WAV file into memory.

    Hand-rolled rather than routed through juce_audio_formats for the same
    reason the exporter writes its own 44-byte header: the format module drags
    in a codec registry, platform reader backends and an audio-source layer that
    this app has no other use for. A DAW that plays one file format should say
    so plainly instead of shipping a decoder wall.

    Supports RIFF/WAVE with PCM 8/16/24/32-bit and IEEE float 32/64-bit, any
    channel count (downmixed to stereo), and skips unknown chunks. */
LoadedSample loadSampleFile (const juce::File& file);

/** Extensions the browser offers. One list, so the panel, the drop handler and
    the self-test agree on what "a sample" is. */
bool isSampleFile (const juce::File& file);

//==============================================================================
/** Writes a small, deterministic WAV.

    Exists so the demo fixture and the self-test have a real file to decode
    without shipping binary assets: the test writes it to the temp folder, loads
    it back through the same code the user's drag-and-drop hits, and asserts on
    the frame count. `shape` renders a decaying tone so the clip's waveform
    thumbnail has something to draw.
*/
bool writeTestSampleFile (const juce::File& file,
                          double seconds,
                          double sampleRate = 44100.0,
                          int channels = 2,
                          double frequency = 220.0);

} // namespace atomic

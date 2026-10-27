#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>

#include "Audio/PlaybackEngine.h"

namespace atomic
{

//==============================================================================
/**
    Offline bounce to a standard 16-bit stereo WAV.

    The render is a straight walk over the published schedule, block by block,
    using the same PlaybackEngine code the sound card uses. That is deliberate:
    "what you hear" and "what you export" cannot drift apart, because there is
    only one mixer.

    Two details that matter more than they look:

    - **Block-by-block, not one giant block.** Automation is evaluated at the
      start and end of each block and ramped between; rendering a four-minute
      song as one block would evaluate each curve twice and interpolate over
      minutes. The synth filter also carries state between blocks, so the sweep
      stays continuous.

    - **Own engine instance.** The caller passes the schedule and this function
      builds a private PlaybackEngine for it, so the export's filter memory is
      independent of whatever the live transport was doing.
*/
namespace wav
{

/** 44.1 kHz, the rate the project is mixed and auditioned at. */
inline constexpr double exportSampleRate = 44100.0;

struct ExportResult
{
    bool   ok          = false;
    int    numSamples  = 0;
    double seconds     = 0.0;
    double sampleRate  = 0.0;
    int    numChannels = 0;
    float  peak        = 0.0f;      // before the ceiling guard, 0..1+
    float  rms         = 0.0f;
    bool   peakLimited = false;     // the ceiling guard had to pull the mix down
    juce::String message;
};

/**
    Renders `schedule` from time 0 to its content end (plus `tailSeconds`) and
    writes it to `destination`.

    `progress` is called with 0..1 after every block; returning false cancels
    the export and deletes the partial file. It is called from the rendering
    thread, so it must not touch the UI directly.

    Phase 50: `startSeconds` moves the whole bounce later in the timeline. Used
    by "render this group to a block", where the group may start at beat 40 and
    the block must contain the group, not forty beats of silence. Nothing is
    rebased — see scope::extract for why.
*/
ExportResult renderToFile (const juce::File& destination,
                           const PublishedSchedule& schedule,
                           double sampleRate = exportSampleRate,
                           double tailSeconds = 0.05,
                           const std::function<bool (double)>& progress = {},
                           PluginHost* livePlugins = nullptr,
                           double startSeconds = 0.0);

} // namespace wav

} // namespace atomic

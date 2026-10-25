#include "WavExport.h"

#include "PluginHost.h"

#include <cmath>

namespace atomic
{

namespace wav
{

namespace
{
    /** Writes canonical 44-byte-header PCM. juce_audio_formats would do this
        too, but one format does not justify pulling in the whole module. */
    bool writePcm16 (const juce::File& file, const juce::AudioBuffer<float>& buffer,
                     double sampleRate, juce::String& error)
    {
        file.deleteFile();

        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

        if (stream == nullptr || ! stream->openedOk())
        {
            error = juce::String::fromUTF8 (u8"无法创建文件：") + file.getFullPathName();
            return false;
        }

        const int numChannels = juce::jmin (2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();
        const int dataBytes   = numSamples * numChannels * 2;

        stream->write ("RIFF", 4);
        stream->writeInt (36 + dataBytes);
        stream->write ("WAVEfmt ", 8);
        stream->writeInt (16);                                   // fmt chunk size
        stream->writeShort (1);                                  // PCM
        stream->writeShort ((short) numChannels);
        stream->writeInt ((int) sampleRate);
        stream->writeInt ((int) sampleRate * numChannels * 2);   // byte rate
        stream->writeShort ((short) (numChannels * 2));          // block align
        stream->writeShort (16);                                 // bits per sample
        stream->write ("data", 4);
        stream->writeInt (dataBytes);

        // Interleaved, and clamped: a sample above 1.0 would wrap to a loud
        // click on playback, which is far worse than a flat top.
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float sample = juce::jlimit (-1.0f, 1.0f, buffer.getSample (ch, i));
                stream->writeShort ((short) juce::roundToInt (sample * 32767.0f));
            }
        }

        stream->flush();

        if (stream->getStatus().failed())
        {
            error = juce::String::fromUTF8 (u8"写入失败：") + stream->getStatus().getErrorMessage();
            file.deleteFile();
            return false;
        }

        return true;
    }
}

//==============================================================================
ExportResult renderToFile (const juce::File& destination,
                           const PublishedSchedule& schedule,
                           double sampleRate,
                           double tailSeconds,
                           const std::function<bool (double)>& progress,
                           PluginHost* livePlugins)
{
    ExportResult result;

    if (sampleRate <= 0.0)
        sampleRate = exportSampleRate;

    result.sampleRate  = sampleRate;
    result.numChannels = 2;

    // The content end, not the project grid: an export stops where the music
    // stops. A tiny tail keeps the very last release from being cut mid-sample.
    const double contentSeconds = juce::jmax (0.0, schedule.contentBeats) * schedule.secondsPerBeat;
    const double totalSeconds   = juce::jmax (0.05, contentSeconds + juce::jmax (0.0, tailSeconds));

    const int totalSamples = juce::jmax (1, (int) std::llround (totalSeconds * sampleRate));

    juce::AudioBuffer<float> bounce (2, totalSamples);
    bounce.clear();

    constexpr int blockSize = 4096;

    PlaybackEngine renderer;
    renderer.prepareToPlay (sampleRate, blockSize);

    // The bounce gets its OWN plugin instances. Two threads driving one plugin's
    // state machine would be a race inside someone else's code, and sharing a
    // lock instead would make the export and the live playback cancel each
    // other out block by block.
    std::unique_ptr<PluginHost> offlinePlugins;

    if (livePlugins != nullptr)
    {
        offlinePlugins = livePlugins->createOfflineClone (sampleRate, blockSize);

        if (offlinePlugins != nullptr)
            renderer.setPluginHost (offlinePlugins.get());
    }

    renderer.resetMixState();

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const int numThisBlock = juce::jmin (blockSize, totalSamples - offset);

        renderer.renderScheduleSpan (schedule,
                                     (double) offset / sampleRate,
                                     numThisBlock, bounce, offset, nullptr);

        if (progress != nullptr && ! progress ((double) (offset + numThisBlock) / (double) totalSamples))
        {
            destination.deleteFile();
            result.message = "cancelled";
            return result;
        }
    }

    result.peak = juce::jmax (bounce.getMagnitude (0, 0, totalSamples),
                              bounce.getMagnitude (1, 0, totalSamples));

    const float leftRms  = bounce.getRMSLevel (0, 0, totalSamples);
    const float rightRms = bounce.getRMSLevel (1, 0, totalSamples);
    result.rms = std::sqrt ((leftRms * leftRms + rightRms * rightRms) * 0.5f);

    // Ceiling guard, not a normaliser: only engages when the mix would clip.
    // Bouncing a project at a different level every time would be the worse
    // surprise, so the gain stays exactly 1.0 unless it has to move.
    constexpr float ceiling = 0.999f;

    if (result.peak > ceiling)
    {
        bounce.applyGain (ceiling / result.peak);
        result.peakLimited = true;
    }

    juce::String error;

    if (! writePcm16 (destination, bounce, sampleRate, error))
    {
        result.message = error;
        return result;
    }

    result.ok         = true;
    result.numSamples = totalSamples;
    result.seconds    = (double) totalSamples / sampleRate;
    result.message    = juce::String::fromUTF8 (u8"已导出 ");

    return result;
}

} // namespace wav
} // namespace atomic

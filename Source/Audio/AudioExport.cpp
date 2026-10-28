#include "AudioExport.h"

#include "Mp3Encoder.h"
#include "PluginHost.h"

#include <cmath>

namespace atomic
{

//==============================================================================
juce::String ExportSettings::describe() const
{
    const juce::String rate ("44.1 kHz");

    if (format == ExportFormat::mp3)
        return rate + " · " + juce::String (mp3::clampBitrate (mp3Bitrate)) + " kbps MP3"
                 + (mono ? juce::String::fromUTF8 (u8" · 单声道")
                         : juce::String::fromUTF8 (u8" · 立体声"));

    return rate + " · " + juce::String (bitDepth) + "-bit"
             + (bitDepth == 32 ? juce::String::fromUTF8 (u8" 浮点") : juce::String())
             + (mono ? juce::String::fromUTF8 (u8" · 单声道")
                     : juce::String::fromUTF8 (u8" · 立体声"));
}

namespace bounce
{

namespace
{
    /** How many bytes one sample takes, or 0 for the float case. */
    int bytesPerSample (int bitDepth) noexcept
    {
        return bitDepth == 32 ? 4 : (bitDepth == 24 ? 3 : 2);
    }

    /** Writes a canonical RIFF/WAVE file: PCM 16/24, or IEEE float 32.

        juce_audio_formats would do this too, and the moment this project needs
        a third header layout it should switch to it — but three writers that
        differ only in how a sample is packed is still one writer's worth of
        code, and pulling the whole module in for it would be the bigger cost.

        A float file carries a `fact` chunk as well: readers that follow the
        spec strictly want it for every non-PCM format, and the four bytes of
        "this many sample frames" cost nothing. */
    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer,
                   double sampleRate, int bitDepth, juce::String& error)
    {
        bitDepth = (bitDepth == 24 || bitDepth == 32) ? bitDepth : 16;

        file.deleteFile();

        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

        if (stream == nullptr || ! stream->openedOk())
        {
            error = juce::String::fromUTF8 (u8"无法创建文件：") + file.getFullPathName();
            return false;
        }

        const int numChannels = juce::jlimit (1, 2, buffer.getNumChannels());
        const int numSamples  = buffer.getNumSamples();
        const int sampleBytes = bytesPerSample (bitDepth);
        const int dataBytes   = numSamples * numChannels * sampleBytes;
        const bool isFloat    = (bitDepth == 32);

        const int factBytes   = isFloat ? 12 : 0;          // "fact" + size + frames
        const int fmtBytes    = 16;

        stream->write ("RIFF", 4);
        stream->writeInt (4 + (8 + fmtBytes) + factBytes + (8 + dataBytes));
        stream->write ("WAVE", 4);

        stream->write ("fmt ", 4);
        stream->writeInt (fmtBytes);
        stream->writeShort ((short) (isFloat ? 3 : 1));           // IEEE float / PCM
        stream->writeShort ((short) numChannels);
        stream->writeInt ((int) sampleRate);
        stream->writeInt ((int) sampleRate * numChannels * sampleBytes);   // byte rate
        stream->writeShort ((short) (numChannels * sampleBytes));          // block align
        stream->writeShort ((short) bitDepth);

        if (isFloat)
        {
            stream->write ("fact", 4);
            stream->writeInt (4);
            stream->writeInt (numSamples);
        }

        stream->write ("data", 4);
        stream->writeInt (dataBytes);

        // Interleaved, and clamped: a sample above 1.0 would wrap to a loud
        // click on playback, which is far worse than a flat top.
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float sample = juce::jlimit (-1.0f, 1.0f, buffer.getSample (ch, i));

                if (isFloat)
                {
                    // ieee754 bits, little endian. JUCE has no portable helper
                    // for this that does not drag in a stream object.
                    int bits = 0;
                    std::memcpy (&bits, &sample, sizeof (bits));
                    stream->writeInt (bits);
                }
                else if (bitDepth == 24)
                {
                    const auto value = (juce::int32) juce::roundToInt (sample * 8388607.0f);

                    stream->writeByte ((char) (value & 0xff));
                    stream->writeByte ((char) ((value >> 8) & 0xff));
                    stream->writeByte ((char) ((value >> 16) & 0xff));
                }
                else
                {
                    stream->writeShort ((short) juce::roundToInt (sample * 32767.0f));
                }
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
                           const ExportSettings& settings,
                           double sampleRate,
                           double tailSeconds,
                           const std::function<bool (double)>& progress,
                           PluginHost* livePlugins,
                           double startSeconds)
{
    ExportResult result;

    if (sampleRate <= 0.0)
        sampleRate = exportSampleRate;

    result.sampleRate = sampleRate;

    // The content end, not the project grid: an export stops where the music
    // stops. A tiny tail keeps the very last release from being cut mid-sample.
    // Phase 80: the content end is a BEAT, and the wall-clock answer comes from
    // the schedule's tempo map — an accelerando makes the last bar shorter, so
    // `beats * one number` would export a tail of silence.
    const double contentSeconds = schedule.tempo.secondsAtBeat (juce::jmax (0.0, schedule.contentBeats));

    startSeconds = juce::jlimit (0.0, juce::jmax (0.0, contentSeconds - 0.001), startSeconds);

    const double totalSeconds   = juce::jmax (0.05, contentSeconds - startSeconds
                                                       + juce::jmax (0.0, tailSeconds));

    const int totalSamples = juce::jmax (1, (int) std::llround (totalSeconds * sampleRate));

    juce::AudioBuffer<float> bounceBuffer (2, totalSamples);
    bounceBuffer.clear();

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
                                     startSeconds + (double) offset / sampleRate,
                                     numThisBlock, bounceBuffer, offset, nullptr);

        if (progress != nullptr && ! progress ((double) (offset + numThisBlock) / (double) totalSamples))
        {
            destination.deleteFile();
            result.message = "cancelled";
            return result;
        }
    }

    //-- Mono downmix, BEFORE the measurements --------------------------------
    // A mono export is half as loud at the same peak, and reporting the stereo
    // peak of a file that holds something else would be a lie about what the
    // user is about to hear. So the buffer becomes what will be written, and
    // then it gets measured.
    if (settings.mono)
    {
        juce::AudioBuffer<float> mono (1, totalSamples);

        for (int i = 0; i < totalSamples; ++i)
            mono.setSample (0, i, (bounceBuffer.getSample (0, i) + bounceBuffer.getSample (1, i)) * 0.5f);

        bounceBuffer = std::move (mono);
    }

    result.numChannels = bounceBuffer.getNumChannels();

    result.peak = juce::jmax (bounceBuffer.getMagnitude (0, 0, totalSamples),
                              bounceBuffer.getNumChannels() > 1
                                ? bounceBuffer.getMagnitude (1, 0, totalSamples) : 0.0f);

    const float leftRms  = bounceBuffer.getRMSLevel (0, 0, totalSamples);
    const float rightRms = bounceBuffer.getNumChannels() > 1
                             ? bounceBuffer.getRMSLevel (1, 0, totalSamples) : leftRms;
    result.rms = std::sqrt ((leftRms * leftRms + rightRms * rightRms) * 0.5f);

    // Ceiling guard, not a normaliser: only engages when the mix would clip.
    // Bouncing a project at a different level every time would be the worse
    // surprise, so the gain stays exactly 1.0 unless it has to move.
    constexpr float ceiling = 0.999f;

    if (result.peak > ceiling)
    {
        bounceBuffer.applyGain (ceiling / result.peak);
        result.peakLimited = true;
    }

    //-- Write it --------------------------------------------------------------
    juce::String error;

    if (settings.format == ExportFormat::mp3)
    {
        const auto written = mp3::encode (destination, bounceBuffer, sampleRate,
                                          settings.mp3Bitrate);

        if (written.isNotEmpty())
        {
            result.message = written;
            destination.deleteFile();
            return result;
        }
    }
    else if (! writeWav (destination, bounceBuffer, sampleRate, settings.bitDepth, error))
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

//==============================================================================
ExportResult renderToFile (const juce::File& destination,
                           const PublishedSchedule& schedule,
                           double sampleRate,
                           double tailSeconds,
                           const std::function<bool (double)>& progress,
                           PluginHost* livePlugins,
                           double startSeconds)
{
    return renderToFile (destination, schedule, ExportSettings{}, sampleRate, tailSeconds,
                         progress, livePlugins, startSeconds);
}

} // namespace bounce
} // namespace atomic

#include "Media/SampleLibrary.h"

#include "Util/Text.h"

#include <cmath>
#include <cstring>
#include <string>

namespace atomic
{

//==============================================================================
float SampleBuffer::peakLevel() const noexcept
{
    float highest = 0.0f;

    for (const auto sample : frames)
        highest = juce::jmax (highest, std::abs (sample));

    return highest;
}

//==============================================================================
namespace
{
    /** Little-endian readers over a raw byte range. Bounds are checked once, at
        the top of loadSampleFile, so these can assume a valid chunk. */
    juce::uint32 readU32 (const juce::uint8* p) noexcept
    {
        return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8)
             | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
    }

    juce::uint16 readU16 (const juce::uint8* p) noexcept
    {
        return (juce::uint16) ((juce::uint16) p[0] | ((juce::uint16) p[1] << 8));
    }

    /** 24-bit little-endian PCM, sign extended. There is no 24-bit integer type,
        so this is the one width that needs its own decoder. */
    float read24 (const juce::uint8* p) noexcept
    {
        const juce::int32 value = ((juce::int32) p[0] << 8)
                                | ((juce::int32) p[1] << 16)
                                | ((juce::int32) p[2] << 24);

        return (float) (value / 256) / 8388608.0f;
    }

    std::string fourCC (const juce::uint8* p)
    {
        return std::string (reinterpret_cast<const char*> (p), 4);
    }
}

bool isSampleFile (const juce::File& file)
{
    // One format, stated plainly. juce_audio_formats would also read AIFF and
    // FLAC, but it brings a codec registry and an audio-source layer this app
    // has no other use for — and a browser that lists files it cannot decode
    // produces a dead click, which is worse than a short list.
    const auto ext = file.getFileExtension().toLowerCase();

    return ext == ".wav" || ext == ".wave";
}

//==============================================================================
LoadedSample loadSampleFile (const juce::File& file)
{
    LoadedSample result;

    if (! file.existsAsFile())
    {
        result.error = utf8 ("文件不存在");
        return result;
    }

    juce::MemoryBlock data;

    if (! file.loadFileAsData (data) || data.getSize() < 44)
    {
        result.error = utf8 ("读不出文件内容");
        return result;
    }

    const auto* bytes = static_cast<const juce::uint8*> (data.getData());
    const auto  size  = (size_t) data.getSize();

    if (fourCC (bytes) != "RIFF" || fourCC (bytes + 8) != "WAVE")
    {
        result.error = utf8 ("不是 WAV 文件");
        return result;
    }

    int           formatTag   = 0;      // 1 = PCM, 3 = IEEE float, 0xFFFE = extensible
    int           numChannels = 0;
    double        sampleRate  = 0.0;
    int           bits        = 0;
    size_t        dataOffset  = 0;
    size_t        dataSize    = 0;

    size_t pos = 12;

    while (pos + 8 <= size)
    {
        const auto id        = fourCC (bytes + pos);
        const auto chunkSize = (size_t) readU32 (bytes + pos + 4);
        const auto body      = pos + 8;

        if (id == "fmt " && body + 16 <= size)
        {
            formatTag   = (int) readU16 (bytes + body);
            numChannels = (int) readU16 (bytes + body + 2);
            sampleRate  = (double) readU32 (bytes + body + 4);
            bits        = (int) readU16 (bytes + body + 14);

            // WAVE_FORMAT_EXTENSIBLE carries the real tag in the first two bytes
            // of the sub-format GUID. Without this, every 24-bit file written by
            // a proper DAW looks like an unknown format and gets rejected.
            if (formatTag == 0xFFFE && body + 26 <= size)
                formatTag = (int) readU16 (bytes + body + 24);
        }
        else if (id == "data")
        {
            dataOffset = body;
            dataSize   = juce::jmin (chunkSize, size - body);
        }

        // Chunks are word aligned: an odd size is followed by one pad byte.
        pos = body + chunkSize + (chunkSize & 1u);
    }

    if (numChannels <= 0 || bits <= 0 || sampleRate <= 0.0 || dataSize == 0)
    {
        result.error = utf8 ("文件里没有可用的音频段");
        return result;
    }

    if (sampleRate > 1.0e6)
    {
        result.error = utf8 ("采样率异常");
        return result;
    }

    if (formatTag != 1 && formatTag != 3)
    {
        result.error = utf8 ("不支持的压缩格式（只收 PCM / 浮点）");
        return result;
    }

    int bytesPerSample = 0;

    if (formatTag == 1)
    {
        if (bits == 8 || bits == 16 || bits == 24 || bits == 32)
            bytesPerSample = bits / 8;
    }
    else
    {
        if (bits == 32 || bits == 64)
            bytesPerSample = bits / 8;
    }

    if (bytesPerSample == 0 || numChannels > 32)
    {
        result.error = utf8 ("无法识别的位深");
        return result;
    }

    const auto frameBytes = (size_t) (bytesPerSample * numChannels);
    auto       frames     = (int) (dataSize / frameBytes);

    if (frames <= 0)
    {
        result.error = utf8 ("音频段是空的");
        return result;
    }

    // A ten-minute ceiling. A corrupt header claiming four billion frames would
    // otherwise turn a drag-and-drop into an out-of-memory crash.
    const int maxFrames = (int) juce::jmin (1.0e9, sampleRate * 600.0);
    frames = juce::jmin (frames, maxFrames);

    // Two channels are all the render path has a use for; a 5.1 sample keeps
    // its first two rather than being silently folded into noise.
    const int outChannels = juce::jmin (2, numChannels);

    auto buffer = std::make_shared<SampleBuffer>();
    buffer->sampleRate = sampleRate;
    buffer->channels   = outChannels;
    buffer->frames.resize ((size_t) frames * (size_t) outChannels);

    const auto* src = bytes + dataOffset;

    for (int i = 0; i < frames; ++i)
    {
        const auto* frame = src + (size_t) i * frameBytes;

        for (int c = 0; c < outChannels; ++c)
        {
            const auto* p = frame + (size_t) c * (size_t) bytesPerSample;
            float value = 0.0f;

            if (formatTag == 3)
            {
                if (bits == 32)
                {
                    const auto raw = readU32 (p);
                    std::memcpy (&value, &raw, sizeof (float));
                }
                else
                {
                    const juce::uint64 raw = (juce::uint64) readU32 (p)
                                           | ((juce::uint64) readU32 (p + 4) << 32);
                    double d = 0.0;
                    std::memcpy (&d, &raw, sizeof (double));
                    value = (float) d;
                }
            }
            else if (bits == 8)
            {
                value = ((float) p[0] - 128.0f) / 128.0f;
            }
            else if (bits == 16)
            {
                value = (float) (juce::int16) readU16 (p) / 32768.0f;
            }
            else if (bits == 24)
            {
                value = read24 (p);
            }
            else
            {
                value = (float) ((double) (juce::int32) readU32 (p) / 2147483648.0);
            }

            buffer->frames[(size_t) i * (size_t) outChannels + (size_t) c] = value;
        }
    }

    // Peak envelope, once. Painting a waveform must not walk eight million
    // frames on every frame of a scroll.
    buffer->peaks.assign ((size_t) SampleBuffer::peakBuckets, 0.0f);

    const double step = (double) frames / (double) SampleBuffer::peakBuckets;

    for (int b = 0; b < SampleBuffer::peakBuckets; ++b)
    {
        const int from = (int) (b * step);
        const int to   = juce::jlimit (from + 1, frames, (int) ((b + 1) * step));
        float     peak = 0.0f;

        for (int i = from; i < to; ++i)
            for (int c = 0; c < outChannels; ++c)
                peak = juce::jmax (peak, std::abs (buffer->frames[(size_t) i * (size_t) outChannels
                                                                  + (size_t) c]));

        buffer->peaks[(size_t) b] = juce::jlimit (0.0f, 1.0f, peak);
    }

    result.buffer = std::move (buffer);
    return result;
}

//==============================================================================
bool writeTestSampleFile (const juce::File& file, double seconds, double sampleRate,
                          int channels, double frequency)
{
    if (seconds <= 0.0 || sampleRate <= 0.0 || channels <= 0)
        return false;

    const int bits        = 16;
    const int frameBytes  = channels * bits / 8;
    const int frames      = (int) (seconds * sampleRate);

    if (frames <= 0 || frames > 44100 * 600)
        return false;

    const int dataBytes = frames * frameBytes;

    juce::MemoryOutputStream out;
    out.write ("RIFF", 4);
    out.writeInt ((int) (36 + dataBytes));
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    out.writeInt (16);
    out.writeShort (1);
    out.writeShort ((short) channels);
    out.writeInt ((int) sampleRate);
    out.writeInt ((int) (sampleRate * frameBytes));
    out.writeShort ((short) frameBytes);
    out.writeShort ((short) bits);
    out.write ("data", 4);
    out.writeInt (dataBytes);

    for (int i = 0; i < frames; ++i)
    {
        const double t     = (double) i / sampleRate;
        const double fade  = std::exp (-3.0 * t / seconds);       // decaying tail
        const double phase = 2.0 * juce::MathConstants<double>::pi * frequency * t;

        for (int c = 0; c < channels; ++c)
        {
            // Second channel a fifth up, so a stereo sample is visibly stereo.
            const double detune = c == 1 ? 1.5 : 1.0;
            const double value  = std::sin (phase * detune) * fade * 0.6;
            out.writeShort ((short) juce::jlimit (-32767.0, 32767.0, value * 32767.0));
        }
    }

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    return file.replaceWithData (out.getData(), out.getDataSize());
}

} // namespace atomic

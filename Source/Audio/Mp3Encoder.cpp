#include "Mp3Encoder.h"

#include "Util/Text.h"

#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #include <mferror.h>
#endif

namespace atomic::mp3
{

int clampBitrate (int kbps) noexcept
{
    return juce::jlimit (minBitrateKbps, maxBitrateKbps, kbps);
}

#if JUCE_WINDOWS

//==============================================================================
namespace
{
    juce::String hresultText (HRESULT hr)
    {
        return "0x" + juce::String::toHexString ((int) hr).paddedLeft ('0', 8);
    }

    /** MF counts in 100 ns units, and the sample time must be the ABSOLUTE frame
        position, not an accumulating sum — one rounding error per block would
        drift the whole file out of sync with its own duration. */
    LONGLONG framesToHns (LONGLONG frames, double sampleRate) noexcept
    {
        return (LONGLONG) std::llround ((double) frames * 10000000.0 / sampleRate);
    }

    void release (IUnknown* object) noexcept
    {
        if (object != nullptr)
            object->Release();
    }
}

bool isAvailable() noexcept { return true; }

juce::String encode (const juce::File& destination,
                     const juce::AudioBuffer<float>& buffer,
                     double sampleRate,
                     int bitrateKbps)
{
    const int numChannels = juce::jlimit (1, 2, buffer.getNumChannels());
    const int numSamples  = buffer.getNumSamples();

    if (numSamples <= 0 || sampleRate <= 0.0)
        return utf8 ("没有可编码的音频");

    const int bitrate = clampBitrate (bitrateKbps);

    HRESULT hr = MFStartup (MF_VERSION, MFSTARTUP_LITE);

    if (FAILED (hr))
        return utf8 ("本机没有可用的 MP3 编码器（Media Foundation 无法启动 ")
                 + hresultText (hr) + utf8 ("）");

    struct ShutdownGuard
    {
        ~ShutdownGuard() { MFShutdown(); }
    } shutdownGuard;

    // The sink is chosen from the file extension, so the caller's ".mp3" is
    // load-bearing — this is not a place to be generous with guesses.
    IMFSinkWriter* writer = nullptr;

    hr = MFCreateSinkWriterFromURL (destination.getFullPathName().toWideCharPointer(),
                                    nullptr, nullptr, &writer);

    if (FAILED (hr) || writer == nullptr)
        return utf8 ("无法创建 MP3 输出文件：") + hresultText (hr);

    auto fail = [writer] (const juce::String& message)
    {
        release (writer);
        return message;
    };

    //-- Output type: MP3, the rate and channel count we were handed, and a byte
    //   rate derived from the chosen bitrate. ----------------------------------
    {
        IMFMediaType* outType = nullptr;

        if (FAILED (hr = MFCreateMediaType (&outType)))
            return fail (utf8 ("无法创建媒体类型：") + hresultText (hr));

        outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_MP3);
        outType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) numChannels);
        outType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        outType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (bitrate * 1000 / 8));

        DWORD streamIndex = 0;
        hr = writer->AddStream (outType, &streamIndex);

        release (outType);

        if (FAILED (hr))
            return fail (utf8 ("本机不接受该 MP3 格式（可能是缺少媒体功能包）：")
                           + hresultText (hr));

        //-- Input type: plain 16-bit PCM. Encoding from integers, not floats,
        //   keeps the hand-off exactly as wide as the encoder's own input. -----
        IMFMediaType* inType = nullptr;

        if (FAILED (hr = MFCreateMediaType (&inType)))
            return fail (utf8 ("无法创建媒体类型：") + hresultText (hr));

        inType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        inType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) numChannels);
        inType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) (numChannels * 2));
        inType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           (UINT32) ((int) sampleRate * numChannels * 2));

        hr = writer->SetInputMediaType (streamIndex, inType, nullptr);

        release (inType);

        if (FAILED (hr))
            return fail (utf8 ("MP3 编码器不接受 PCM 输入：") + hresultText (hr));

        hr = writer->BeginWriting();

        if (FAILED (hr))
            return fail (utf8 ("MP3 编码器无法开始写入：") + hresultText (hr));

        //-- Samples, block by block -------------------------------------------
        constexpr int blockFrames = 4096;

        for (int offset = 0; offset < numSamples; offset += blockFrames)
        {
            const int   frames = juce::jmin (blockFrames, numSamples - offset);
            const DWORD bytes  = (DWORD) (frames * numChannels * 2);

            IMFMediaBuffer* mediaBuffer = nullptr;

            if (FAILED (hr = MFCreateMemoryBuffer (bytes, &mediaBuffer)))
                return fail (utf8 ("MP3 编码失败（缓冲）：") + hresultText (hr));

            BYTE* dst = nullptr;

            if (FAILED (hr = mediaBuffer->Lock (&dst, nullptr, nullptr)))
            {
                release (mediaBuffer);
                return fail (utf8 ("MP3 编码失败（锁定缓冲）：") + hresultText (hr));
            }

            for (int i = 0; i < frames; ++i)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    // Clamped: a sample above 1.0 would wrap to a loud click,
                    // the same reason the WAV writer clamps.
                    const float sample = juce::jlimit (-1.0f, 1.0f,
                                                       buffer.getSample (ch, offset + i));
                    const auto  value  = (juce::uint16) (juce::int16) juce::roundToInt (sample * 32767.0f);

                    *dst++ = (BYTE) (value & 0xff);
                    *dst++ = (BYTE) (value >> 8);
                }
            }

            mediaBuffer->Unlock();
            mediaBuffer->SetCurrentLength (bytes);

            IMFSample* mediaSample = nullptr;

            if (FAILED (hr = MFCreateSample (&mediaSample)))
            {
                release (mediaBuffer);
                return fail (utf8 ("MP3 编码失败（样本）：") + hresultText (hr));
            }

            mediaSample->AddBuffer (mediaBuffer);
            mediaSample->SetSampleTime (framesToHns ((LONGLONG) offset, sampleRate));
            mediaSample->SetSampleDuration (framesToHns ((LONGLONG) frames, sampleRate));

            hr = writer->WriteSample (streamIndex, mediaSample);

            release (mediaSample);
            release (mediaBuffer);

            if (FAILED (hr))
                return fail (utf8 ("MP3 写入失败：") + hresultText (hr));
        }
    }

    hr = writer->Finalize();
    release (writer);

    if (FAILED (hr))
        return utf8 ("MP3 收尾失败：") + hresultText (hr);

    if (! destination.existsAsFile() || destination.getSize() == 0)
        return utf8 ("MP3 编码器没有写出文件");

    return {};
}

#else

bool isAvailable() noexcept { return false; }

juce::String encode (const juce::File& destination,
                     const juce::AudioBuffer<float>& buffer,
                     double sampleRate,
                     int bitrateKbps)
{
    juce::ignoreUnused (destination, buffer, sampleRate, bitrateKbps);

    return utf8 ("本平台未内置 MP3 编码器，请改用 WAV 导出");
}

#endif

} // namespace atomic::mp3

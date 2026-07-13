// Editor/MovieEncoder.cpp - Media Foundation H.264 .mp4 encoder.
#include "Editor/MovieEncoder.h"

#include "Core/Log.h"
#include "Editor/MovieRender.h" // movie::WritePng

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace hbe::movie {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

Mp4Writer::~Mp4Writer() {
    if (writer_) {
        static_cast<IMFSinkWriter*>(writer_)->Release();
        writer_ = nullptr;
    }
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}

bool Mp4Writer::Begin(const fs::path& path, u32 width, u32 height, u32 fps,
                      std::vector<i16> audioPcmStereo, u32 audioRate, u32 bitrate) {
    if (width == 0 || height == 0 || fps == 0) return false;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        HBE_ERROR("MovieEncoder: MFStartup failed.");
        return false;
    }
    mfStarted_ = true;
    w_ = width;
    h_ = height;
    fps_ = fps;
    frameDur_ = 10000000LL / static_cast<long long>(fps);
    videoTime_ = 0;
    // Rough VBR-ish target: ~0.1 bit/pixel/frame, floored at 4 Mbit/s.
    const u32 br = bitrate ? bitrate
                           : std::max(4000000u, static_cast<u32>(static_cast<u64>(width) * height *
                                                                 fps / 10));

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    ComPtr<IMFSinkWriter> writer;
    const std::wstring wpath = path.wstring();
    if (FAILED(MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &writer))) {
        HBE_ERROR("MovieEncoder: cannot create sink writer for '{}'.", path.string());
        return false;
    }

    // Output: H.264.
    ComPtr<IMFMediaType> outT;
    MFCreateMediaType(&outT);
    outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outT->SetUINT32(MF_MT_AVG_BITRATE, br);
    outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outT.Get(), MF_MT_FRAME_SIZE, w_, h_);
    MFSetAttributeRatio(outT.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(outT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD vs = 0;
    if (FAILED(writer->AddStream(outT.Get(), &vs))) {
        HBE_ERROR("MovieEncoder: AddStream(H264) failed.");
        return false;
    }
    videoStream_ = vs;

    // Input: RGB32 (BGRA byte order), top-down (positive default stride). The sink
    // writer auto-inserts the color-converter MFT to feed the encoder.
    ComPtr<IMFMediaType> inT;
    MFCreateMediaType(&inT);
    inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inT->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(w_ * 4)); // + = top-down
    MFSetAttributeSize(inT.Get(), MF_MT_FRAME_SIZE, w_, h_);
    MFSetAttributeRatio(inT.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(inT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->SetInputMediaType(videoStream_, inT.Get(), nullptr))) {
        HBE_ERROR("MovieEncoder: SetInputMediaType(RGB32) failed.");
        return false;
    }

    // Optional AAC audio stream (added before BeginWriting; samples written in Finish).
    if (!audioPcmStereo.empty()) {
        audioPcm_ = std::move(audioPcmStereo);
        audioRate_ = audioRate ? audioRate : 48000;
        ComPtr<IMFMediaType> aOut;
        MFCreateMediaType(&aOut);
        aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioRate_);
        aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbit/s AAC
        DWORD as = 0;
        if (SUCCEEDED(writer->AddStream(aOut.Get(), &as))) {
            audioStream_ = as;
            ComPtr<IMFMediaType> aIn;
            MFCreateMediaType(&aIn);
            aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioRate_);
            aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            if (SUCCEEDED(writer->SetInputMediaType(audioStream_, aIn.Get(), nullptr)))
                haveAudio_ = true;
        }
        if (!haveAudio_) HBE_WARN("MovieEncoder: AAC audio stream setup failed; video-only.");
    }

    if (FAILED(writer->BeginWriting())) {
        HBE_ERROR("MovieEncoder: BeginWriting failed.");
        return false;
    }
    writer->AddRef();
    writer_ = writer.Get();
    return true;
}

bool Mp4Writer::WriteVideoFrame(const std::vector<u8>& rgba) {
    if (!writer_) return false;
    const DWORD bytes = w_ * h_ * 4;
    if (rgba.size() < bytes) return false;
    auto* writer = static_cast<IMFSinkWriter*>(writer_);

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    // RGBA (top-down) -> BGRA (top-down); RGB32 is BGRA byte order.
    const usize px = static_cast<usize>(w_) * h_;
    for (usize i = 0; i < px; ++i) {
        dst[i * 4 + 0] = rgba[i * 4 + 2];
        dst[i * 4 + 1] = rgba[i * 4 + 1];
        dst[i * 4 + 2] = rgba[i * 4 + 0];
        dst[i * 4 + 3] = rgba[i * 4 + 3];
    }
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    ComPtr<IMFSample> smp;
    MFCreateSample(&smp);
    smp->AddBuffer(buf.Get());
    smp->SetSampleTime(videoTime_);
    smp->SetSampleDuration(frameDur_);
    const HRESULT hr = writer->WriteSample(videoStream_, smp.Get());
    videoTime_ += frameDur_;
    return SUCCEEDED(hr);
}

bool Mp4Writer::Finish() {
    bool ok = true;
    if (writer_) {
        auto* writer = static_cast<IMFSinkWriter*>(writer_);
        // Write the whole audio track (in ~0.1s chunks) before finalizing; MF muxes
        // it against the already-written video by timestamp.
        if (haveAudio_ && !audioPcm_.empty()) {
            const usize totalFrames = audioPcm_.size() / 2; // interleaved stereo
            const usize chunk = std::max<u32>(1, audioRate_ / 10);
            long long atime = 0;
            for (usize f = 0; f < totalFrames; f += chunk) {
                const usize n = std::min(chunk, totalFrames - f);
                const DWORD bytes = static_cast<DWORD>(n * 2 * sizeof(i16));
                ComPtr<IMFMediaBuffer> buf;
                if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) break;
                BYTE* dst = nullptr;
                buf->Lock(&dst, nullptr, nullptr);
                std::memcpy(dst, audioPcm_.data() + f * 2, bytes);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
                ComPtr<IMFSample> smp;
                MFCreateSample(&smp);
                smp->AddBuffer(buf.Get());
                const long long dur = static_cast<long long>(n) * 10000000LL / audioRate_;
                smp->SetSampleTime(atime);
                smp->SetSampleDuration(dur);
                writer->WriteSample(audioStream_, smp.Get());
                atime += dur;
            }
        }
        ok = SUCCEEDED(writer->Finalize());
        writer->Release();
        writer_ = nullptr;
    }
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    ok_ = ok;
    if (!ok) HBE_ERROR("MovieEncoder: Finalize failed.");
    return ok;
}

bool DecodeFirstFrameToPng(const fs::path& mp4, const fs::path& png) {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    bool ok = false;
    {
        ComPtr<IMFSourceReader> reader;
        const std::wstring wp = mp4.wstring();
        // Advanced video processing lets the reader insert a YUV->RGB converter so we
        // can request RGB32 output (the H.264 decoder itself only emits NV12).
        ComPtr<IMFAttributes> rattr;
        MFCreateAttributes(&rattr, 1);
        rattr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(MFCreateSourceReaderFromURL(wp.c_str(), rattr.Get(), &reader))) {
            ComPtr<IMFMediaType> want;
            MFCreateMediaType(&want);
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                                   want.Get()))) {
                HBE_ERROR("MovieEncoder: verify-decode cannot set RGB32 output.");
                MFShutdown();
                return false;
            }

            DWORD flags = 0, streamIdx = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            if (SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx,
                                             &flags, &ts, &smp)) &&
                smp) {
                ComPtr<IMFMediaType> cur;
                reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                UINT32 strideU = w * 4;
                cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU);
                // The attribute is stored unsigned but is a SIGNED stride (negative =
                // bottom-up); reinterpret the bits as INT32 before widening to LONG.
                const LONG stride = static_cast<LONG>(static_cast<INT32>(strideU));
                ComPtr<IMFMediaBuffer> buf;
                if (w && h && SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
                    BYTE* data = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                        std::vector<u8> rgba(static_cast<usize>(w) * h * 4);
                        for (UINT32 y = 0; y < h; ++y) {
                            const BYTE* srow = stride < 0
                                                   ? data + static_cast<LONG>(h - 1 - y) * (-stride)
                                                   : data + static_cast<LONG>(y) * stride;
                            for (UINT32 x = 0; x < w; ++x) {
                                u8* d = &rgba[(static_cast<usize>(y) * w + x) * 4];
                                d[0] = srow[x * 4 + 2]; // R <- B
                                d[1] = srow[x * 4 + 1];
                                d[2] = srow[x * 4 + 0]; // B <- R
                                d[3] = 255;
                            }
                        }
                        buf->Unlock();
                        ok = WritePng(png, w, h, rgba);
                    }
                }
            }
        }
    }
    MFShutdown();
    return ok;
}

} // namespace hbe::movie

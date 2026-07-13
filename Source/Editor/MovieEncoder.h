// Editor/MovieEncoder.h - H.264 .mp4 writer via Windows Media Foundation.
//
// Feed RGBA8 frames (top row first) via WriteVideoFrame; optionally feed a full
// interleaved-stereo 16-bit PCM track via SetAudio before Finish. No external
// tools (ffmpeg) - Media Foundation ships with Windows. Editor-only, main-thread.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <vector>

namespace hbe::movie {

class Mp4Writer {
public:
    ~Mp4Writer();
    // Opens `path` for an H.264 video stream at width x height @ fps, plus (when
    // `audioPcmStereo` is non-empty) an AAC audio stream from that interleaved-stereo
    // int16 track (the MF stream must be added before writing, hence at Begin). The
    // audio is muxed during Finish(). `bitrate` 0 = auto (~res-scaled). Returns false
    // if Media Foundation / the sink writer fails.
    bool Begin(const std::filesystem::path& path, u32 width, u32 height, u32 fps,
               std::vector<i16> audioPcmStereo = {}, u32 audioRate = 48000, u32 bitrate = 0);
    // Appends one frame (top-down RGBA8, exactly width*height*4 bytes).
    bool WriteVideoFrame(const std::vector<u8>& rgba);
    // Finalizes the file (writes the audio track, closes the container). Success.
    bool Finish();
    bool Ok() const { return ok_; }

private:
    void* writer_ = nullptr; // IMFSinkWriter*
    unsigned long videoStream_ = 0;
    unsigned long audioStream_ = 0;
    bool haveAudio_ = false;
    u32 w_ = 0, h_ = 0, fps_ = 30;
    long long frameDur_ = 0; // 100ns units
    long long videoTime_ = 0;
    bool ok_ = false;
    bool mfStarted_ = false;
    std::vector<u8> bgra_;         // scratch: swizzled frame
    std::vector<i16> audioPcm_;    // interleaved stereo
    u32 audioRate_ = 48000;
};

// Verification helper: decodes the FIRST video frame of `mp4` back to RGBA8 and
// writes it as a PNG (so the encode/orientation/colour can be checked without a
// player). Returns false on failure.
bool DecodeFirstFrameToPng(const std::filesystem::path& mp4, const std::filesystem::path& png);

} // namespace hbe::movie

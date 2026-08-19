// Editor/MovieRender.h - offline movie render (trailers to video).
//
// Renders a cinematic (a .hbcutscene, or "the current scene for N seconds") by
// marching time forward at a FIXED dt = 1/fps (deterministic frame-stepping),
// capturing each offscreen frame via Renderer::ReadbackViewportColor, and encoding.
// Runs as a MAIN-THREAD state machine (one output frame per editor tick) because
// GPU work must stay on the device thread. Editor-only.
#pragma once

#include "Assets/CutsceneAsset.h"
#include "Cinematics/Evaluator.h" // cine::Sequence + SequenceInstance (offline .hbseq render)
#include "Core/Types.h"
#include "Editor/MovieEncoder.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {

class Engine;

namespace movie {

// Writes RGBA8 pixels (top row first, tightly packed) as a PNG. Creates parent dirs.
bool WritePng(const std::filesystem::path& path, u32 w, u32 h, const std::vector<u8>& rgba);

struct MovieConfig {
    std::string cutsceneRel;         // .hbcutscene relative to Assets/ ("" = current scene)
    std::string sequenceRel;         // .hbseq Sequencer timeline relative to Assets/ (overrides cutscene)
    std::string musicRel;            // optional background-music file (rel Assets/; .uaf/.mp3/.wav)
    std::filesystem::path outputFile; // .mp4 target (if set, encode H.264; else PNG dir)
    std::filesystem::path outputDir; // PNG frame sequence dir (used when outputFile empty)
    u32 width = 1920, height = 1080;
    u32 fps = 30;
    u32 warmupFrames = 12;           // rendered (not captured) so TAA/exposure settle
    f32 duration = 5.0f;             // seconds (cutscene mode uses the asset's duration)
};

// A running render. Drive it: Start() once, Tick() every editor frame until
// Finished(), then Stop(). The scene is snapshotted at Start (SceneSnapshot()) so the
// caller can restore it afterwards (cutscene evaluation mutates entity transforms).
class MovieJob {
public:
    void Start(Engine& engine, const std::filesystem::path& assetsDir, const MovieConfig& cfg);
    void Tick(Engine& engine); // once per editor frame while Active()
    void Cancel() { cancel_ = true; }
    void Stop(Engine& engine); // clears the fixed-dt override; caller restores the scene

    bool Active() const { return phase_ == Phase::WarmUp || phase_ == Phase::Capture; }
    bool Finished() const { return phase_ == Phase::Done; }
    f32 Progress() const { return progress_; }
    const std::string& Status() const { return status_; }
    u32 FramesWritten() const { return framesWritten_; }
    u32 TotalFrames() const { return static_cast<u32>(totalFrames_); }
    const std::string& SceneSnapshot() const { return sceneSnapshot_; }

private:
    enum class Phase { Idle, WarmUp, Capture, Done };
    Phase phase_ = Phase::Idle;
    MovieConfig cfg_;
    std::filesystem::path assetsDir_;
    CutsceneAsset cutscene_;
    bool haveCutscene_ = false;
    cine::Sequence sequence_;          // offline .hbseq render (sibling to cutscene_)
    cine::SequenceInstance seqInstance_;
    bool haveSequence_ = false;
    std::string sceneSnapshot_;
    int warmLeft_ = 0;
    int totalFrames_ = 0;
    int frameIndex_ = 0; // next frame to CAPTURE
    bool posed_ = false; // a frame was posed last tick (ready to read this tick)
    f32 t_ = 0.0f, prevT_ = 0.0f;
    f32 progress_ = 0.0f;
    u32 framesWritten_ = 0;
    bool cancel_ = false;
    std::string status_;
    Mp4Writer mp4_;     // used when cfg_.outputFile is set
    bool useMp4_ = false;
};

} // namespace movie
} // namespace hbe

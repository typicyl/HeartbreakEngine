// Editor/MovieRender.cpp
#include "Editor/MovieRender.h"

#include "Assets/UAF.h"
#include "Cinematics/SequenceAsset.h"
#include "Cinematics/TrackRegistry.h"
#include "Core/Log.h"
#include "Engine/CutscenePlayer.h"
#include "Engine/Engine.h"
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <miniaudio.h>
#include <stb_image_write.h>

namespace hbe::movie {

namespace fs = std::filesystem;

namespace {

// Deterministically pose a .hbseq frame for offline capture. Uses the SAME runtime
// evaluator as playback and editor preview (cine::Evaluate/FireEvents), with
// fireDeferred=false so game:: side effects never queue (the game is not running);
// animation clip triggers still fire (they write the Animator directly).
void PoseSequenceFrame(Engine& engine, cine::Sequence& seq, cine::SequenceInstance& inst,
                       const fs::path& assetsDir, f32 prevT, f32 t, bool fire) {
    cine::EvalContext ctx;
    ctx.scene = &engine.GetScene();
    ctx.camera = &engine.GetRenderer().GetCamera();
    ctx.post = &engine.GetScene().Environment().post;
    ctx.assetsDir = assetsDir;
    ctx.mode = cine::EvalMode::Offline;
    ctx.applyCamera = true;
    ctx.fireDeferred = false;
    ctx.applyGameplay = false;
    ctx.t = t;
    ctx.prevT = prevT;
    ctx.dt = t - prevT;
    if (fire) cine::FireEvents(seq, inst, ctx);
    cine::Evaluate(seq, inst, ctx);
}

// Decodes any audio asset to interleaved-stereo int16 at `rate` Hz. A `.uaf` holds
// raw PCM (resampled via miniaudio); anything else (.mp3/.wav/.flac) goes through
// ma_decoder. Returns empty on failure.
std::vector<i16> DecodeAudioStereo(const fs::path& path, u32 rate) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".uaf") {
        const auto a = uaf::ReadAudio(path);
        if (!a || a->pcm.empty()) return {};
        const u32 srcCh = a->channels ? a->channels : 1;
        const u32 srcRate = a->sampleRate ? a->sampleRate : rate;
        const auto* src = reinterpret_cast<const i16*>(a->pcm.data());
        const ma_uint64 srcFrames = a->pcm.size() / (static_cast<usize>(srcCh) * sizeof(i16));
        const ma_uint64 dstFrames =
            static_cast<ma_uint64>(static_cast<double>(srcFrames) * rate / srcRate) + 2;
        std::vector<i16> out(static_cast<usize>(dstFrames) * 2);
        const ma_uint64 wrote = ma_convert_frames(out.data(), dstFrames, ma_format_s16, 2, rate, src,
                                                   srcFrames, ma_format_s16, srcCh, srcRate);
        out.resize(static_cast<usize>(wrote) * 2);
        return out;
    }
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 2, rate);
    ma_decoder dec;
    if (ma_decoder_init_file_w(path.wstring().c_str(), &cfg, &dec) != MA_SUCCESS) return {};
    std::vector<i16> out;
    const ma_uint64 kChunk = 8192;
    std::vector<i16> tmp(static_cast<usize>(kChunk) * 2);
    for (;;) {
        ma_uint64 got = 0;
        const ma_result rr = ma_decoder_read_pcm_frames(&dec, tmp.data(), kChunk, &got);
        if (rr != MA_SUCCESS && rr != MA_AT_END)
            HBE_WARN("MovieRender: audio decode error for '{}'.", path.string());
        if (rr != MA_SUCCESS || got == 0) break;
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<usize>(got) * 2);
        if (got < kChunk) break;
    }
    ma_decoder_uninit(&dec);
    return out;
}

// Mixes the cutscene's dialogue voicelines (at their marker times) + an optional
// music bed into one interleaved-stereo int16 track `duration` seconds long.
std::vector<i16> BuildCutsceneAudio(const fs::path& assetsDir, const CutsceneAsset* cs,
                                    const std::string& musicRel, f32 duration, u32 rate) {
    const usize totalFrames = static_cast<usize>(std::ceil(std::max(0.0f, duration) * rate));
    if (totalFrames == 0) return {};
    std::vector<i32> mix(totalFrames * 2, 0); // accumulate wide to avoid clipping mid-sum
    bool any = false;
    const auto add = [&](const std::vector<i16>& clip, usize startFrame, f32 gain) {
        const usize frames = clip.size() / 2;
        for (usize i = 0; i < frames; ++i) {
            const usize f = startFrame + i;
            if (f >= totalFrames) break;
            mix[f * 2 + 0] += static_cast<i32>(clip[i * 2 + 0] * gain);
            mix[f * 2 + 1] += static_cast<i32>(clip[i * 2 + 1] * gain);
        }
        any = true;
    };
    if (cs) {
        for (const CutsceneDialogueMarker& m : cs->dialogue) {
            if (m.voiceline.empty() || m.time < 0.0f) continue; // guard the unsigned start-frame cast
            const auto clip = DecodeAudioStereo(assetsDir / m.voiceline, rate);
            if (!clip.empty()) add(clip, static_cast<usize>(std::floor(m.time * rate)), 1.0f);
        }
    }
    if (!musicRel.empty()) {
        const auto music = DecodeAudioStereo(assetsDir / musicRel, rate);
        if (!music.empty()) add(music, 0, 0.6f); // sit the bed under the voices
    }
    if (!any) return {};
    std::vector<i16> out(totalFrames * 2);
    for (usize i = 0; i < mix.size(); ++i)
        out[i] = static_cast<i16>(std::clamp(mix[i], -32768, 32767));
    return out;
}

} // namespace

bool WritePng(const fs::path& path, u32 w, u32 h, const std::vector<u8>& rgba) {
    if (w == 0 || h == 0 || rgba.size() < static_cast<usize>(w) * h * 4) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const int ok = stbi_write_png(path.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                  rgba.data(), static_cast<int>(w) * 4);
    if (!ok) HBE_ERROR("MovieRender: failed to write PNG '{}'.", path.string());
    return ok != 0;
}

void MovieJob::Start(Engine& engine, const fs::path& assetsDir, const MovieConfig& cfg) {
    cfg_ = cfg;
    assetsDir_ = assetsDir;
    cancel_ = false;
    framesWritten_ = 0;
    progress_ = 0.0f;
    frameIndex_ = 0;
    posed_ = false;
    t_ = 0.0f;
    prevT_ = 0.0f;
    haveCutscene_ = false;
    haveSequence_ = false;

    f32 dur = cfg.duration;
    if (!cfg.cutsceneRel.empty()) {
        if (auto cs = assets::LoadCutscene(assetsDir / cfg.cutsceneRel)) {
            cutscene_ = *cs;
            haveCutscene_ = true;
            if (cutscene_.duration > 0.0f) dur = cutscene_.duration;
        } else {
            status_ = "Failed to load cutscene '" + cfg.cutsceneRel + "'.";
            phase_ = Phase::Done;
            return;
        }
    }
    // A .hbseq takes precedence over a cutscene (the Sequencer supersedes it).
    if (!cfg.sequenceRel.empty()) {
        cine::RegisterBuiltinTrackKinds();
        if (auto s = cine::LoadSequence(assetsDir / cfg.sequenceRel)) {
            sequence_ = std::move(*s);
            seqInstance_ = cine::SequenceInstance{};
            haveSequence_ = true;
            haveCutscene_ = false;
            if (sequence_.Length() > 0.0f) dur = sequence_.Length();
        } else {
            status_ = "Failed to load sequence '" + cfg.sequenceRel + "'.";
            phase_ = Phase::Done;
            return;
        }
    }
    totalFrames_ = std::max(1, static_cast<int>(std::lround(dur * cfg.fps)));

    // Snapshot the scene so the caller can restore after the render (Evaluate poses
    // entity transforms; the editor scene must be left as authored).
    sceneSnapshot_ = scene::SaveSceneToString(engine.GetScene());

    // Output target: an .mp4 (H.264 + AAC) if outputFile is set, else a PNG dir.
    useMp4_ = !cfg.outputFile.empty();
    if (useMp4_) {
        // Offline-mix the audio (cutscene dialogue voicelines + optional music bed)
        // up front - MF must know about the audio stream before writing begins.
        std::vector<i16> audio;
        if (haveCutscene_ || !cfg.musicRel.empty()) {
            audio = BuildCutsceneAudio(assetsDir, haveCutscene_ ? &cutscene_ : nullptr, cfg.musicRel,
                                       dur, 48000);
        }
        if (!mp4_.Begin(cfg.outputFile, cfg.width, cfg.height, cfg.fps, std::move(audio), 48000)) {
            status_ = "Failed to open .mp4 encoder.";
            phase_ = Phase::Done;
            return;
        }
    } else {
        std::error_code ec;
        fs::create_directories(cfg.outputDir, ec);
    }

    const f32 fdt = 1.0f / static_cast<f32>(std::max(1u, cfg.fps));
    engine.SetRenderFixedDt(fdt);
    engine.GetRenderer().SetViewportSize(cfg.width, cfg.height);

    warmLeft_ = static_cast<int>(cfg.warmupFrames);
    phase_ = Phase::WarmUp;
    status_ = "Warming up...";
    HBE_INFO("MovieRender: {} frames at {}x{} {}fps -> {} ({})", totalFrames_, cfg.width, cfg.height,
             cfg.fps, (useMp4_ ? cfg.outputFile : cfg.outputDir).string(),
             useMp4_ ? "mp4" : "png-seq");
}

void MovieJob::Tick(Engine& engine) {
    Renderer& r = engine.GetRenderer();
    Scene& scene = engine.GetScene();
    // Pin the render size + fixed dt each tick (a target realloc / a stray resize
    // elsewhere must not knock the render off its resolution or timing).
    r.SetViewportSize(cfg_.width, cfg_.height);
    engine.SetRenderFixedDt(1.0f / static_cast<f32>(std::max(1u, cfg_.fps)));

    if (phase_ == Phase::WarmUp) {
        if (cancel_) { // respond immediately, don't burn the remaining warm-up frames
            if (useMp4_) mp4_.Finish();
            phase_ = Phase::Done;
            status_ = "Cancelled.";
            return;
        }
        // Hold the timeline at t=0 while TAA history + auto-exposure + particles settle.
        if (haveCutscene_) cutscene::Evaluate(cutscene_, 0.0f, scene, r.GetCamera(), true);
        if (haveSequence_) PoseSequenceFrame(engine, sequence_, seqInstance_, assetsDir_, 0.0f, 0.0f, false);
        if (--warmLeft_ <= 0) {
            phase_ = Phase::Capture;
            posed_ = false;
            frameIndex_ = 0;
            t_ = 0.0f;
            prevT_ = 0.0f;
        }
        return;
    }

    if (phase_ == Phase::Capture) {
        // Pipeline: a frame posed last tick was rendered by RenderScene AFTER this
        // hook ran, so it is only available in the offscreen target THIS tick.
        // 1) read + encode the frame posed on the previous tick.
        if (posed_) {
            std::vector<u8> px;
            u32 pw = 0, ph = 0;
            if (r.ReadbackViewportColor(px, pw, ph)) {
                if (useMp4_) {
                    if (mp4_.WriteVideoFrame(px)) ++framesWritten_;
                } else {
                    char name[64];
                    std::snprintf(name, sizeof(name), "frame_%05d.png", frameIndex_);
                    if (WritePng(cfg_.outputDir / name, pw, ph, px)) ++framesWritten_;
                }
            }
            ++frameIndex_;
            posed_ = false;
        }
        progress_ = totalFrames_ > 0 ? static_cast<f32>(frameIndex_) / totalFrames_ : 1.0f;
        char st[96];
        std::snprintf(st, sizeof(st), "Rendering frame %d / %d", frameIndex_, totalFrames_);
        status_ = st;
        if (cancel_ || frameIndex_ >= totalFrames_) {
            phase_ = Phase::Done;
            if (useMp4_) mp4_.Finish(); // finalize the container
            // Surface dropped frames (a readback/encode/write failure advances the
            // timeline but writes nothing) instead of silently reporting success.
            const u32 done = static_cast<u32>(frameIndex_);
            const u32 dropped = done > framesWritten_ ? done - framesWritten_ : 0;
            if (cancel_) status_ = "Cancelled.";
            else if (dropped > 0) status_ = "Done - WARNING: " + std::to_string(dropped) +
                                            " frame(s) dropped.";
            else status_ = "Done.";
            return;
        }
        // 2) pose the next frame at t = frameIndex/fps (the rest of the sim -
        //    animators, particles, day/night - advances by the fixed dt via the loop).
        prevT_ = t_;
        t_ = static_cast<f32>(frameIndex_) / static_cast<f32>(cfg_.fps);
        if (haveCutscene_) {
            cutscene::FireMarkers(cutscene_, prevT_, t_, scene, false);
            cutscene::Evaluate(cutscene_, t_, scene, r.GetCamera(), true);
        }
        if (haveSequence_) PoseSequenceFrame(engine, sequence_, seqInstance_, assetsDir_, prevT_, t_, true);
        posed_ = true;
    }
}

void MovieJob::Stop(Engine& engine) {
    engine.SetRenderFixedDt(-1.0f);
    if (phase_ != Phase::Done) phase_ = Phase::Done;
}

} // namespace hbe::movie

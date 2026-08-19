// Cinematics/Evaluator.h - the shared deterministic sequence evaluator.
//
// Mirrors the crown-jewel CutscenePlayer contract so editor preview, runtime
// playback and offline capture all run ONE code path and cannot drift:
//   Evaluate()   applies the POSE at ctx.t. Pure/idempotent - safe to scrub.
//   FireEvents() fires events crossed in [ctx.prevT, ctx.t). Non-idempotent, and
//                gated by ctx.fireDeferred (editor preview passes false so game::
//                side effects never queue during a scrub).
#pragma once

#include "Cinematics/Sequence.h"
#include "Cinematics/Binding.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <memory>
#include <unordered_map>

namespace hbe {

class Scene;
class Camera;
namespace rhi { struct PostSettings; }

namespace cine {

// Which execution context an evaluation runs in (spec §16). Preview = editor
// scrub/preview; Runtime = shipped game / play mode; Offline = deterministic
// fixed-dt movie capture.
enum class EvalMode : u8 { Preview = 0, Runtime, Offline };

// The sinks + gates for one evaluation. Any sink pointer may be null (a track that
// needs a missing sink is a no-op). Populated by the caller each frame.
struct EvalContext {
    Scene* scene = nullptr;
    Camera* camera = nullptr;          // the render camera a camera track may drive
    rhi::PostSettings* post = nullptr; // scene post settings (DoF / grade / etc.)
    std::filesystem::path assetsDir;   // for asset-driven tracks + sub-sequences

    EvalMode mode = EvalMode::Preview;
    bool applyCamera = true;   // may a camera track drive the render camera?
    bool fireDeferred = true;  // may FireEvents enqueue game:: side effects?
    bool applyGameplay = true; // may gameplay/spawn tracks mutate gameplay state?

    f32 t = 0.0f;      // absolute sequence time to pose at
    f32 prevT = 0.0f;  // previous time; events fire across [prevT, t)
    f32 weight = 1.0f; // outer blend weight (nested sub-sequences)
    f32 dt = 0.0f;     // frame delta (for systems that need it)

    // Camera-cut resolution (filled by the evaluator's pre-pass each Evaluate).
    bool hasCameraCut = false;
    int activeCameraBinding = -1; // which Binding's camera is live at ctx.t

    // Outputs the caller reads back.
    bool cameraDriven = false; // a camera/cameraCut track claimed the render camera
};

// Per-run mutable state for a playing sequence (distinct from the immutable
// Sequence asset). Holds the binding cache, spawnables, playhead, and nested
// shot/sub-sequence instances.
struct SequenceInstance {
    BindingResolver bindings;
    f32 time = 0.0f;
    f32 prevTime = 0.0f;
    bool started = false;

    // Sub-sequence (shot) support: child instances by shot id, loaded sub-sequences
    // by relative path (cached for the instance's lifetime).
    std::unordered_map<u64, std::unique_ptr<SequenceInstance>> children;
    std::unordered_map<std::string, std::shared_ptr<Sequence>> subCache;

    // Visibility ledger: entities this instance hid (to restore on release).
    std::vector<entt::entity> hidden;

    // Releases spawnables + restores hidden entities. Call when the sequence stops.
    void Release(Scene& scene);
};

// Apply the pose at ctx.t (idempotent). Also runs the camera-cut pre-pass.
void Evaluate(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx);

// Fire events crossed in [ctx.prevT, ctx.t). Non-idempotent; only while advancing.
void FireEvents(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx);

// -- Helpers used by track kinds (Tracks.cpp) --------------------------------
// The latest-starting section covering track-local time `t` with weight > 0, or
// nullptr. (Single-value tracks use the top section; layered tracks iterate.)
const Section* ActiveSection(const Track& tr, f32 t);
// Evaluate a named scalar channel at content time; returns `fallback` if absent.
f32 SampleChannel(const Section& s, const char* target, f32 contentTime, f32 fallback);
bool HasChannel(const Section& s, const char* target);
// Rotation from a section at content time: quaternion keys (slerp) win, else three
// euler channels ("rotation.x/y/z", degrees), else `fallback`.
glm::quat SampleRotation(const Section& s, f32 contentTime, glm::quat fallback);

} // namespace cine
} // namespace hbe

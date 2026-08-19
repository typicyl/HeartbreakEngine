// Cinematics/Evaluator.cpp - the shared deterministic sequence evaluator.
#include "Cinematics/Evaluator.h"

#include "Cinematics/TrackRegistry.h"
#include "Cinematics/SequenceAsset.h"
#include "Core/Log.h"
#include "Scene/Components.h" // Hidden
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <unordered_set>

namespace hbe::cine {
namespace {

enum class Pass { Pose, Fire };

void WarnUnknownKindOnce(const std::string& kind) {
    static std::unordered_set<std::string> warned;
    if (warned.insert(kind).second) {
        HBE_WARN("Cinematics: unknown track kind '{}' - skipped (register it or update the asset)", kind);
    }
}

bool AnySoloRec(const std::vector<Track>& tracks) {
    for (const auto& t : tracks) {
        if (t.solo) return true;
        if (AnySoloRec(t.children)) return true;
    }
    return false;
}

void ResolveCameraCutRec(const Track& tr, f32 t, bool& has, int& binding, f32& bestStart) {
    if (!tr.mute && tr.kind == "cameraCut") {
        const Section* s = ActiveSection(tr, t);
        if (s && s->start >= bestStart) {
            has = true;
            binding = s->bindingRef;
            bestStart = s->start;
        }
    }
    for (const auto& c : tr.children) ResolveCameraCutRec(c, t, has, binding, bestStart);
}

void ResolveCameraCut(const Sequence& seq, EvalContext& ctx) {
    ctx.hasCameraCut = false;
    ctx.activeCameraBinding = -1;
    f32 best = -1e30f;
    for (const auto& tr : seq.tracks) ResolveCameraCutRec(tr, ctx.t, ctx.hasCameraCut, ctx.activeCameraBinding, best);
}

void WalkTrack(const Sequence& seq, const Track& tr, SequenceInstance& inst,
               EvalContext& ctx, bool soloActive, Pass pass, bool inSoloScope) {
    if (tr.mute) return; // muting a group mutes its children
    const bool scoped = inSoloScope || tr.solo;

    if (!soloActive || scoped) {
        const TrackKind* kind = FindTrackKind(tr.kind);
        if (kind) {
            const TrackEvalFn fn = (pass == Pass::Pose) ? kind->evaluate : kind->fireEvents;
            if (fn) {
                entt::entity target = entt::null;
                if (tr.binding >= 0 && ctx.scene) {
                    target = inst.bindings.Resolve(seq, tr.binding, *ctx.scene, ctx.assetsDir);
                }
                TrackEvalArgs args{tr, target, ctx, inst};
                fn(args);
            }
        } else if (!tr.kind.empty()) {
            WarnUnknownKindOnce(tr.kind);
        }
    }
    // Always recurse so a solo descendant inside a non-scoped group is still reached.
    for (const auto& c : tr.children) WalkTrack(seq, c, inst, ctx, soloActive, pass, scoped);
}

std::shared_ptr<Sequence> LoadSub(SequenceInstance& inst, EvalContext& ctx, const std::string& rel) {
    auto it = inst.subCache.find(rel);
    if (it != inst.subCache.end()) return it->second;
    std::shared_ptr<Sequence> sub;
    if (auto loaded = LoadSequence(ctx.assetsDir / rel)) {
        sub = std::make_shared<Sequence>(std::move(*loaded));
    }
    inst.subCache[rel] = sub; // cache even nullptr so a missing file is not re-read every frame
    return sub;
}

void Walk(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx, Pass pass, int depth);

void WalkShots(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx, Pass pass, int depth) {
    if (depth > 8) return; // cycle / deep-nest guard
    for (const auto& shot : seq.shots) {
        if (!shot.enabled || shot.sequence.empty()) continue;
        if (ctx.t < shot.start || ctx.t > shot.End()) continue;
        auto sub = LoadSub(inst, ctx, shot.sequence);
        if (!sub) continue;

        auto& child = inst.children[shot.id];
        if (!child) child = std::make_unique<SequenceInstance>();

        EvalContext cctx = ctx;
        cctx.t = (ctx.t - shot.start) * shot.timeScale + shot.innerStart;
        cctx.prevT = (ctx.prevT - shot.start) * shot.timeScale + shot.innerStart;
        cctx.cameraDriven = false;
        Walk(*sub, *child, cctx, pass, depth + 1);
        if (cctx.cameraDriven) ctx.cameraDriven = true;
    }
}

void Walk(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx, Pass pass, int depth) {
    if (pass == Pass::Pose) ResolveCameraCut(seq, ctx);
    const bool solo = AnySoloRec(seq.tracks);
    for (const auto& tr : seq.tracks) WalkTrack(seq, tr, inst, ctx, solo, pass, false);
    WalkShots(seq, inst, ctx, pass, depth);
}

} // namespace

// ---------------------------------------------------------------------------
void Evaluate(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx) {
    Walk(seq, inst, ctx, Pass::Pose, 0);
}

void FireEvents(const Sequence& seq, SequenceInstance& inst, EvalContext& ctx) {
    Walk(seq, inst, ctx, Pass::Fire, 0);
}

// ---------------------------------------------------------------------------
const Section* ActiveSection(const Track& tr, f32 t) {
    const Section* best = nullptr;
    for (const auto& s : tr.sections) {
        if (t >= s.start && t <= s.End() && s.Weight(t) > 0.0f) {
            if (!best || s.start > best->start) best = &s;
        }
    }
    return best;
}

bool HasChannel(const Section& s, const char* target) {
    for (const auto& ch : s.channels)
        if (ch.target == target) return true;
    return false;
}

f32 SampleChannel(const Section& s, const char* target, f32 contentTime, f32 fallback) {
    for (const auto& ch : s.channels)
        if (ch.target == target) return curve::Evaluate(ch.curve, contentTime);
    return fallback;
}

glm::quat SampleRotation(const Section& s, f32 contentTime, glm::quat fallback) {
    if (!s.rotationKeys.empty()) {
        const auto& k = s.rotationKeys;
        if (contentTime <= k.front().time) return glm::normalize(k.front().value);
        if (contentTime >= k.back().time) return glm::normalize(k.back().value);
        for (usize i = 0; i + 1 < k.size(); ++i) {
            if (contentTime >= k[i].time && contentTime <= k[i + 1].time) {
                const f32 span = k[i + 1].time - k[i].time;
                f32 u = (span > 1e-6f) ? (contentTime - k[i].time) / span : 0.0f;
                u = ease::Ease(k[i + 1].ease, u);
                return glm::normalize(glm::slerp(k[i].value, k[i + 1].value, u));
            }
        }
        return glm::normalize(k.back().value);
    }
    if (HasChannel(s, "rotation.x") || HasChannel(s, "rotation.y") || HasChannel(s, "rotation.z")) {
        const glm::vec3 e{glm::radians(SampleChannel(s, "rotation.x", contentTime, 0.0f)),
                          glm::radians(SampleChannel(s, "rotation.y", contentTime, 0.0f)),
                          glm::radians(SampleChannel(s, "rotation.z", contentTime, 0.0f))};
        return glm::normalize(glm::quat(e));
    }
    return fallback;
}

// ---------------------------------------------------------------------------
void SequenceInstance::Release(Scene& scene) {
    bindings.ReleaseSpawnables(scene);
    auto& reg = scene.Registry();
    for (entt::entity e : hidden) {
        if (reg.valid(e) && reg.all_of<Hidden>(e)) reg.remove<Hidden>(e);
    }
    hidden.clear();
    for (auto& [id, child] : children) {
        (void)id;
        if (child) child->Release(scene);
    }
    children.clear();
    subCache.clear();
    time = prevTime = 0.0f;
    started = false;
}

} // namespace hbe::cine

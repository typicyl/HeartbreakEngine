// Cinematics/Tracks.cpp - the built-in cinematic track kinds.
//
// Each kind is a small free function registered into the TrackRegistry. Two flavours:
//   * DIRECT-WRITE tracks (camera/transform/animation/visibility/light/post) pose the
//     scene through the existing components - safe in editor preview (they animate the
//     scrub) and idempotent where they can be.
//   * DEFERRED-EVENT tracks (audio/music/subtitle/dialogue/shake/event/spawn) enqueue
//     existing game:: verbs on section-cross, gated by ctx.fireDeferred so an editor
//     scrub never queues gameplay side effects (the CutscenePlayer safeguard).
//
// Nothing here re-implements playback: it drives Camera / Animator / light components /
// PostSettings / the game:: command queue that already exist.
#include "Cinematics/Evaluator.h"
#include "Cinematics/TrackRegistry.h"

#include "Core/Log.h"
#include "Game/GameSystems.h"
#include "RHI/RHI.h"           // rhi::PostSettings
#include "Renderer/Camera.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/norm.hpp>

namespace hbe::cine {
namespace {

// ---- section param access ----------------------------------------------------
f32 PF(const Section& s, const char* key, f32 def) {
    for (const auto& p : s.floatParams)
        if (p.first == key) return p.second;
    return def;
}
const std::string& PS(const Section& s, const char* key, const std::string& def) {
    for (const auto& p : s.stringParams)
        if (p.first == key) return p.second;
    return def;
}

template <class F>
void ForEachTriggered(const Track& tr, const EvalContext& ctx, F fn) {
    for (const auto& s : tr.sections) {
        if (s.start >= ctx.prevT && s.start < ctx.t) fn(s);
    }
}

glm::vec3 WorldPos(Scene& scene, const std::string& name) {
    entt::entity e = scene.FindByName(name);
    if (e == entt::null) return glm::vec3(0.0f);
    return glm::vec3(scene.WorldMatrix(e)[3]);
}

// ============================ CAMERA =========================================
void EvalCamera(const TrackEvalArgs& a) {
    EvalContext& ctx = a.ctx;
    if (!ctx.applyCamera || !ctx.camera || !ctx.scene) return;
    // Camera-cut arbitration: when a cut track exists, only the live camera applies.
    if (ctx.hasCameraCut && a.track.binding != ctx.activeCameraBinding) return;

    const Section* s = ActiveSection(a.track, ctx.t);
    if (!s) return;
    const f32 ct = s->ContentTime(ctx.t);
    const f32 w = s->Weight(ctx.t);

    Camera& cam = *ctx.camera;
    glm::vec3 eye = cam.Position();
    eye.x = SampleChannel(*s, "location.x", ct, eye.x);
    eye.y = SampleChannel(*s, "location.y", ct, eye.y);
    eye.z = SampleChannel(*s, "location.z", ct, eye.z);

    // Aim: a tracked entity wins; else the aim channels; else look forward.
    glm::vec3 target;
    const std::string& aimTarget = PS(*s, "aimTarget", std::string());
    if (!aimTarget.empty()) {
        target = WorldPos(*ctx.scene, aimTarget);
    } else {
        target = cam.Position() + cam.Forward();
        target.x = SampleChannel(*s, "aim.x", ct, target.x);
        target.y = SampleChannel(*s, "aim.y", ct, target.y);
        target.z = SampleChannel(*s, "aim.z", ct, target.z);
    }

    f32 fov = SampleChannel(*s, "fov", ct, glm::degrees(cam.FovY()));
    const f32 roll = SampleChannel(*s, "roll", ct, 0.0f);

    // Blend against the camera's current pose during the edge ramp.
    if (w < 0.999f) {
        const glm::vec3 curEye = cam.Position();
        const glm::vec3 curTarget = cam.Position() + cam.Forward();
        eye = glm::mix(curEye, eye, w);
        target = glm::mix(curTarget, target, w);
        fov = glm::mix(glm::degrees(cam.FovY()), fov, w);
    }

    glm::vec3 up{0.0f, 1.0f, 0.0f};
    if (std::fabs(roll) > 1e-4f && glm::length2(target - eye) > 1e-8f) {
        const glm::vec3 fwd = glm::normalize(target - eye);
        up = glm::angleAxis(glm::radians(roll), fwd) * up;
    }

    cam.SetFovY(fov);
    cam.LookAt(eye, target, up);
    ctx.cameraDriven = true;

    // Depth of field / rack focus -> caller-owned PostSettings (spec §4/§11).
    if (ctx.post && (HasChannel(*s, "focusDistance") || HasChannel(*s, "aperture"))) {
        f32 focus = SampleChannel(*s, "focusDistance", ct, -1.0f);
        if (focus <= 0.0f) focus = glm::length(target - eye); // auto = focus on the aim
        ctx.post->dofEnabled = 1;
        ctx.post->dofFocusDistance = focus;
        ctx.post->dofFocusRange = SampleChannel(*s, "focusRange", ct, ctx.post->dofFocusRange);
        if (HasChannel(*s, "aperture")) {
            const f32 ap = SampleChannel(*s, "aperture", ct, 0.0f);
            if (ap > 0.0f) ctx.post->dofMaxBlur = ap;
        }
    }
}

// ============================ TRANSFORM ======================================
void EvalTransform(const TrackEvalArgs& a) {
    if (a.target == entt::null || !a.ctx.scene) return;
    Transform* tf = a.ctx.scene->Registry().try_get<Transform>(a.target);
    if (!tf) return;
    const Section* s = ActiveSection(a.track, a.ctx.t);
    if (!s) return;
    const f32 ct = s->ContentTime(a.ctx.t);
    const f32 w = s->Weight(a.ctx.t);

    glm::vec3 pos = tf->position;
    pos.x = SampleChannel(*s, "location.x", ct, pos.x);
    pos.y = SampleChannel(*s, "location.y", ct, pos.y);
    pos.z = SampleChannel(*s, "location.z", ct, pos.z);

    glm::vec3 scl = tf->scale;
    scl.x = SampleChannel(*s, "scale.x", ct, scl.x);
    scl.y = SampleChannel(*s, "scale.y", ct, scl.y);
    scl.z = SampleChannel(*s, "scale.z", ct, scl.z);

    const glm::quat rot = SampleRotation(*s, ct, tf->rotation);

    tf->position = glm::mix(tf->position, pos, w);
    tf->scale = glm::mix(tf->scale, scl, w);
    tf->rotation = glm::normalize(glm::slerp(tf->rotation, rot, w));
}

// ============================ ANIMATION ======================================
// Clip TRIGGER on section cross (mirrors the cutscene clip markers): restart the
// target's Animator on the new clip; the runtime animation system poses it.
void FireAnimation(const TrackEvalArgs& a) {
    if (a.target == entt::null || !a.ctx.scene) return;
    Animator* an = a.ctx.scene->Registry().try_get<Animator>(a.target);
    if (!an) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        an->clip = static_cast<int>(PF(s, "clip", static_cast<f32>(an->clip)));
        an->time = 0.0f;
        an->speed = PF(s, "speed", 1.0f);
        an->loop = s.loop;
        an->playing = true;
        if (!s.assetRef.empty()) an->sourceAsset = s.assetRef; // retarget source
    });
}

// ============================ VISIBILITY =====================================
void EvalVisibility(const TrackEvalArgs& a) {
    if (a.target == entt::null || !a.ctx.scene) return;
    auto& reg = a.ctx.scene->Registry();
    const Section* s = ActiveSection(a.track, a.ctx.t);
    // A section with "visible"==0 hides the entity for its range (default hide).
    const bool hide = s && PF(*s, "visible", 0.0f) < 0.5f;
    if (hide) {
        if (!reg.all_of<Hidden>(a.target)) {
            reg.emplace<Hidden>(a.target);
            a.inst.hidden.push_back(a.target);
        }
    } else if (reg.all_of<Hidden>(a.target)) {
        reg.remove<Hidden>(a.target);
    }
}

// ============================ LIGHT ==========================================
void EvalLight(const TrackEvalArgs& a) {
    if (a.target == entt::null || !a.ctx.scene) return;
    auto& reg = a.ctx.scene->Registry();
    const Section* s = ActiveSection(a.track, a.ctx.t);
    if (!s) return;
    const f32 ct = s->ContentTime(a.ctx.t);
    const f32 w = s->Weight(a.ctx.t);

    auto apply = [&](auto& L) {
        if (HasChannel(*s, "intensity")) {
            L.intensity = glm::mix(L.intensity, SampleChannel(*s, "intensity", ct, L.intensity), w);
        }
        glm::vec3 c = L.color;
        c.r = SampleChannel(*s, "color.r", ct, c.r);
        c.g = SampleChannel(*s, "color.g", ct, c.g);
        c.b = SampleChannel(*s, "color.b", ct, c.b);
        L.color = glm::mix(L.color, c, w);
    };
    if (auto* d = reg.try_get<DirectionalLightComponent>(a.target)) apply(*d);
    if (auto* p = reg.try_get<PointLightComponent>(a.target)) apply(*p);
    if (auto* sp = reg.try_get<SpotLightComponent>(a.target)) apply(*sp);
    if (auto* r = reg.try_get<RectLightComponent>(a.target)) apply(*r);
}

// ============================ POST ===========================================
void EvalPost(const TrackEvalArgs& a) {
    EvalContext& ctx = a.ctx;
    if (!ctx.post) return;
    const Section* s = ActiveSection(a.track, ctx.t);
    if (!s) return;
    const f32 ct = s->ContentTime(ctx.t);
    const f32 w = s->Weight(ctx.t);
    auto set = [&](const char* name, f32& field) {
        if (HasChannel(*s, name)) field = glm::mix(field, SampleChannel(*s, name, ct, field), w);
    };
    set("bloomIntensity", ctx.post->bloomIntensity);
    set("bloomThreshold", ctx.post->bloomThreshold);
    set("vignette", ctx.post->vignette);
    set("saturation", ctx.post->saturation);
    set("contrast", ctx.post->contrast);
    set("gradeTemperature", ctx.post->gradeTemperature);
    set("gradeTint", ctx.post->gradeTint);
    set("filmGrain", ctx.post->filmGrain);
    set("chromaticAberration", ctx.post->chromaticAberration);
    set("dofFocusDistance", ctx.post->dofFocusDistance);
    set("dofFocusRange", ctx.post->dofFocusRange);
    set("dofMaxBlur", ctx.post->dofMaxBlur);
    set("motionBlurIntensity", ctx.post->motionBlurIntensity);
    set("fogDensity", ctx.post->fogDensity);
    set("ssgiIntensity", ctx.post->ssgiIntensity);
    // Fog colour (vec3).
    ctx.post->fogColor.r = glm::mix(ctx.post->fogColor.r, SampleChannel(*s, "fogColor.r", ct, ctx.post->fogColor.r), w);
    ctx.post->fogColor.g = glm::mix(ctx.post->fogColor.g, SampleChannel(*s, "fogColor.g", ct, ctx.post->fogColor.g), w);
    ctx.post->fogColor.b = glm::mix(ctx.post->fogColor.b, SampleChannel(*s, "fogColor.b", ct, ctx.post->fogColor.b), w);
}

// ============================ PROPERTY (generic scalar) ======================
// A generic single-scalar driver: the track's "property" string selects a target,
// the section's "value" channel supplies the animated value. Extensible dispatch;
// v1 covers common component scalars without reflection (spec §11).
void EvalProperty(const TrackEvalArgs& a) {
    if (!a.ctx.scene) return;
    const Section* s = ActiveSection(a.track, a.ctx.t);
    if (!s) return;
    const f32 ct = s->ContentTime(a.ctx.t);
    const f32 w = s->Weight(a.ctx.t);
    const std::string& prop = a.track.StringParam("property", std::string());
    if (prop.empty() || !HasChannel(*s, "value")) return;
    auto& reg = a.ctx.scene->Registry();

    auto blend = [&](f32& field) { field = glm::mix(field, SampleChannel(*s, "value", ct, field), w); };

    if (a.target != entt::null) {
        if (prop == "pointLight.range") { if (auto* p = reg.try_get<PointLightComponent>(a.target)) blend(p->range); }
        else if (prop == "spotLight.range") { if (auto* p = reg.try_get<SpotLightComponent>(a.target)) blend(p->range); }
        else if (prop == "spotLight.innerAngle") { if (auto* p = reg.try_get<SpotLightComponent>(a.target)) blend(p->innerAngle); }
        else if (prop == "spotLight.outerAngle") { if (auto* p = reg.try_get<SpotLightComponent>(a.target)) blend(p->outerAngle); }
        else if (prop == "audioSource.volume") { if (auto* s2 = reg.try_get<AudioSource>(a.target)) blend(s2->volume); }
        else if (prop == "camera.fovY") { if (auto* c = reg.try_get<CameraComponent>(a.target)) blend(c->fovY); }
    }
}

// ============================ DEFERRED-EVENT tracks ==========================
void FireEvent(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return; // editor preview must not queue gameplay effects
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        const std::string& ev = PS(s, "event", std::string());
        if (ev == "SetFlag") game::SetFlag(PS(s, "flag", std::string()), PF(s, "value", 1.0f));
        else if (ev == "SetObjective") game::SetObjective(PS(s, "id", std::string()), PS(s, "text", std::string()));
        else if (ev == "CompleteObjective") game::CompleteObjective(PS(s, "id", std::string()));
        else if (ev == "ReachCheckpoint") game::ReachCheckpoint(PS(s, "id", std::string()), PF(s, "save", 1.0f) > 0.5f);
        else if (ev == "PlayStinger") game::PlayStinger(s.assetRef.empty() ? PS(s, "asset", std::string()) : s.assetRef);
        else if (!ev.empty()) HBE_WARN("Cinematics: event track: unknown event '{}'", ev);
    });
}

void FireAudio(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        if (!s.assetRef.empty()) game::PlayVoiceline(s.assetRef); // captioned .uaf voice/one-shot
    });
}

void FireMusic(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        const std::string& state = PS(s, "state", std::string());
        if (!state.empty()) game::SetMusicState(state);
        const std::string& param = PS(s, "parameter", std::string());
        if (!param.empty()) game::SetMusicParameter(param, PF(s, "value", 1.0f));
        if (!s.assetRef.empty()) game::PlayStinger(s.assetRef);
    });
}

void FireSubtitle(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        game::QueueSubtitle(PS(s, "speaker", std::string()), PS(s, "text", std::string()),
                            s.duration > 0.0f ? s.duration : PF(s, "duration", 0.0f));
    });
}

void FireDialogue(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        if (!s.assetRef.empty()) game::PlayDialogue(s.assetRef);
    });
}

void FireShake(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred) return;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        game::QueueCameraShake(glm::clamp(PF(s, "trauma", 0.5f), 0.0f, 1.0f));
    });
}

void FireSpawn(const TrackEvalArgs& a) {
    if (!a.ctx.fireDeferred || !a.ctx.applyGameplay || !a.ctx.scene) return;
    const std::string emptyStr;
    ForEachTriggered(a.track, a.ctx, [&](const Section& s) {
        const std::string& id = PS(s, "spawner", emptyStr);
        const std::string& op = PS(s, "op", emptyStr);
        if (id.empty()) return;
        auto& reg = a.ctx.scene->Registry();
        for (entt::entity e : reg.view<Spawner>()) {
            Spawner& sp = reg.get<Spawner>(e);
            if (sp.spawnerId != id) continue;
            if (op == "despawn") sp.despawnRequested = true;
            else sp.spawnRequested = true;
        }
    });
}

} // namespace

// Registration (called once from RegisterBuiltinTrackKinds in TrackRegistry.cpp).
void RegisterBuiltinTrackKindsImpl() {
    auto reg = [](const char* id, const char* disp, const char* cat, bool needsBinding,
                  bool drivesCamera, bool isEvent, TrackEvalFn ev, TrackEvalFn fire) {
        TrackKind k;
        k.id = id; k.display = disp; k.category = cat;
        k.needsBinding = needsBinding; k.drivesCamera = drivesCamera; k.isEvent = isEvent;
        k.evaluate = ev; k.fireEvents = fire;
        RegisterTrackKind(k);
    };

    // Direct-write (pose) tracks.
    reg("camera",     "Camera",      "Camera",    true,  true,  false, &EvalCamera,     nullptr);
    reg("cameraCut",  "Camera Cut",  "Camera",    false, true,  false, nullptr,         nullptr); // handled by the evaluator pre-pass
    reg("transform",  "Transform",   "Animation", true,  false, false, &EvalTransform,  nullptr);
    reg("animation",  "Animation",   "Animation", true,  false, false, nullptr,         &FireAnimation);
    reg("visibility", "Visibility",  "Animation", true,  false, false, &EvalVisibility, nullptr);
    reg("light",      "Lighting",    "Scene",     true,  false, false, &EvalLight,      nullptr);
    reg("post",       "Post Process","Scene",     false, false, false, &EvalPost,       nullptr);
    reg("property",   "Property",    "Property",  true,  false, false, &EvalProperty,   nullptr);

    // Deferred-event tracks.
    reg("event",    "Event",    "Event", false, false, true, nullptr, &FireEvent);
    reg("audio",    "Audio",    "Audio", false, false, true, nullptr, &FireAudio);
    reg("music",    "Music",    "Audio", false, false, true, nullptr, &FireMusic);
    reg("subtitle", "Subtitle", "Audio", false, false, true, nullptr, &FireSubtitle);
    reg("dialogue", "Dialogue", "Audio", false, false, true, nullptr, &FireDialogue);
    reg("shake",    "Camera Shake", "Camera", false, false, true, nullptr, &FireShake);
    reg("spawn",    "Spawn/Destroy", "Gameplay", false, false, true, nullptr, &FireSpawn);
}

} // namespace hbe::cine

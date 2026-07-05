// Engine/CutscenePlayer.cpp
#include "Engine/CutscenePlayer.h"

#include "Game/GameSystems.h"
#include "Renderer/Camera.h"
#include "Scene/Components.h" // Transform, Name, Animator
#include "Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace hbe::cutscene {

namespace {
// Cutscenes reference entities by Name (stable across scene loads, unlike entt
// ids). Linear scan - cutscene tracks are few, evaluated once per frame.
entt::entity FindEntityByName(Scene& scene, const std::string& name) {
    if (name.empty()) return entt::null;
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<Name>())
        if (reg.get<Name>(e).value == name) return e;
    return entt::null;
}
} // namespace

void Evaluate(const CutsceneAsset& cs, f32 t, Scene& scene, Camera& camera,
              bool applyCamera) {
    auto& reg = scene.Registry();

    // --- Camera track: interpolate eye/aim/fov, honoring hard cuts. -----------
    if (applyCamera && !cs.camera.empty()) {
        const auto& ks = cs.camera;
        glm::vec3 pos = ks.front().position, aim = ks.front().aim;
        f32 fov = ks.front().fov;
        if (t >= ks.back().time) {
            pos = ks.back().position; aim = ks.back().aim; fov = ks.back().fov;
        } else if (t > ks.front().time) {
            usize i = 0;
            while (i + 1 < ks.size() && ks[i + 1].time <= t) ++i; // last key <= t
            const CutsceneCameraKey& a = ks[i];
            const CutsceneCameraKey& b = ks[i + 1];
            if (b.cut) { // hold a's pose until the cut instant (b.time)
                pos = a.position; aim = a.aim; fov = a.fov;
            } else {
                const f32 span = glm::max(b.time - a.time, 1e-4f);
                const f32 u = glm::clamp((t - a.time) / span, 0.0f, 1.0f);
                pos = glm::mix(a.position, b.position, u);
                aim = glm::mix(a.aim, b.aim, u);
                fov = glm::mix(a.fov, b.fov, u);
            }
        }
        camera.SetFovY(fov);
        camera.LookAt(pos, aim, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // --- Animation tracks: transform keyframes (pose only). -------------------
    for (const CutsceneAnimTrack& tr : cs.animTracks) {
        if (tr.keys.empty()) continue;
        const entt::entity e = FindEntityByName(scene, tr.target);
        if (e == entt::null) continue;
        Transform* xf = reg.try_get<Transform>(e);
        if (!xf) continue;
        const auto& ks = tr.keys;
        if (t <= ks.front().time) {
            xf->position = ks.front().position;
            xf->rotation = ks.front().rotation;
            xf->scale = ks.front().scale;
        } else if (t >= ks.back().time) {
            xf->position = ks.back().position;
            xf->rotation = ks.back().rotation;
            xf->scale = ks.back().scale;
        } else {
            usize i = 0;
            while (i + 1 < ks.size() && ks[i + 1].time <= t) ++i;
            const CutsceneTransformKey& a = ks[i];
            const CutsceneTransformKey& b = ks[i + 1];
            const f32 span = glm::max(b.time - a.time, 1e-4f);
            const f32 u = glm::clamp((t - a.time) / span, 0.0f, 1.0f);
            xf->position = glm::mix(a.position, b.position, u);
            xf->rotation = glm::slerp(a.rotation, b.rotation, u);
            xf->scale = glm::mix(a.scale, b.scale, u);
        }
    }
}

void FireMarkers(const CutsceneAsset& cs, f32 prev, f32 t, Scene& scene,
                 bool fireDialogue) {
    auto& reg = scene.Registry();

    // Skeletal-clip triggers crossed [prev, t): restart the target's clip.
    for (const CutsceneAnimTrack& tr : cs.animTracks) {
        if (tr.clips.empty()) continue;
        const entt::entity e = FindEntityByName(scene, tr.target);
        if (e == entt::null) continue;
        Animator* an = reg.try_get<Animator>(e);
        if (!an) continue;
        for (const CutsceneClipMarker& m : tr.clips) {
            if (m.time >= prev && m.time < t) {
                an->clip = m.clip;
                an->time = 0.0f;
                an->playing = true;
            }
        }
    }

    // Dialogue markers crossed [prev, t).
    if (fireDialogue) {
        for (const CutsceneDialogueMarker& m : cs.dialogue) {
            if (m.time >= prev && m.time < t) {
                if (!m.dialogue.empty()) game::PlayDialogue(m.dialogue);
                else if (!m.voiceline.empty()) game::PlayVoiceline(m.voiceline);
            }
        }
    }
}

} // namespace hbe::cutscene

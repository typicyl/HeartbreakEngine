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
// ids). Backed by Scene's hashed name index - this runs per camera key AND per
// animation track every frame a cutscene plays, so a linear scan here cost
// (tracks x named entities) string compares per frame.
entt::entity FindEntityByName(Scene& scene, const std::string& name) {
    return scene.FindByName(name);
}
} // namespace

void Evaluate(const CutsceneAsset& cs, f32 t, Scene& scene, Camera& camera,
              bool applyCamera, FocusState* outFocus) {
    auto& reg = scene.Registry();

    // --- Camera track: interpolate eye/aim/fov/roll/focus, honoring hard cuts.
    if (applyCamera && !cs.camera.empty()) {
        const auto& ks = cs.camera;
        // Resolves a key's aim: a named entity's world position when `aimTarget`
        // is set (so a shot tracks a moving actor), else the static aim point.
        const auto resolveAim = [&](const CutsceneCameraKey& k) {
            if (!k.aimTarget.empty()) {
                const entt::entity e = FindEntityByName(scene, k.aimTarget);
                if (e != entt::null) return glm::vec3(scene.WorldMatrix(e)[3]);
            }
            return k.aim;
        };

        const CutsceneCameraKey* keyA = &ks.front();
        const CutsceneCameraKey* keyB = &ks.front();
        glm::vec3 pos = ks.front().position, aim = resolveAim(ks.front());
        f32 fov = ks.front().fov, roll = ks.front().roll;
        f32 focusDist = ks.front().focusDistance, focusRange = ks.front().focusRange;
        f32 aperture = ks.front().aperture;
        if (t >= ks.back().time) {
            const CutsceneCameraKey& k = ks.back();
            keyA = keyB = &k;
            pos = k.position; aim = resolveAim(k); fov = k.fov; roll = k.roll;
            focusDist = k.focusDistance; focusRange = k.focusRange; aperture = k.aperture;
        } else if (t > ks.front().time) {
            usize i = 0;
            while (i + 1 < ks.size() && ks[i + 1].time <= t) ++i; // last key <= t
            const CutsceneCameraKey& a = ks[i];
            const CutsceneCameraKey& b = ks[i + 1];
            keyA = &a;
            keyB = &b;
            const glm::vec3 aimA = resolveAim(a), aimB = resolveAim(b);
            if (b.cut || b.ease == CutsceneEase::Hold) {
                // Hold a's pose until the cut instant (b.time).
                pos = a.position; aim = aimA; fov = a.fov; roll = a.roll;
                focusDist = a.focusDistance; focusRange = a.focusRange; aperture = a.aperture;
            } else {
                const f32 span = glm::max(b.time - a.time, 1e-4f);
                const f32 raw = glm::clamp((t - a.time) / span, 0.0f, 1.0f);
                // The EASE of the key being moved INTO shapes the segment.
                const f32 u = ApplyEase(b.ease, raw);
                pos = glm::mix(a.position, b.position, u);
                aim = glm::mix(aimA, aimB, u);
                fov = glm::mix(a.fov, b.fov, u);
                roll = glm::mix(a.roll, b.roll, u);
                focusRange = glm::mix(a.focusRange, b.focusRange, u);
                aperture = glm::mix(a.aperture, b.aperture, u);
                // Focus: only blend when BOTH keys are manual. Mixing a manual
                // distance with the -1 "auto" sentinel would rack toward garbage.
                focusDist = (a.focusDistance > 0.0f && b.focusDistance > 0.0f)
                                ? glm::mix(a.focusDistance, b.focusDistance, u)
                                : (u < 0.5f ? a.focusDistance : b.focusDistance);
            }
        }
        (void)keyA;
        (void)keyB;

        // Roll tilts the up vector about the view axis (a dutch angle).
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (roll != 0.0f) {
            glm::vec3 fwd = aim - pos;
            if (glm::length(fwd) > 1e-5f) {
                fwd = glm::normalize(fwd);
                up = glm::normalize(glm::vec3(
                    glm::rotate(glm::mat4(1.0f), glm::radians(roll), fwd) * glm::vec4(up, 0.0f)));
            }
        }
        camera.SetFovY(fov);
        camera.LookAt(pos, aim, up);

        // Publish the focus plane so the caller can drive depth of field. Auto
        // (<=0) focuses on the aim point, which is what a focus puller does.
        if (outFocus) {
            outFocus->valid = true;
            outFocus->distance =
                focusDist > 0.0f ? focusDist : glm::length(aim - pos);
            outFocus->range = glm::max(focusRange, 0.01f);
            outFocus->aperture = aperture;
        }
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
        // Standalone subtitles (narration / signage with no voice clip behind
        // them). Queued like dialogue so the engine's one subtitle stack owns
        // formatting and the accessibility gate.
        for (const CutsceneSubtitleMarker& m : cs.subtitles) {
            if (m.time >= prev && m.time < t && !m.text.empty())
                game::QueueSubtitle(m.speaker, m.text, m.duration);
        }
    }

    // Shake impulses crossed [prev, t). Queued rather than applied so the same
    // trauma reaches whichever camera state is live (game rig or preview).
    for (const CutsceneShakeMarker& m : cs.shakes) {
        if (m.time >= prev && m.time < t) game::QueueCameraShake(m.trauma);
    }
}

} // namespace hbe::cutscene

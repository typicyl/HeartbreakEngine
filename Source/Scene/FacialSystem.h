// Scene/FacialSystem.h - facial animation driver (lip-sync + blink + expression).
//
// facial::Update runs each frame after the skeletal pose: for every FacialAnimator
// it advances an amplitude-envelope lip-sync (drives the jaw morph), a timed
// eye-blink, and an expression preset, writing the result into the target's
// MorphState.weights (which the renderer accumulates on the GPU before skinning).
// facial::StartLipSync is called when a voiceline/dialogue line begins so the mouth
// tracks the speech amplitude. Expression presets come from a .hbface library.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <string>

namespace hbe {

class Scene;
struct MorphState;

namespace facial {

// The MorphState an entity's face driver / schematic nodes should write: the same
// entity's MorphState, else (on a modular Character) the live "head" part's. Null
// if none. Shared by the driver AND the SetMorphWeight schematic node so both hit
// the same target on a modular rig.
MorphState* ResolveMorphTarget(Scene& scene, entt::entity e);

// Per-frame face driver. Cheap when there are no FacialAnimators.
void Update(Scene& scene, f32 dt);

// Begins lip-sync on `actor`: decodes `clip` (rel Assets/) into an amplitude
// envelope and starts playback on the actor's FacialAnimator. No-op if the actor
// has no FacialAnimator. `assetsDir` roots the clip path.
void StartLipSync(Scene& scene, entt::entity actor, const std::filesystem::path& assetsDir,
                  const std::string& clip);

// Drops the cached decoded envelopes (project switch).
void ClearEnvelopeCache();

// Loads a .hbface preset library (presetName -> {targetName: weight}) so
// FacialAnimator.expression / the PlayFacialExpression node can resolve presets.
// Returns the number of presets loaded.
u32 LoadPresetLibrary(const std::filesystem::path& hbface);

} // namespace facial
} // namespace hbe

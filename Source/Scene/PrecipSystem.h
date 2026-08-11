// Scene/PrecipSystem.h - weather-driven precipitation (rain / snow) particles.
//
// A runtime, camera-following particle FIELD - not an ECS component and not serialized.
// It is driven entirely by the scene's weather state (SceneEnvironment::precipType /
// precipIntensity / wind): rain falls as camera-projected streaks, snow drifts as soft
// flakes. The field lives in a box centred on the camera and wraps toroidally, so density
// stays constant around the viewer no matter how far they travel.
//
// It rides the EXISTING alpha particle draw: UpdateAndBuild() appends this frame's
// billboards straight into the same std::vector<ParticleVertex> the authored emitters and
// world-text fill, right before Renderer::SetParticles - exactly like ui::AppendWorldText.
// No new render pass, no new pipeline, no serialized entity.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h" // rhi::ParticleVertex

#include <glm/glm.hpp>

#include <vector>

namespace hbe {

class Scene;

namespace precip {

// Persistent per-camera precipitation state. Held by the Engine (like particle::GpuSim)
// so the pool survives across frames. Positions are CAMERA-RELATIVE offsets.
struct PrecipField {
    std::vector<glm::vec3> off;  // camera-relative offset of each drop/flake
    std::vector<f32>       seed; // per-particle random phase (drift + size variation)
    u32 curType = 0;             // precip type the pool is currently seeded for
    f32 time = 0.0f;             // accumulated seconds (snow drift animation)
};

// Advance the field by dt and APPEND this frame's billboards to `alphaOut`. No-op (and
// releases the pool) when precip is off (precipType 0 or intensity ~0). camRight/camUp
// are the same camera billboard basis BuildVertices uses.
void UpdateAndBuild(PrecipField& field, Scene& scene, const glm::vec3& camPos,
                    const glm::vec3& camRight, const glm::vec3& camUp, f32 dt,
                    std::vector<rhi::ParticleVertex>& alphaOut);

} // namespace precip
} // namespace hbe

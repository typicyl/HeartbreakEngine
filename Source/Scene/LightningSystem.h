// Scene/LightningSystem.h - weather-driven lightning flashes.
//
// During a storm (rain + heavy overcast, and the lightning toggle on) this randomly
// triggers lightning strikes: a burst of 1-3 quick flashes that set Scene::LightningFlash
// (read by MakeView to briefly boost scene light / ambient / exposure), followed after a
// distance-based delay by an optional thunder cue. Mirrors weather::Update.
#pragma once

#include "Core/Types.h"

#include <filesystem>

namespace hbe {

class Scene;
class AudioSystem;

namespace lightning {

// Call once per frame (before RenderScene). `audio` may be null (flash-only); thunder only
// plays when SceneEnvironment::thunderSound is set.
void Update(Scene& scene, f32 dt, AudioSystem* audio, const std::filesystem::path& assetsDir);

} // namespace lightning
} // namespace hbe

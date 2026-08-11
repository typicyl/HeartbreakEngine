// Scene/DecalSystem.h - resolves DecalComponent textures to bindless indices.
//
// DecalComponent stores .uaf texture PATHS (so a decal round-trips through the scene
// file). The renderer needs bindless INDICES. Update() loads each decal's albedo/normal/
// metal-rough once and caches the handles on the component (re-resolving only when a path
// changes and the component is flagged unresolved, exactly like ParticleEmitter's sprite).
// Call once per frame before RenderScene, where the renderer + assets dir are in scope.
#pragma once

#include <filesystem>

namespace hbe {

class Scene;
class Renderer;

namespace decal {

void Update(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir);

} // namespace decal
} // namespace hbe

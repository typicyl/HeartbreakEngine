// Scene/TerrainSystem.h - procedural + editable chunked heightfield terrain.
//
// A TerrainComponent owns an editable heightmap; this system materializes it as
// a grid of mesh CHUNKS (child entities tagged TerrainChunk), one drawable per
// chunk so the world breaks into independently cullable pieces. The terrain
// editor sculpts the heightmap with brushes and the affected chunk meshes are
// updated in place (no GPU buffer churn). Chunks are runtime-only (rebuilt from
// the component, never serialized).
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace hbe {

class Scene;
class Renderer;
struct TerrainComponent;

namespace terrain {

// Rebuilds ALL chunk meshes for any TerrainComponent flagged `dirty` (creating
// the chunk child entities). Call each frame on the main thread; a no-op when
// nothing is dirty.
void Update(Scene& scene, Renderer& renderer);

// Ensures the heightmap is sized for the (clamped) params, seeding it
// procedurally (fbm noise) when it is empty or the resolution changed.
void EnsureHeights(TerrainComponent& t);

// Bilinear height sample at terrain-local XZ (from the heightmap when sized,
// else the procedural noise). Used for the editor brush raycast.
f32 SampleHeight(const TerrainComponent& t, f32 x, f32 z);

enum class Brush { Raise, Lower, Smooth, Flatten };

// Sculpts the heightmap around terrain-local (x,z) with a falloff brush, then
// updates the affected chunk meshes in place. `amount` is the per-call delta
// (caller scales by dt). For Flatten, `flattenTarget` is the goal height.
void Sculpt(Scene& scene, Renderer& renderer, entt::entity terrain, f32 localX,
            f32 localZ, f32 radius, f32 amount, Brush brush, f32 flattenTarget);

// Raycasts a terrain-local ray against the heightfield (marched). Returns true
// and the local hit point when the ray meets the surface.
bool RaycastLocal(const TerrainComponent& t, const glm::vec3& localOrigin,
                  const glm::vec3& localDir, glm::vec3& outHit);

// Paints (or erases) the hole mask around terrain-local (x,z) with a falloff brush.
// `erase` restores solid terrain; otherwise carves a hole (clipped in the shader so
// cliff/cave models show through). Flags holeDirty so Update re-uploads holeMaskTex.
void PaintHole(TerrainComponent& t, f32 localX, f32 localZ, f32 radius, bool erase);

// Paints splat layer `layer` (0..3) into the weight mask around terrain-local (x,z):
// the layer's channel rises with the brush falloff and the others fade, so that layer
// takes over there. Flags splatDirty so Update re-uploads splatWeightTex.
void PaintSplat(TerrainComponent& t, f32 localX, f32 localZ, f32 radius, i32 layer);

} // namespace terrain
} // namespace hbe

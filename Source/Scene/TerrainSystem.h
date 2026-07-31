// Scene/TerrainSystem.h - procedural + editable chunked heightfield terrain.
//
// A TerrainComponent owns an editable heightmap; this system materializes it as
// a grid of mesh CHUNKS (child entities tagged TerrainChunk), one drawable per
// chunk so the world breaks into independently cullable pieces. The terrain
// editor sculpts the heightmap with brushes and the affected chunk meshes are
// updated in place (no GPU buffer churn). Chunks are runtime-only (rebuilt from
// the component, never serialized).
//
// COLLISION is NOT per chunk. PhysicsWorld gives the terrain ENTITY a single
// static Jolt HeightFieldShape body built straight off `heights` (see the
// collider handshake on TerrainComponent), and a sculpt stroke updates only the
// sample blocks it touched. Chunks carry no RigidBody.
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

// True when no TerrainComponent still needs a (re)build - every chunk mesh (and
// hole/splat upload) is current. Lets a loading screen wait until terrain has
// fully materialized instead of watching chunks pop in after the screen clears.
bool IsSettled(Scene& scene);

// Ensures the heightmap is sized for the (clamped) params, seeding it
// procedurally (fbm noise) when it is empty or the resolution changed.
void EnsureHeights(TerrainComponent& t);

// Height sample at terrain-local XZ (from the heightmap when sized, else the
// procedural noise). Evaluates the SAME triangle the renderer draws and the
// physics heightfield collides (see the triangulation note below), so the brush
// raycast, the collider and navigation cannot land on three different surfaces.
f32 SampleHeight(const TerrainComponent& t, f32 x, f32 z);

// --- The ONE surface: height + slope + hole, in a single 4-sample read ---------
// THE TRIANGULATION IS LOAD-BEARING. A heightfield quad can be split two ways and
// the two surfaces differ in the quad INTERIOR by the twist (h00+h11-h01-h10);
// only the corner samples agree. Jolt's HeightFieldShape splits on the MAIN
// diagonal (x,z)-(x+1,z+1):
//     tri0 = (x,z), (x,z+1), (x+1,z+1)      [the tz >= tx half]
//     tri1 = (x,z), (x+1,z+1), (x+1,z)      [the tz <= tx half]
// so BuildChunk emits that same split, this function evaluates that same split,
// and MeshPBR.hlsl clips on that same split. Anything that re-derives the surface
// with a bilinear filter (the old SampleHeight) sits half a twist away from what
// the player collides with - an invisible bump, or a step you fall through.
//
// HOLES follow Jolt's rule exactly: a TRIANGLE exists only when all THREE of its
// corner samples are solid (Jolt drops both triangles of a quad when either
// main-diagonal corner is no-collision, and drops one when its third corner is).
// So a hole is never larger in the collider than on screen, and never smaller.
//
// Returns FALSE outside the terrain footprint (+/- ExtentXZ*0.5). Callers depend on
// that: the heightmap is edge-clamped, so sampling past the border would invent an
// infinite skirt of walkable floor. `outDhDx`/`outDhDz` are rise per LOCAL UNIT
// (not per sample step), which is what a world-space slope test needs.
bool SampleSurface(const TerrainComponent& t, f32 localX, f32 localZ, f32& outH,
                   f32& outDhDx, f32& outDhDz, bool& outHole);

// --- Heightfield layout: the ONE definition of where a sample lives ----------
// The terrain is CENTERED on the entity origin, so sample (gx,gz) sits at
// terrain-local
//     (-ExtentXZ*0.5 + gx*SampleStep, heights[gz*GridN() + gx],
//      -ExtentXZ*0.5 + gz*SampleStep).
// The renderer's chunk builder, the brush raycast and the physics heightfield
// collider all derive from these two functions - do not re-derive the mapping.
f32 SampleStep(const TerrainComponent& t); // world units between adjacent samples
f32 ExtentXZ(const TerrainComponent& t);   // total side length (chunks * chunkSize)

// True when `holeMask` is present AND sized for the current grid. Nothing resizes
// the mask when the resolution changes, so a stale mask is routine (the shipped
// reference project carries a 385^2 mask against 641^2 heights). A stale mask must
// be ignored WHOLESALE - partially applying it puts holes in the wrong places, and
// a phantom hole in the collider is something the player falls through.
bool HoleMaskUsable(const TerrainComponent& t);

// Sample-space hole test, edge-clamped. POLARITY: 255 = HOLE, 0 = SOLID (the brush
// and the shader clip agree on this). Always false when !HoleMaskUsable.
bool IsHole(const TerrainComponent& t, i32 gx, i32 gz);

// Grows the pending physics-collider dirty rect by an inclusive sample range.
// Anything that writes `heights` or `holeMask` outside this file must call this,
// or the collider silently keeps the old shape.
void MarkColliderDirty(TerrainComponent& t, i32 x0, i32 z0, i32 x1, i32 z1);

enum class Brush { Raise, Lower, Smooth, Flatten };

// Sculpts the heightmap around terrain-local (x,z) with a falloff brush and marks
// the edited sample rect for the physics collider. `amount` is the per-call delta
// (caller scales by dt). For Flatten, `flattenTarget` is the goal height. Does NOT
// touch chunk meshes - use Sculpt for that (this is the renderer-free core, so a
// headless test can sculpt).
void SculptHeights(TerrainComponent& t, f32 localX, f32 localZ, f32 radius,
                   f32 amount, Brush brush, f32 flattenTarget);

// SculptHeights + an in-place refresh of the chunk meshes overlapping the brush.
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

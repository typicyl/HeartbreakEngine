// Scene/BrushSystem.h - the CSG BLOCKOUT BOX BRUSH system (Unreal-style editable level geometry).
//
// A BrushComponent (Scene/Components.h) is the editable SOURCE: an oriented box that is either
// Additive (adds solid) or Subtractive (carves). This system turns those sources into real,
// rendered, collidable geometry - the exact analogue of terrain::Update turning a heightfield into
// chunk meshes:
//
//   * Each ADDITIVE brush entity gets a generated box mesh = its box MINUS every subtractive brush
//     it overlaps (csg::CarveBox), plus an AABB and a STATIC triangle-mesh RigidBody collider.
//   * SUBTRACTIVE brushes carry no drawable/collider (carving tools only); they keep an AABB so the
//     editor can still pick + gizmo them, and are drawn as a wireframe.
//
// All of it is DERIVED and regenerated from the brushes; nothing here is serialized (BrushComponent
// is, its generated mesh is not), so a scene stays small and reloads reproduce the geometry exactly.
#pragma once

#include "Assets/Mesh.h" // hbe::MeshData
#include "Core/Types.h"

#include <entt/entt.hpp>

namespace hbe {

class Scene;
class Renderer;

namespace brush {

// The generated LOCAL-space mesh for one ADDITIVE brush entity `e`: its box minus every SUBTRACTIVE
// brush in the scene (each transformed into `e`'s local frame). A pure, deterministic function of
// scene state - shared by the geometry rebuild AND the navmesh baker. Returns an empty mesh when `e`
// is not an additive brush, has no BrushComponent, or is fully carved away.
MeshData BuildEntityMesh(const Scene& scene, entt::entity e);

// Rebuild the derived geometry (mesh + AABB + static mesh collision) of every brush when ANY
// BrushComponent is dirty. Mirror of terrain::Update: call each frame on the main thread; a no-op
// when nothing is dirty; requires renderer.SupportsScene() to upload. Clears the dirty flags.
void Update(Scene& scene, Renderer& renderer);

// Mark every brush's geometry stale. Call after any brush add / remove / move / resize: a
// subtractive edit can change any additive brush it overlaps, so global invalidation is the safe,
// simple unit (blockout meshes are tiny).
void MarkAllDirty(Scene& scene);

// True when no brush geometry still needs rebuilding (headless/test convenience).
bool AllClean(const Scene& scene);

} // namespace brush
} // namespace hbe

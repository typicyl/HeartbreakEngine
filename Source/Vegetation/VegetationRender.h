// Vegetation/VegetationRender.h - the CPU-instanced tree render path (P6).
//
// Discrete trees (thousands) render through the EXISTING renderer: each becomes an entt
// entity with Transform + MeshInstance sharing a per-(species) GPU mesh, so the renderer's
// automatic same-material instancing, frustum + shadow culling and distance-LOD selection
// all apply for free - NO new device code. The mass foliage path (10M blades, GPU-expanded)
// is separate (P5). See docs/Design-Vegetation.md section 5.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Vegetation/Species.h"
#include "Vegetation/VegetationInterfaces.h" // PlaceOut / SurfaceQueryFn
#include "RHI/RHI.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace hbe {

class Scene;
class Renderer;

namespace veg {

class VegetationWorld;

// Per-species GPU mesh cache. A representative plant is generated ONCE per species (a
// fixed archetype seed), meshed (woody + foliage submeshes, each with a meshoptimizer LOD
// chain), uploaded, and shared by every instance - so a forest of one species is one GPU
// mesh drawn N times, and respawn never re-uploads.
class VegetationMeshLibrary {
public:
    struct SpeciesMeshes {
        rhi::MeshHandle woody;
        rhi::MeshHandle foliage;
        bool valid = false;
    };

    // Returns (creating on first use) the shared meshes for a species.
    const SpeciesMeshes& GetOrCreate(Renderer& renderer, VegetationWorld& world,
                                     SpeciesId id, const Species& sp);

    // The shared procedural leaf-cluster texture (alpha-cutout silhouette + veined green albedo),
    // generated + uploaded ONCE on first use and reused by every foliage instance. Invalid on a
    // device-less (headless) renderer. Bind it to a foliage MeshInstance's albedoTexture.
    rhi::TextureHandle LeafTexture(Renderer& renderer);

    // Releases every uploaded mesh (base + LODs) + the leaf texture. Call before renderer shutdown.
    void Clear(Renderer& renderer);

    u32 SpeciesCount() const { return static_cast<u32>(byId_.size()); }

private:
    std::unordered_map<u32, SpeciesMeshes> byId_;
    std::vector<rhi::MeshHandle> owned_; // every base + LOD handle, for Clear()
    rhi::TextureHandle leafTex_;         // shared alpha-cutout leaf texture (lazy)
    bool leafTexTried_ = false;          // don't retry a failed/headless upload every call
};

// Spawns tree entities for a set of placements: each becomes a woody entity (+ a foliage
// entity when the species is leafy), sharing the species mesh. Transforms are DETERMINISTIC
// from each placement's per-instance seed (yaw + slight scale) and sit on the ground via
// the surface query. Returns the number of trees spawned.
u32 PopulateForest(Scene& scene, Renderer& renderer, VegetationWorld& world,
                   VegetationMeshLibrary& lib, const PlaceOut& placements,
                   const SurfaceQueryFn& surface);

// PAINT one plant of `species` at a world XZ (the editor vegetation brush, P9). Spawns exactly
// like a scattered tree (woody + foliage entities sharing the species mesh, tagged
// VegetationInstance) via a one-element placement through PopulateForest, so there is a single
// spawn path. `seed` drives the per-instance yaw/scale (vary it per stamp for a natural spread).
// Returns true if a tree was spawned (false if off-terrain / species invalid / mesh failed).
bool PaintTreeAt(Scene& scene, Renderer& renderer, VegetationWorld& world,
                 VegetationMeshLibrary& lib, SpeciesId species, const glm::vec2& worldXZ,
                 u64 seed, const SurfaceQueryFn& surface);

// ERASE every painted/scattered plant (tagged VegetationInstance) whose XZ is within `radius`
// of `worldXZ`. Destroys the entities (their shared species mesh is owned by the library, not
// freed here). Returns the number of entities removed.
u32 EraseVegetationAt(Scene& scene, const glm::vec2& worldXZ, f32 radius);

// Rebuilds the runtime MeshInstance for every VegetationInstance entity that lost its mesh
// handle (e.g. after a scene load): regenerates/uploads the species mesh via `lib` and assigns
// the woody or foliage submesh per the tag. Idempotent - skips entities that already have a
// valid mesh. Returns the number rehydrated. Call after loading a scene that contains painted
// vegetation, once the species are registered in `world`.
u32 RehydrateVegetation(Scene& scene, Renderer& renderer, VegetationWorld& world,
                        VegetationMeshLibrary& lib);

// Registers a couple of built-in paintable species (demo_oak, demo_pine) into the world if not
// already present (idempotent - Add interns by name). Returns the resulting species count. Lets
// the editor paint brush have something to paint without authoring a .hbspecies asset first;
// also the species SpawnDemoForest uses.
u32 RegisterDemoSpecies(VegetationWorld& world);

// One-shot demo (--vegdemo): builds a hilly terrain, interns a couple of species + a biome,
// scatters, and populates a forest so the vegetation vertical slice is visible end-to-end.
// Returns the number of trees spawned.
u32 SpawnDemoForest(Scene& scene, Renderer& renderer, VegetationWorld& world);

// Bakes a species's generated tree to a `.uaf` mesh asset (submesh 0 = woody, 1 = foliage) at
// `outPath`. The "External" authoring workflow: freeze a procedural tree into a fixed asset, then
// a species with `authoredMesh = <that .uaf>` + `strategy = External` replays it as a hand-authored
// mesh (the same slot a proctree import or a Blender export fills). Pure CPU + deterministic (the
// species' fixed archetype seed); no renderer needed. Returns false on generate/mesh/write failure.
bool BakeSpeciesToUaf(VegetationWorld& world, SpeciesId species,
                      const std::filesystem::path& outPath);

// Headless self-test (--test-vegbake): a baked species `.uaf` round-trips (generate -> mesh ->
// WriteMesh -> ReadMesh) with intact geometry. Proves the External-authoring asset path.
bool BakeSelfTest();

// Headless self-test (--test-vegpaint) for the paint brush's renderer-free logic: the
// erase-within-radius geometry (distance^2 selection, collect-then-destroy) and the tag
// isolation (only VegetationInstance entities are removed, never other meshes). Returns true
// on success. The spawn/rehydrate halves need a live device, so they are exercised by the
// --vegdemo smoke path instead.
bool PaintSelfTest();

} // namespace veg
} // namespace hbe

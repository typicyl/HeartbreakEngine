// Vegetation/VegetationSystem.h - the per-frame vegetation tick + self-test entry.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe {

class Scene;

namespace veg {

class VegetationWorld;
struct VegetationStore;

// Simulation-LOD configuration: camera-distance bands for the five tiers, and how many
// trees may be PROMOTED toward higher detail per call (so a camera sweep cannot spike).
struct SimLodConfig {
    f32 heroDist = 8.0f;   // <  -> Hero
    f32 nearDist = 25.0f;  // <  -> Near
    f32 midDist = 60.0f;   // <  -> Mid
    f32 farDist = 120.0f;  // <  -> Far, else Impostor
    u32 promotionBudget = 4;
};

// Assigns each tree in `store` a SimTier from its distance to `camPos` (budgeted promotion,
// immediate demotion). See Vegetation/VegetationSimLod.cpp.
void AssignSimTiers(VegetationStore& store, const glm::vec3& camPos, const SimLodConfig& cfg = {});

// Per-frame vegetation update. Wind + simulation-LOD state advances every frame (it is
// presentational); GROWTH / structural change only advances while `simulating` is true, so
// nondeterministic instance state never advances - or bakes into a `.hbscene` - in the
// editor's edit mode (the edit-mode-determinism law). Walks the resident per-shard stores
// and assigns their simulation tiers from `camPos`.
void Update(VegetationWorld& world, Scene& scene, const glm::vec3& camPos, f32 dt,
            bool simulating);

// --test-vegdata: pins the P1 data model - SoA lockstep + handle round-trip,
// deterministic generation (same seed -> identical skeleton bytes), store add/clear,
// registry interning, and noise-field determinism. Headless: no GPU, no window, no
// project. See Vegetation/VegetationSelfTest.cpp.
bool DataSelfTest();

// --test-vegscatter: pins P2 - DETERMINISTIC placements (same worldSeed -> identical
// points across runs), tileable min-distance, terrain/water filtering via the surface
// query, biome/species scoring, and the .hbbiome round-trip. Headless.
bool ScatterSelfTest();

// --test-vegmesh: pins P3 - the .hbspecies round-trip, the tubular mesher, the
// meshoptimizer LOD chain, deterministic geometry, and sane bounds. Headless.
bool MeshSelfTest();

// --test-veggen: pins P4 - both procedural generators (space colonization + L-system)
// are registered and strategy-selected, deterministic, bounded, structurally distinct,
// and meshable. Headless.
bool GenSelfTest();

// --test-veggrow: pins structural growth - generating the same species+seed at
// increasing ages yields MORE structure (nodes/branches), not a scaled copy, and
// stays deterministic and taller with age. Headless.
bool GrowthSelfTest();

// --test-veglod: pins P7 simulation-LOD - correct distance bands, budgeted promotion (a
// sweep promotes at most `promotionBudget`/call), and immediate demotion. Headless.
bool LodSelfTest();

} // namespace veg
} // namespace hbe

// Vegetation/VegetationInterfaces.h - the library-extensibility seam.
//
// The whole point of these seven interfaces is that a better procedural-generation,
// growth, distribution, noise, wind, simulation or rendering LIBRARY can be plugged in
// LATER as a registered backend WITHOUT rewriting the vegetation core - exactly the way
// meshoptimizer / Jolt / Detour are wrapped so no third-party type ever crosses an
// engine header. The core owns the data model (PlantSkeleton, VegetationStore); a
// backend is a strategy behind one of these pure interfaces.
//
// THREADING CONTRACT: generation / growth / distribution / noise run on WORKER FIBERS
// during streamed generation and MUST be pure CPU, deterministic and free of any
// RHI / registry / tag-table call (those are main-thread only). Wind / simulation run
// in veg::Update on the main thread. Nothing here returns a GPU handle.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Vegetation/Species.h"
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe {

struct SceneEnvironment; // Scene/Scene.h - the wind model reads it (weather/time/heading)

namespace veg {

class SpeciesRegistry; // VegetationWorld.h
class VegetationStore;  // VegetationStore.h

// The environment ONE streamed region presents to a scatter/generation job. Pure data
// so it is safe to hand to a worker fiber. Terrain sampling (height/slope/splat/water)
// is wired in P2 as a surface-query callback carried here; P1 keeps only the bounds +
// seed the distribution needs to be deterministic.
struct VegShardContext {
    u64 worldSeed = 0;         // the world's master seed
    u64 shardId   = 0;         // this shard's stable id (folded into every draw)
    glm::vec2 aabbMin{0.0f};   // world XZ min of the region
    glm::vec2 aabbMax{0.0f};   // world XZ max of the region
    f32 lodDensityScale = 1.0f; // distance-driven density reduction (1 = full)
};

// Candidate placements a distribution backend produces for a shard (SoA).
struct PlaceOut {
    std::vector<glm::vec2> xz;      // world XZ
    std::vector<SpeciesId> species; // chosen species per point
    std::vector<u64>       seed;    // per-instance deterministic seed
    void Clear() { xz.clear(); species.clear(); seed.clear(); }
    u32 Count() const { return static_cast<u32>(xz.size()); }
    void Add(const glm::vec2& p, SpeciesId sp, u64 s) {
        xz.push_back(p); species.push_back(sp); seed.push_back(s);
    }
};

// The biomes overlapping a shard, resolved from the BiomeRegistry.
struct BiomeSet {
    std::vector<Biome> biomes;
    bool Empty() const { return biomes.empty(); }
};

// Parameters for generating ONE plant's structure.
struct PlantGenParams {
    SpeciesId species;
    u64 seed  = 0;    // the per-instance seed (from the distribution)
    f32 age01 = 1.0f; // 0 seedling .. 1 mature
};

// A hierarchical wind sample at a world position/time.
struct WindSample {
    glm::vec3 dir{1.0f, 0.0f, 0.0f}; // unit heading the wind blows toward
    f32 strength   = 0.0f;           // base amplitude
    f32 gust       = 0.0f;           // low-frequency gust contribution
    f32 turbulence = 0.0f;           // high-frequency flutter contribution
};

// ---- 1. Structure generator (owner's IPlantGenerator) ------------------------------
// Pure CPU, job-safe, deterministic: same (params, species) -> byte-identical skeleton
// on every run, thread and compiler.
class IPlantGenerator {
public:
    virtual ~IPlantGenerator() = default;
    virtual const char* Name() const = 0;
    virtual bool Generate(const PlantGenParams& p, const Species& sp, PlantSkeleton& out) = 0;
};

// ---- 2. Growth over time (owner's IPlantGrowthModel) -------------------------------
// Advances a skeleton's STRUCTURE from age a0 to a1 (adds nodes, thickens radii),
// NOT merely scaling a mesh. Low-frequency / offline-capable and deterministic.
class IPlantGrowthModel {
public:
    virtual ~IPlantGrowthModel() = default;
    virtual const char* Name() const = 0;
    virtual void Grow(PlantSkeleton& s, const Species& sp, f32 a0, f32 a1) = 0;
};

// ---- 3. Distribution / scatter (owner's IVegetationDistribution) -------------------
// Per-shard, pure CPU, seeded from the shard context so results are tile-seamless and
// order-independent across parallel jobs.
class IVegetationDistribution {
public:
    virtual ~IVegetationDistribution() = default;
    virtual const char* Name() const = 0;
    virtual void Scatter(const VegShardContext& ctx, const BiomeSet& biomes,
                         const SpeciesRegistry& species, PlaceOut& out) = 0;
};

// ---- 4. Noise field provider (swappable, so no noise lib is load-bearing) ----------
// Stateless per-call seed keeps it parallel-safe. FastNoise2 slots in here (P2); the
// P1 built-in is a small deterministic value-noise so nothing depends on a library yet.
class INoiseField {
public:
    virtual ~INoiseField() = default;
    virtual const char* Name() const = 0;
    virtual f32 Sample(f32 x, f32 z, i32 seed) const = 0;
};

// ---- 5. Wind (render-side; fills the audited SceneEnvironment wind gap) ------------
class IWindModel {
public:
    virtual ~IWindModel() = default;
    virtual const char* Name() const = 0;
    virtual WindSample Evaluate(const SceneEnvironment& env, const glm::vec3& worldPos,
                                f32 timeSeconds) const = 0;
};

// ---- 6. Simulation LOD (owner's IVegetationSimulation) -----------------------------
class IVegetationSimulation {
public:
    virtual ~IVegetationSimulation() = default;
    virtual const char* Name() const = 0;
    virtual void Tick(VegetationStore& store, SimTier tier, f32 dt, const WindSample& wind) = 0;
};

// ---- 7. Render backend (render-side; declared now, implemented P5/P6) --------------
// Kept minimal in P1 so this header pulls in no RHI/Renderer types. The real submit
// methods (CPU-instanced trees -> DrawItems; GPU-driven grass) are added in P5/P6.
class IVegetationRenderer {
public:
    virtual ~IVegetationRenderer() = default;
    virtual const char* Name() const = 0;
};

} // namespace veg
} // namespace hbe

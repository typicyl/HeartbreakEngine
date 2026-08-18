// Vegetation/PoissonDiscDistribution.cpp - deterministic, tileable Poisson-disc scatter.
//
// Implemented in-house against Core/Rng (NOT a library): correctness here is entirely
// about determinism + tileability, and a third-party sampler would drag its own
// std::mt19937/uniform_real (the exact non-portable-determinism trap Core/Rng exists to
// avoid) and would not tile across streamed shards.
//
// METHOD: a jittered lattice with local Poisson rejection. Space is a grid of cells of
// side r (the min spacing); each cell holds ONE candidate at a position that is a pure
// function of (cellX, cellZ, worldSeed), so two shards sharing a cell agree on it - the
// scatter is SEAMLESS across shard boundaries by construction. A candidate survives if no
// higher-priority neighbour candidate (deterministic priority hash) lies within r, giving
// blue-noise-like spacing. Density can be modulated by a noise field for clumping. Every
// draw (jitter, priority, acceptance, species pick, per-instance seed) is on its own
// Rng.Split salt so adding a consumer never renumbers the others.
#include "Vegetation/VegetationBackends.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Vegetation/VegetationWorld.h"
#include "Core/Rng.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace hbe::veg {
namespace {

// Salts keep independent draw streams stable as the algorithm grows.
constexpr u64 kSaltJitter   = 0x11u;
constexpr u64 kSaltPriority = 0x22u;
constexpr u64 kSaltAccept   = 0x33u;
constexpr u64 kSaltSpecies  = 0x44u;
constexpr u64 kSaltInstance = 0x55u;

u64 CellHash(i32 cx, i32 cz, u64 worldSeed) {
    Hasher h;
    h.Mix(worldSeed);
    h.Mix(cx);
    h.Mix(cz);
    return h.Value();
}

i32 FloorDiv(f32 v, f32 cell) {
    return static_cast<i32>(std::floor(v / cell));
}

// True in [lo,hi] with a soft ramp of `margin` outside; returns a 0..1 fit score.
f32 RangeFit(f32 v, f32 lo, f32 hi, f32 margin) {
    if (v >= lo && v <= hi) return 1.0f;
    if (margin <= 0.0f) return 0.0f;
    const f32 d = (v < lo) ? (lo - v) : (v - hi);
    return glm::clamp(1.0f - d / margin, 0.0f, 1.0f);
}

class PoissonDiscDistribution final : public IVegetationDistribution {
public:
    const char* Name() const override { return "poisson"; }

    void Scatter(const VegShardContext& ctx, const BiomeSet& biomes,
                 const SpeciesRegistry& species, PlaceOut& out) override {
        out.Clear();
        if (biomes.Empty()) return;

        // Effective density = the densest applicable biome, scaled by the LOD reduction.
        f32 density = 0.0f;
        for (const Biome& b : biomes.biomes) density = std::max(density, b.baseDensity);
        density *= glm::clamp(ctx.lodDensityScale, 0.0f, 1.0f);
        if (density <= 0.0f) return;

        const f32 r = 1.0f / std::sqrt(density); // approx min spacing (m)
        const f32 cell = r;
        const f32 r2 = r * r;

        const i32 cx0 = FloorDiv(ctx.aabbMin.x, cell) - 1;
        const i32 cx1 = FloorDiv(ctx.aabbMax.x, cell) + 1;
        const i32 cz0 = FloorDiv(ctx.aabbMin.y, cell) - 1;
        const i32 cz1 = FloorDiv(ctx.aabbMax.y, cell) + 1;

        // Guard against a pathologically large cell count (huge shard / tiny density).
        const i64 cellCount = static_cast<i64>(cx1 - cx0 + 1) * (cz1 - cz0 + 1);
        if (cellCount <= 0 || cellCount > 4'000'000) return;

        for (i32 cz = cz0; cz <= cz1; ++cz) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                const glm::vec2 cand = CellPoint(cx, cz, ctx.worldSeed, cell);
                if (cand.x < ctx.aabbMin.x || cand.x >= ctx.aabbMax.x ||
                    cand.y < ctx.aabbMin.y || cand.y >= ctx.aabbMax.y)
                    continue;

                if (!SurvivesRejection(cand, cx, cz, ctx.worldSeed, cell, r2)) continue;

                Rng crng = Rng(ctx.worldSeed).Split(CellHash(cx, cz, ctx.worldSeed));

                // Density clumping: modulate acceptance by the noise field.
                if (ctx.noise) {
                    const f32 d = ctx.noise->Sample(cand.x * 0.03f, cand.y * 0.03f,
                                                    static_cast<i32>(ctx.worldSeed));
                    const f32 accept = glm::clamp(d * 1.4f, 0.08f, 1.0f);
                    if (crng.Split(kSaltAccept).NextFloat() > accept) continue;
                }

                // Terrain filter (flat plane when there is no surface query).
                SpawnSample ss;
                if (ctx.surface) {
                    ss = ctx.surface(cand);
                } else {
                    // No terrain: scatter on a flat, dry plane (waterDepth stays at its
                    // large-negative "no water" default so exclusion never triggers).
                    ss.onTerrain = true; ss.moisture = 0.5f;
                }
                if (!ss.onTerrain || ss.waterDepth > 0.0f) continue;

                const SpeciesId pick =
                    PickSpecies(biomes, species, ss, crng.Split(kSaltSpecies));
                if (!pick.Valid()) continue;

                const u64 instSeed = crng.Split(kSaltInstance).NextU64();
                out.Add(cand, pick, instSeed);
            }
        }
    }

private:
    static glm::vec2 CellPoint(i32 cx, i32 cz, u64 worldSeed, f32 cell) {
        Rng j = Rng(worldSeed).Split(CellHash(cx, cz, worldSeed)).Split(kSaltJitter);
        const f32 jx = j.NextFloat();
        const f32 jz = j.NextFloat();
        return glm::vec2((static_cast<f32>(cx) + jx) * cell,
                         (static_cast<f32>(cz) + jz) * cell);
    }

    static u64 CellPriority(i32 cx, i32 cz, u64 worldSeed) {
        return Rng(worldSeed).Split(CellHash(cx, cz, worldSeed)).Split(kSaltPriority).NextU64();
    }

    // A candidate survives if no neighbour candidate within r has a higher priority
    // (ties broken by the cell hash, so exactly one of a colliding pair survives).
    static bool SurvivesRejection(const glm::vec2& cand, i32 cx, i32 cz, u64 worldSeed,
                                  f32 cell, f32 r2) {
        const u64 pr = CellPriority(cx, cz, worldSeed);
        const u64 selfHash = CellHash(cx, cz, worldSeed);
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const i32 nx = cx + dx, nz = cz + dz;
                const glm::vec2 nc = CellPoint(nx, nz, worldSeed, cell);
                const glm::vec2 d = cand - nc;
                if (d.x * d.x + d.y * d.y >= r2) continue;
                const u64 npr = CellPriority(nx, nz, worldSeed);
                const u64 nHash = CellHash(nx, nz, worldSeed);
                if (npr > pr || (npr == pr && nHash > selfHash)) return false;
            }
        }
        return true;
    }

    // Weighted-random species pick from the applicable biome rules at this sample.
    static SpeciesId PickSpecies(const BiomeSet& biomes, const SpeciesRegistry& reg,
                                 const SpawnSample& ss, Rng rng) {
        // Two-pass reservoir-free weighted pick: sum weights, draw, walk. Deterministic.
        f32 total = 0.0f;
        // First pass: total weight.
        for (const Biome& b : biomes.biomes) {
            if (!BiomeApplies(b, ss)) continue;
            for (const BiomeSpeciesRule& sr : b.species)
                total += RuleWeight(sr, reg, ss);
        }
        if (total <= 0.0f) return SpeciesId{};

        f32 pickT = rng.NextFloat() * total;
        for (const Biome& b : biomes.biomes) {
            if (!BiomeApplies(b, ss)) continue;
            for (const BiomeSpeciesRule& sr : b.species) {
                const f32 w = RuleWeight(sr, reg, ss);
                if (w <= 0.0f) continue;
                pickT -= w;
                if (pickT <= 0.0f) return sr.species;
            }
        }
        return SpeciesId{};
    }

    static bool BiomeApplies(const Biome& b, const SpawnSample& ss) {
        if (ss.height < b.altitudeRange.x || ss.height > b.altitudeRange.y) return false;
        if (ss.slopeDeg < b.slopeRangeDeg.x || ss.slopeDeg > b.slopeRangeDeg.y) return false;
        if (ss.moisture < b.moistureRange.x || ss.moisture > b.moistureRange.y) return false;
        // Water exclusion: require the ground to sit at least `waterExclusion` above the
        // water surface (waterDepth is waterY - groundY, so -waterDepth is the clearance).
        if (-ss.waterDepth < b.waterExclusion) return false;
        return true;
    }

    static f32 RuleWeight(const BiomeSpeciesRule& sr, const SpeciesRegistry& reg,
                          const SpawnSample& ss) {
        if (!reg.Valid(sr.species)) return 0.0f;
        if (sr.requireSplatLayer >= 0 && ss.splatLayer != sr.requireSplatLayer) return 0.0f;
        const Species& sp = reg.Get(sr.species);
        // Environment fit: altitude, slope, moisture (soft ramps so an edge is not a cliff).
        const f32 fitAlt = RangeFit(ss.height, sp.altitudeRange.x, sp.altitudeRange.y, 30.0f);
        const f32 fitSlope =
            RangeFit(ss.slopeDeg, sp.slopeToleranceDeg.x, sp.slopeToleranceDeg.y, 8.0f);
        const f32 fitMoist = RangeFit(ss.moisture, sp.moistureRange.x, sp.moistureRange.y, 0.2f);
        const f32 fit = fitAlt * fitSlope * fitMoist;
        return sr.weight * sr.densityMul * fit;
    }
};

} // namespace

std::unique_ptr<IVegetationDistribution> MakePoissonDiscDistribution() {
    return std::make_unique<PoissonDiscDistribution>();
}

} // namespace hbe::veg

// Vegetation/VegetationScatterTest.cpp - --test-vegscatter.
//
// Pins P2: determinism, tileable min-distance, terrain/water filtering, biome/species
// scoring, and the .hbbiome round-trip. Headless (Scene + TerrainComponent need no GPU;
// terrain::EnsureHeights + SampleSurface are renderer-free).
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Vegetation/VegetationBackends.h"
#include "Vegetation/VegetationSurface.h"
#include "Vegetation/BiomeAsset.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/TerrainSystem.h"
#include "Core/Log.h"

#include <cmath>
#include <vector>

namespace hbe::veg {
namespace {

int g_fail = 0;
#define VS_CHECK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) { HBE_ERROR("vegscatter: FAIL - {}", msg); ++g_fail; }    \
    } while (0)

Species WideSpecies(const char* name, glm::vec2 moisture) {
    Species s;
    s.name = name;
    s.altitudeRange = {-1000.0f, 1000.0f};
    s.slopeToleranceDeg = {0.0f, 60.0f};
    s.moistureRange = moisture;
    return s;
}

bool SamePlacements(const PlaceOut& a, const PlaceOut& b) {
    if (a.Count() != b.Count()) return false;
    for (u32 i = 0; i < a.Count(); ++i) {
        if (a.xz[i] != b.xz[i]) return false;
        if (!(a.species[i] == b.species[i])) return false;
        if (a.seed[i] != b.seed[i]) return false;
    }
    return true;
}

} // namespace

bool ScatterSelfTest() {
    g_fail = 0;
    VegetationWorld world;
    SpeciesRegistry& reg = world.Species();
    const SpeciesId oak = reg.Add(WideSpecies("oak", {0.0f, 0.45f}));   // prefers DRY
    const SpeciesId pine = reg.Add(WideSpecies("pine", {0.55f, 1.0f})); // prefers WET

    IVegetationDistribution* dist = world.Distribution("poisson");
    VS_CHECK(dist != nullptr, "'poisson' distribution registered");

    // --- Biome + .hbbiome round-trip -------------------------------------------------
    Biome biome;
    biome.name = "test_forest";
    biome.altitudeRange = {-1000.0f, 1000.0f};
    biome.slopeRangeDeg = {0.0f, 60.0f};
    biome.moistureRange = {0.0f, 1.0f};
    biome.baseDensity = 0.04f; // ~1 plant / 25 m^2 -> r = 5 m
    biome.waterExclusion = 0.5f;
    biome.species.push_back({oak, 1.0f, 1.0f, -1});
    biome.species.push_back({pine, 1.0f, 1.0f, -1});
    {
        const std::string text = BiomeToJson(biome, reg);
        Biome loaded;
        VS_CHECK(ParseBiomeJson(text, reg, loaded), "biome JSON round-trips (parse)");
        VS_CHECK(loaded.name == biome.name, "biome name round-trips");
        VS_CHECK(loaded.species.size() == 2, "biome species rules round-trip");
        VS_CHECK(loaded.baseDensity == biome.baseDensity, "biome density round-trips");
        if (loaded.species.size() == 2) {
            VS_CHECK(loaded.species[0].species == oak, "rule 0 resolves to oak");
            VS_CHECK(loaded.species[1].species == pine, "rule 1 resolves to pine");
        }
        // A rule naming an unknown species is dropped, not fatal.
        Biome dropped;
        const std::string bad =
            R"({"name":"x","species":[{"species":"ghost","weight":1}]})";
        VS_CHECK(ParseBiomeJson(bad, reg, dropped), "biome with unknown species parses");
        VS_CHECK(dropped.species.empty(), "unknown-species rule is dropped");
    }

    BiomeSet biomes;
    biomes.biomes.push_back(biome);

    // --- Determinism: same seed -> identical placements (flat plane, no noise) --------
    VegShardContext ctx;
    ctx.worldSeed = 0xC0FFEE1234u;
    ctx.shardId = 7;
    ctx.aabbMin = {0.0f, 0.0f};
    ctx.aabbMax = {100.0f, 100.0f};
    // no surface (flat plane), no noise (full acceptance) -> fully deterministic set

    PlaceOut a, b;
    dist->Scatter(ctx, biomes, reg, a);
    dist->Scatter(ctx, biomes, reg, b);
    VS_CHECK(a.Count() > 20, "scatter produced a populated field");
    VS_CHECK(SamePlacements(a, b), "same worldSeed -> byte-identical placements");

    // Different seed -> different set.
    VegShardContext ctx2 = ctx;
    ctx2.worldSeed = 0xABCDEF99u;
    PlaceOut c;
    dist->Scatter(ctx2, biomes, reg, c);
    VS_CHECK(!SamePlacements(a, c), "different worldSeed -> different placements");

    // --- Bounds + tileable min-distance ----------------------------------------------
    const f32 r = 1.0f / std::sqrt(biome.baseDensity); // matches the distribution
    bool inBounds = true;
    f32 minDist2 = 1e30f;
    for (u32 i = 0; i < a.Count(); ++i) {
        const glm::vec2 p = a.xz[i];
        if (p.x < ctx.aabbMin.x || p.x >= ctx.aabbMax.x || p.y < ctx.aabbMin.y ||
            p.y >= ctx.aabbMax.y)
            inBounds = false;
        for (u32 j = i + 1; j < a.Count(); ++j) {
            const glm::vec2 d = p - a.xz[j];
            minDist2 = std::min(minDist2, d.x * d.x + d.y * d.y);
        }
    }
    VS_CHECK(inBounds, "all placements lie within the shard AABB");
    VS_CHECK(minDist2 >= (0.5f * r) * (0.5f * r), "placements respect a min spacing");

    // --- Seamless tiling: a shifted shard agrees on the overlap ----------------------
    // Two shards over the same seed but different AABBs must produce the SAME points where
    // they overlap (the lattice is a pure function of cell coord + worldSeed).
    {
        VegShardContext left = ctx;  left.aabbMin = {0.0f, 0.0f};   left.aabbMax = {50.0f, 100.0f};
        VegShardContext right = ctx; right.aabbMin = {50.0f, 0.0f}; right.aabbMax = {100.0f, 100.0f};
        PlaceOut pl, pr;
        dist->Scatter(left, biomes, reg, pl);
        dist->Scatter(right, biomes, reg, pr);
        // Every left/right point should also appear in the full-shard result `a`.
        auto inA = [&](const glm::vec2& q) {
            for (u32 i = 0; i < a.Count(); ++i)
                if (a.xz[i] == q) return true;
            return false;
        };
        bool tiled = (pl.Count() + pr.Count() > 0);
        for (u32 i = 0; i < pl.Count(); ++i) tiled = tiled && inA(pl.xz[i]);
        for (u32 i = 0; i < pr.Count(); ++i) tiled = tiled && inA(pr.xz[i]);
        VS_CHECK(tiled, "split shards reproduce the same points as the whole (seamless)");
    }

    // --- Terrain/water filtering via a synthetic surface -----------------------------
    // x < 20 = off-terrain (cliff); a z-band [40,60] is submerged; moisture rises with x.
    {
        VegShardContext sctx = ctx;
        sctx.surface = [](const glm::vec2& q) -> SpawnSample {
            SpawnSample s;
            s.onTerrain = q.x >= 20.0f;
            s.height = 0.0f;
            s.slopeDeg = 0.0f;
            s.waterDepth = (q.y >= 40.0f && q.y <= 60.0f) ? 1.0f : -10.0f; // submerged band
            s.moisture = glm::clamp(q.x / 100.0f, 0.0f, 1.0f);
            return s;
        };
        PlaceOut sp;
        dist->Scatter(sctx, biomes, reg, sp);
        VS_CHECK(sp.Count() > 0, "scatter with a surface still produces plants");
        bool offCliff = false, inWater = false;
        // Tight buckets past each species' moisture fit cutoff (oak fades out above
        // moisture 0.65 -> x>65; pine fades out below 0.35 -> x<35), so the mix is
        // UNAMBIGUOUS there rather than a probabilistic lean.
        u32 oakDry = 0, pineDry = 0, oakWet = 0, pineWet = 0;
        for (u32 i = 0; i < sp.Count(); ++i) {
            const glm::vec2 p = sp.xz[i];
            if (p.x < 20.0f) offCliff = true;
            if (p.y >= 40.0f && p.y <= 60.0f) inWater = true;
            if (p.x < 35.0f) { if (sp.species[i] == oak) ++oakDry; else ++pineDry; }
            if (p.x > 65.0f) { if (sp.species[i] == pine) ++pineWet; else ++oakWet; }
        }
        VS_CHECK(!offCliff, "no plants on the off-terrain cliff (x<20)");
        VS_CHECK(!inWater, "no plants in the submerged band");
        // Moisture scoring: only oak survives the dry region, only pine the wet region.
        VS_CHECK(oakDry > 0 && pineDry == 0, "dry region (x<35) is oak-only");
        VS_CHECK(pineWet > 0 && oakWet == 0, "wet region (x>65) is pine-only");
    }

    // --- Real terrain bridge (Scene + TerrainComponent, headless) --------------------
    {
        Scene scene;
        const entt::entity te = scene.CreateEntity("terrain");
        scene.Registry().emplace<Transform>(te);
        TerrainComponent& tc = scene.Registry().emplace<TerrainComponent>(te);
        tc.chunks = 2; tc.resolution = 8; tc.chunkSize = 16.0f; tc.height = 0.0f;
        terrain::EnsureHeights(tc); // flat heightfield (height=0), renderer-free

        VS_CHECK(FindTerrain(scene) == te, "FindTerrain locates the terrain entity");
        SurfaceQueryFn q = MakeTerrainSurfaceQuery(scene);
        VS_CHECK(static_cast<bool>(q), "surface query built over the terrain");
        if (q) {
            const SpawnSample center = q(glm::vec2(0.0f, 0.0f));
            VS_CHECK(center.onTerrain, "origin is on the terrain");
            VS_CHECK(std::abs(center.height) < 0.001f, "flat terrain height ~0");
            VS_CHECK(center.slopeDeg < 1.0f, "flat terrain slope ~0");
            const f32 ext = terrain::ExtentXZ(tc);
            const SpawnSample outside = q(glm::vec2(ext, ext)); // past the footprint
            VS_CHECK(!outside.onTerrain, "a point past the footprint is off-terrain");
        }
    }

    // --- Default noise backend actually runs + drives deterministic clumping ---------
    // Exercises whichever noise is the default (FastNoise2 when compiled in, else the
    // built-in value noise) so the backend is proven to RUN, not merely link.
    {
        INoiseField* nz = world.DefaultNoise();
        VS_CHECK(nz != nullptr, "a default noise field exists");
        if (nz) {
            const f32 v1 = nz->Sample(3.5f, -2.25f, 1234);
            const f32 v2 = nz->Sample(3.5f, -2.25f, 1234);
            VS_CHECK(v1 == v2, "default noise: same (x,z,seed) -> same value");
            VS_CHECK(v1 >= 0.0f && v1 < 1.0f, "default noise in [0,1)");
            VS_CHECK(nz->Sample(3.5f, -2.25f, 1235) != v1, "default noise: seed changes value");

            VegShardContext nctx = ctx;
            nctx.noise = nz;
            PlaceOut n1, n2;
            dist->Scatter(nctx, biomes, reg, n1);
            dist->Scatter(nctx, biomes, reg, n2);
            VS_CHECK(n1.Count() > 0, "noise-modulated scatter still produces plants");
            VS_CHECK(SamePlacements(n1, n2), "noise-modulated scatter is deterministic");
            VS_CHECK(n1.Count() <= a.Count(), "noise clumping only removes points (<= uniform)");
        }
    }

    if (g_fail == 0) HBE_INFO("vegscatter: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg

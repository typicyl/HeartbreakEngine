// Vegetation/VegetationStreaming.cpp - streamed shard generation + job-safety test.
#include "Vegetation/VegetationStreaming.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationStore.h"
#include "Core/Rng.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <vector>

namespace hbe::veg {

void GenerateShard(const VegetationWorld& world, const VegShardContext& ctx,
                   const BiomeSet& biomes, VegetationStore& store) {
    IVegetationDistribution* dist = world.Distribution("poisson");
    if (!dist) return;

    PlaceOut places;
    dist->Scatter(ctx, biomes, world.Species(), places);

    for (u32 i = 0; i < places.Count(); ++i) {
        const SpeciesId sid = places.species[i];
        if (!world.Species().Valid(sid)) continue;
        const Species& sp = world.Species().Get(sid);
        IPlantGenerator* gen = world.GeneratorForStrategy(sp.strategy);
        if (!gen) continue;

        // Ground the plant (skip off-terrain points when a surface query is present).
        f32 groundY = 0.0f;
        if (ctx.surface) {
            const SpawnSample ss = ctx.surface(places.xz[i]);
            if (!ss.onTerrain) continue;
            groundY = ss.height;
        }

        PlantGenParams p;
        p.species = sid;
        p.seed = places.seed[i];
        p.age01 = 1.0f;
        PlantSkeleton skel;
        if (!gen->Generate(p, sp, skel)) continue;

        // Deterministic per-instance world transform (yaw + slight scale from the seed).
        Rng r(places.seed[i]);
        const f32 yaw = r.NextFloat() * glm::two_pi<f32>();
        const f32 scale = 0.8f + 0.4f * r.NextFloat();
        glm::mat4 xform =
            glm::translate(glm::mat4(1.0f), glm::vec3(places.xz[i].x, groundY, places.xz[i].y));
        xform *= glm::mat4_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        xform = glm::scale(xform, glm::vec3(scale));

        store.AddTree(xform, sid, places.seed[i], skel);
    }
}

namespace {

// Compares two stores' tree rows for byte-equality (determinism check).
bool SameTrees(const VegetationStore& a, const VegetationStore& b) {
    if (a.trees.Count() != b.trees.Count()) return false;
    for (u32 i = 0; i < a.trees.Count(); ++i) {
        if (a.trees.seed[i] != b.trees.seed[i]) return false;
        if (!(a.trees.species[i] == b.trees.species[i])) return false;
        if (a.trees.branchCount[i] != b.trees.branchCount[i]) return false;
        if (a.trees.xform[i] != b.trees.xform[i]) return false;
    }
    return true;
}

Species WideOak() {
    Species s;
    s.name = "oak";
    s.altitudeRange = {-1000.0f, 1000.0f};
    s.slopeToleranceDeg = {0.0f, 60.0f};
    return s;
}

} // namespace

bool StreamSelfTest() {
    int fail = 0;
    auto check = [&](bool c, const char* m) { if (!c) { HBE_ERROR("vegstream: FAIL - {}", m); ++fail; } };

    VegetationWorld world;
    const SpeciesId oak = world.Species().Add(WideOak());
    Biome biome;
    biome.name = "b";
    biome.altitudeRange = {-1000.0f, 1000.0f};
    biome.slopeRangeDeg = {0.0f, 60.0f};
    biome.baseDensity = 0.02f;
    biome.species.push_back({oak, 1.0f, 1.0f, -1});
    BiomeSet biomes;
    biomes.biomes.push_back(biome);

    auto makeCtx = [&](u64 shard) {
        VegShardContext c;
        c.worldSeed = 0xF0057EEDull;
        c.shardId = shard;
        // Each shard is a 40 m tile; they tile the plane by shard index.
        const f32 tile = 40.0f;
        const f32 ox = static_cast<f32>(shard % 4u) * tile;
        const f32 oz = static_cast<f32>(shard / 4u) * tile;
        c.aabbMin = {ox, oz};
        c.aabbMax = {ox + tile, oz + tile};
        return c;
    };

    // Determinism: the same shard generated twice is byte-identical.
    VegetationStore a, b;
    GenerateShard(world, makeCtx(3), biomes, a);
    GenerateShard(world, makeCtx(3), biomes, b);
    check(a.trees.Count() > 0, "shard generates trees");
    check(SameTrees(a, b), "same shard ctx -> identical store");

    // Job-safety: generate many shards IN PARALLEL into their own stores and compare each
    // to a serial reference. Stateless generators + per-shard stores => no locking needed.
    constexpr u32 kShards = 16;
    std::vector<VegetationStore> serial(kShards), parallel(kShards);
    for (u32 s = 0; s < kShards; ++s) GenerateShard(world, makeCtx(s), biomes, serial[s]);

    jobs::Initialize();
    jobs::ParallelFor(kShards, 1, [&](u32 begin, u32 end) {
        for (u32 s = begin; s < end; ++s) GenerateShard(world, makeCtx(s), biomes, parallel[s]);
    });
    jobs::Shutdown();

    bool allMatch = true;
    u32 total = 0;
    for (u32 s = 0; s < kShards; ++s) {
        if (!SameTrees(serial[s], parallel[s])) allMatch = false;
        total += serial[s].trees.Count();
    }
    check(allMatch, "parallel shard generation matches the serial reference (job-safe)");
    check(total > 0, "shards produced trees");

    if (fail == 0) HBE_INFO("vegstream: all checks passed ({} trees across {} shards)", total, kShards);
    return fail == 0;
}

} // namespace hbe::veg

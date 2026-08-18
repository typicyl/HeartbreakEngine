// Vegetation/VegetationBackends.cpp - the built-in (in-house) vegetation backends.
//
// All backends are deterministic and, where they run on worker fibers (generation,
// distribution, noise), touch nothing but their inputs + Core/Rng. See the threading
// contract in VegetationInterfaces.h.
#include "Vegetation/VegetationBackends.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Vegetation/VegetationGrowth.h" // GrowSkeleton (IncrementalGrowth backend)
#include "Core/Rng.h"
#include "Scene/Scene.h" // SceneEnvironment (the wind model reads heading/speed/time)

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <memory>

namespace hbe::veg {
namespace {

// ===== 1. Generator: a minimal but structurally-real trunk+branch skeleton ==========
// NOT the final generator (space colonization / L-system land in P4). It exists so P1
// has a deterministic, valid PlantSkeleton to mesh, store and hash. It produces a
// tapering trunk of segments, a ring of first-order branches near the crown, and a leaf
// cluster at each branch tip - enough structure to exercise every downstream consumer.
class MinimalTrunkGenerator final : public IPlantGenerator {
public:
    const char* Name() const override { return "minimal"; }

    bool Generate(const PlantGenParams& p, const Species& sp, PlantSkeleton& out) override {
        out.Clear();
        out.sourceSeed = p.seed;

        const f32 age = glm::clamp(p.age01, 0.05f, 1.0f);
        const f32 height = sp.maxHeight * age;
        const u32 trunkSegs = 6u + static_cast<u32>(sp.maxBranchOrder) * 2u;
        const f32 baseR = sp.trunkRadius * age;

        Rng rng(p.seed);
        Rng leanRng = rng.Split(1);   // trunk lean/wobble
        Rng branchRng = rng.Split(2); // branch placement/angles
        Rng leafRng = rng.Split(3);   // leaf-cluster jitter

        // --- Trunk: a slightly leaning column of tapering segments -------------------
        i32 prev = -1;
        f32 leanX = leanRng.Signed() * 0.15f * (1.0f - sp.windResistance);
        f32 leanZ = leanRng.Signed() * 0.15f * (1.0f - sp.windResistance);
        std::vector<u32> trunkNodes;
        trunkNodes.reserve(trunkSegs + 1);
        for (u32 i = 0; i <= trunkSegs; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(trunkSegs);
            const f32 y = height * t;
            const glm::vec3 pos(leanX * y, y, leanZ * y);
            const f32 r = baseR * (1.0f - 0.85f * t); // taper to a thin tip
            const u32 idx = out.AddNode(pos, prev, glm::max(r, 0.004f), 0u,
                                        (i == 0) ? PlantPart::Root : PlantPart::Trunk, age);
            trunkNodes.push_back(idx);
            prev = static_cast<i32>(idx);
        }

        // --- Branches: first-order limbs from the upper half of the trunk ------------
        const u32 branchCount =
            static_cast<u32>(glm::mix(2.0f, 10.0f, glm::clamp(sp.branchDensity, 0.0f, 1.0f)));
        const f32 branchLen = sp.crownWidth * 0.5f * age;
        for (u32 b = 0; b < branchCount; ++b) {
            // Attach to a trunk node in the upper 60%.
            const f32 up = glm::mix(0.4f, 0.95f, static_cast<f32>(b) / glm::max(1u, branchCount - 1u));
            const u32 attach = trunkNodes[static_cast<u32>(up * trunkSegs)];
            const glm::vec3 base = out.pos[attach];

            const f32 azimuth = branchRng.NextFloat() * glm::two_pi<f32>();
            const f32 elev = glm::radians(sp.branchAngleDeg +
                                          branchRng.Signed() * sp.branchAngleJitter);
            const glm::vec3 dir(std::cos(azimuth) * std::cos(elev),
                                std::sin(elev),
                                std::sin(azimuth) * std::cos(elev));
            const f32 len = branchLen * glm::mix(0.6f, 1.0f, branchRng.NextFloat());
            const glm::vec3 tip = base + dir * len;
            const f32 br = out.radius[attach] * 0.5f;

            const u32 mid = out.AddNode(base + dir * (len * 0.5f), static_cast<i32>(attach),
                                        glm::max(br, 0.003f), 1u, PlantPart::Branch, age);
            const u32 end = out.AddNode(tip, static_cast<i32>(mid),
                                        glm::max(br * sp.taper, 0.002f), 2u, PlantPart::Twig, age);

            // Leaf cluster at the twig tip (skip when a deciduous species is bare, i.e.
            // very young - a cheap seasonal placeholder until the growth model, P10).
            if (sp.leafDensity > 0.0f) {
                const glm::vec3 jitter(leafRng.Signed(), leafRng.Signed(), leafRng.Signed());
                out.AddNode(tip + jitter * (sp.leafSize * 0.5f), static_cast<i32>(end),
                            sp.leafSize, 3u, PlantPart::LeafCluster, age);
            }
        }

        return out.NodeCount() > 0 && out.Validate();
    }
};

// ===== 2. Growth =====================================================================
// Incremental structural growth: advances a skeleton from a0 to a1 by ADDING tip/leaf
// structure and thickening the wood (Vegetation/VegetationGrowth). NoGrowth is the
// explicit off switch.
class IncrementalGrowth final : public IPlantGrowthModel {
public:
    const char* Name() const override { return "incremental"; }
    void Grow(PlantSkeleton& s, const Species& sp, f32 a0, f32 a1) override {
        GrowSkeleton(s, sp, a0, a1);
    }
};
class NoGrowth final : public IPlantGrowthModel {
public:
    const char* Name() const override { return "none"; }
    void Grow(PlantSkeleton&, const Species&, f32, f32) override {}
};

// ===== 3. Distribution: null placeholder (Bridson Poisson-disc lands in P2) ==========
class NullDistribution final : public IVegetationDistribution {
public:
    const char* Name() const override { return "null"; }
    void Scatter(const VegShardContext&, const BiomeSet&, const SpeciesRegistry&,
                 PlaceOut& out) override {
        out.Clear();
    }
};

// ===== 4. Noise: deterministic 2D value noise (FastNoise2 backend added in P2) =======
// Integer hash -> lattice value -> smoothstep bilerp. Stateless per-call seed, so it is
// safe to sample from any worker fiber. Same (x,z,seed) -> same value everywhere.
class ValueNoiseField final : public INoiseField {
public:
    const char* Name() const override { return "value"; }

    f32 Sample(f32 x, f32 z, i32 seed) const override {
        const i32 x0 = FastFloor(x), z0 = FastFloor(z);
        const f32 fx = x - static_cast<f32>(x0), fz = z - static_cast<f32>(z0);
        const f32 ux = Smooth(fx), uz = Smooth(fz);
        const f32 v00 = Lattice(x0, z0, seed), v10 = Lattice(x0 + 1, z0, seed);
        const f32 v01 = Lattice(x0, z0 + 1, seed), v11 = Lattice(x0 + 1, z0 + 1, seed);
        const f32 a = glm::mix(v00, v10, ux);
        const f32 b = glm::mix(v01, v11, ux);
        return glm::mix(a, b, uz); // [0,1)
    }

private:
    static i32 FastFloor(f32 v) {
        const i32 i = static_cast<i32>(v);
        return (v < static_cast<f32>(i)) ? i - 1 : i;
    }
    static f32 Smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }
    static f32 Lattice(i32 x, i32 z, i32 seed) {
        u64 h = static_cast<u64>(static_cast<u32>(x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<u64>(static_cast<u32>(z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<u64>(static_cast<u32>(seed)) * 0x165667B19E3779F9ull;
        h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        return static_cast<f32>(h >> 40) * (1.0f / 16777216.0f); // [0,1)
    }
};

// ===== 5. Wind: hierarchical model derived from the scene's cloud heading ============
// P1 fills the audited SceneEnvironment wind gap: HB has a cloud drift heading
// (windAngle) + speed (windSpeed) but no physical wind vector. This derives a base
// heading/strength from those and adds a time-driven gust + turbulence. The full
// trunk->branch->twig->leaf hierarchy is a vertex/compute effect layered on top in P7.
class HierarchicalWind final : public IWindModel {
public:
    const char* Name() const override { return "hierarchical"; }

    WindSample Evaluate(const SceneEnvironment& env, const glm::vec3& worldPos,
                        f32 t) const override {
        WindSample w;
        const f32 a = glm::radians(env.windAngle);
        w.dir = glm::vec3(std::cos(a), 0.0f, std::sin(a));
        // Cloud drift speed is tiny (UV/sec); map it to a visible sway strength, and let
        // a storm (precip) raise it.
        const f32 storm = glm::clamp(env.precipIntensity, 0.0f, 1.0f);
        w.strength = glm::clamp(env.windSpeed * 30.0f + storm * 0.6f, 0.0f, 2.0f);
        // Gust: slow sine beating; turbulence: faster, position-jittered.
        const f32 phase = worldPos.x * 0.05f + worldPos.z * 0.05f;
        w.gust = 0.5f + 0.5f * std::sin(t * 0.7f + phase);
        w.turbulence = 0.5f + 0.5f * std::sin(t * 4.3f + phase * 3.1f);
        return w;
    }
};

// ===== 6. Simulation: no-op tick (the sim-LOD tier machine lands in P7) ==============
class NoopSimulation final : public IVegetationSimulation {
public:
    const char* Name() const override { return "noop"; }
    void Tick(VegetationStore&, SimTier, f32, const WindSample&) override {}
};

} // namespace

void RegisterBuiltinBackends(VegetationWorld& world) {
    // Space colonization is the default generator (registered first); L-system is the
    // rule-based alternative; the minimal trunk stays as a cheap fallback.
    world.RegisterGenerator(MakeSpaceColonizationGenerator()); // "spacecol" (default)
    world.RegisterGenerator(MakeLSystemGenerator());           // "lsystem"
    world.RegisterGenerator(std::make_unique<MinimalTrunkGenerator>()); // "minimal"
    world.RegisterGrowth(std::make_unique<IncrementalGrowth>()); // "incremental" (default)
    world.RegisterGrowth(std::make_unique<NoGrowth>());          // "none"
    world.RegisterDistribution(std::make_unique<NullDistribution>());
    world.RegisterDistribution(MakePoissonDiscDistribution()); // "poisson" (P2 default scatter)
    // Noise: prefer FastNoise2 (SIMD) as the DEFAULT when compiled in - registered first
    // so it becomes DefaultNoise() - and always keep the built-in value noise as a
    // registered fallback backend so nothing is load-bearing on the library.
    if (auto fn = MakeFastNoise2Field()) world.RegisterNoise(std::move(fn));
    world.RegisterNoise(std::make_unique<ValueNoiseField>());
    world.RegisterWind(std::make_unique<HierarchicalWind>());
    world.RegisterSimulation(std::make_unique<NoopSimulation>());
}

} // namespace hbe::veg

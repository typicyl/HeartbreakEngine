// Vegetation/VegetationRender.cpp - CPU-instanced tree render path + the --vegdemo forest.
#include "Vegetation/VegetationRender.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Vegetation/TreeMesher.h"
#include "Vegetation/VegetationSurface.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/TerrainSystem.h"
#include "Scene/SceneSerializer.h" // save/load round-trip + veg: resolver (PaintSelfTest)
#include "Assets/UAF.h"            // uaf::ReadMesh (authoredMesh / External species)
#include "Project/Project.h"       // Project::Active().AssetsDir() (authoredMesh path)

#include <filesystem>
#include <optional>
#include "Renderer/Renderer.h"
#include "Core/Rng.h"
#include "Core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

namespace hbe::veg {
namespace {

// Uploads one submesh + its LOD chain and registers the chain. Returns the base handle.
rhi::MeshHandle UploadSubmesh(Renderer& renderer, const MeshData& md,
                              std::vector<rhi::MeshHandle>& owned) {
    if (md.Empty()) return {};
    const rhi::MeshHandle base = renderer.UploadMesh(md);
    if (!base.IsValid()) return {};
    // Track ONLY the base handle: RegisterMeshLods transfers LOD ownership to the base, so
    // DestroyMesh(base) frees the whole chain (Renderer.h) - tracking the LODs too would
    // double-free them.
    owned.push_back(base);
    if (!md.lods.empty()) {
        std::vector<rhi::MeshHandle> lodHandles;
        lodHandles.reserve(md.lods.size());
        for (const MeshLod& lod : md.lods) {
            MeshData tmp;
            tmp.vertices = lod.vertices;
            tmp.indices = lod.indices;
            tmp.material = md.material;
            const rhi::MeshHandle h = renderer.UploadMesh(tmp);
            if (h.IsValid()) lodHandles.push_back(h);
        }
        if (!lodHandles.empty()) renderer.RegisterMeshLods(base, lodHandles);
    }
    return base;
}

// Builds an RGBA8 alpha-cutout texture of a CLUSTER of leaves (each foliage card samples the whole
// thing): scattered almond leaf silhouettes over transparent, with per-leaf green variation, edge
// darkening, a lighter midrib vein, and a lighter tip. Deterministic. sRGB bytes (uploaded as an
// sRGB texture, so the sampler linearizes on read). This is what turns the solid green cards into
// leaf shapes - the single biggest look upgrade for the foliage.
std::vector<u8> MakeLeafClusterTexture(u32 res) {
    std::vector<u8> px(static_cast<usize>(res) * res * 4, 0);
    struct Leaf { glm::vec2 c; f32 ang, len, wid, val, hue; };
    Rng rng(0x1EAF5EEDull);
    std::vector<Leaf> leaves;
    leaves.reserve(20);
    for (u32 i = 0; i < 20; ++i) {
        Leaf L;
        L.c = glm::vec2(rng.Range(0.14f, 0.86f), rng.Range(0.10f, 0.9f));
        L.ang = rng.Range(0.0f, 6.2831853f);
        L.len = rng.Range(0.15f, 0.30f);
        L.wid = L.len * rng.Range(0.34f, 0.52f);
        L.val = rng.Range(0.70f, 1.15f);
        L.hue = rng.Range(-0.05f, 0.06f);
        leaves.push_back(L);
    }
    const auto toU8 = [](f32 v) { return static_cast<u8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (u32 y = 0; y < res; ++y) {
        for (u32 x = 0; x < res; ++x) {
            const glm::vec2 uv((x + 0.5f) / res, (y + 0.5f) / res);
            glm::vec3 col(0.0f);
            bool covered = false;
            for (const Leaf& L : leaves) {
                const glm::vec2 d = uv - L.c;
                const f32 ca = std::cos(-L.ang), sa = std::sin(-L.ang);
                const glm::vec2 lp(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                const f32 lu = lp.x / L.len; // along the leaf length, [-1,1]
                if (std::abs(lu) >= 1.0f) continue;
                const f32 halfW = L.wid * std::sqrt(glm::max(0.0f, 1.0f - lu * lu)); // almond taper
                if (std::abs(lp.y) > halfW) continue;
                const f32 edge = 1.0f - std::abs(lp.y) / glm::max(halfW, 1e-4f); // 1 center..0 edge
                const f32 midrib = std::abs(lp.y) < halfW * 0.09f ? 1.0f : 0.0f;
                glm::vec3 base(0.13f + L.hue, 0.40f, 0.12f - L.hue * 0.5f);
                base *= L.val * (0.55f + 0.45f * edge);                                 // edge shade
                base = glm::mix(base, glm::vec3(0.24f, 0.50f, 0.17f), 0.30f * (lu * 0.5f + 0.5f)); // tip
                base = glm::mix(base, glm::vec3(0.33f, 0.57f, 0.22f), midrib * 0.45f);   // vein
                col = base; // last (topmost) covering leaf wins
                covered = true;
            }
            const usize o = (static_cast<usize>(y) * res + x) * 4;
            if (covered) {
                px[o + 0] = toU8(col.r);
                px[o + 1] = toU8(col.g);
                px[o + 2] = toU8(col.b);
                px[o + 3] = 255;
            }
        }
    }
    return px;
}

} // namespace

rhi::TextureHandle VegetationMeshLibrary::LeafTexture(Renderer& renderer) {
    if (leafTexTried_) return leafTex_;
    leafTexTried_ = true;
    constexpr u32 kRes = 256;
    const std::vector<u8> pixels = MakeLeafClusterTexture(kRes);
    rhi::TextureDesc desc;
    desc.width = kRes;
    desc.height = kRes;
    desc.format = rhi::Format::R8G8B8A8_SRGB;
    desc.mipCount = 1;
    desc.pixels = pixels.data();
    desc.debugName = "VegLeafCluster";
    leafTex_ = renderer.UploadTexture(desc); // invalid on a device-less renderer (headless)
    return leafTex_;
}

const VegetationMeshLibrary::SpeciesMeshes&
VegetationMeshLibrary::GetOrCreate(Renderer& renderer, VegetationWorld& world, SpeciesId id,
                                   const Species& sp) {
    auto it = byId_.find(id.v);
    if (it != byId_.end()) return it->second;

    SpeciesMeshes m;

    // EXTERNALLY-AUTHORED mesh (GenStrategy::External): a proctree import or a Blender-exported
    // .uaf overrides procedural generation. This is the pluggable seam - any external tool that
    // can emit a .uaf (submesh 0 = woody, submesh 1 = optional foliage) backs a species and is
    // painted/scattered/persisted through the exact same path as a generated tree.
    bool authored = false;
    if (!sp.authoredMesh.empty()) {
        const std::filesystem::path path = Project::Active().AssetsDir() / sp.authoredMesh;
        if (std::optional<Model> model = uaf::ReadMesh(path); model && !model->empty()) {
            m.woody = UploadSubmesh(renderer, (*model)[0], owned_);
            if (model->size() >= 2) m.foliage = UploadSubmesh(renderer, (*model)[1], owned_);
            m.valid = m.woody.IsValid();
            authored = true; // loaded (even if a headless upload made no handle) - skip the generator
        } else {
            HBE_WARN("[veg] species '{}' authoredMesh '{}' not loadable; using the generator",
                     sp.name, sp.authoredMesh);
        }
    }

    IPlantGenerator* gen = authored ? nullptr : world.GeneratorForStrategy(sp.strategy);
    if (gen) {
        PlantGenParams p;
        p.species = id;
        p.seed = 0xA11CE5EEDull ^ (static_cast<u64>(id.v) * 0x9E3779B97F4A7C15ull);
        p.age01 = 1.0f;
        PlantSkeleton skel;
        if (gen->Generate(p, sp, skel)) {
            const Model model = BuildTreeMesh(skel, sp);
            if (!model.empty()) m.woody = UploadSubmesh(renderer, model[0], owned_);
            if (model.size() >= 2) m.foliage = UploadSubmesh(renderer, model[1], owned_);
            m.valid = m.woody.IsValid();
        }
    }
    return byId_.emplace(id.v, m).first->second;
}

void VegetationMeshLibrary::Clear(Renderer& renderer) {
    for (const rhi::MeshHandle h : owned_)
        if (h.IsValid()) renderer.DestroyMesh(h);
    owned_.clear();
    byId_.clear();
    if (leafTex_.IsValid()) renderer.DestroyTexture(leafTex_);
    leafTex_ = {};
    leafTexTried_ = false;
}

u32 PopulateForest(Scene& scene, Renderer& renderer, VegetationWorld& world,
                   VegetationMeshLibrary& lib, const PlaceOut& placements,
                   const SurfaceQueryFn& surface) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    u32 spawned = 0;
    for (u32 i = 0; i < placements.Count(); ++i) {
        const glm::vec2 xz = placements.xz[i];
        const SpeciesId sid = placements.species[i];
        if (!world.Species().Valid(sid)) continue;

        f32 groundY = 0.0f;
        if (surface) {
            const SpawnSample ss = surface(xz);
            if (!ss.onTerrain) continue;
            groundY = ss.height;
        }

        const Species& sp = world.Species().Get(sid);
        const VegetationMeshLibrary::SpeciesMeshes& m =
            lib.GetOrCreate(renderer, world, sid, sp);
        if (!m.valid) continue;

        // Deterministic per-instance transform from the placement seed.
        Rng r(placements.seed[i]);
        const f32 yaw = r.NextFloat() * glm::two_pi<f32>();
        const f32 scale = 0.8f + 0.4f * r.NextFloat();
        Transform t;
        t.position = glm::vec3(xz.x, groundY, xz.y);
        t.rotation = glm::angleAxis(yaw, up);
        t.scale = glm::vec3(scale);

        const entt::entity we = scene.CreateEntity("Tree");
        scene.Registry().emplace<Transform>(we, t);
        MeshInstance mi;
        mi.mesh = m.woody;
        mi.surface.base_color = sp.barkColor;
        mi.surface.specular_roughness = 0.85f;
        mi.materialFlags |= rhi::MaterialFlag_Wind; // sway in the wind (stiffer trunk)
        scene.Registry().emplace<MeshInstance>(we, mi);
        // Tag it so the erase brush can find it and a load-time pass can rebuild the mesh.
        scene.Registry().emplace<VegetationInstance>(
            we, VegetationInstance{sp.name, placements.seed[i], /*foliage=*/false, 1.0f});
        // Synthetic mesh SOURCE so the entity persists: the serializer writes MeshRef::source
        // verbatim, and the "veg:" load resolver rebuilds the runtime mesh from it (the raw
        // MeshInstance handle is not itself serializable). See SceneSerializer SetVegMeshResolver.
        scene.Registry().emplace<MeshRef>(we, MeshRef{"veg:" + sp.name + "/woody"});

        if (m.foliage.IsValid()) {
            const entt::entity fe = scene.CreateEntity("Foliage");
            scene.Registry().emplace<Transform>(fe, t);
            MeshInstance fmi;
            fmi.mesh = m.foliage;
            // The leaf-cluster texture supplies the green + the alpha cutout; base_color is a light
            // tint over it (near-white so the texture reads, biased to the species leaf colour).
            fmi.albedoTexture = lib.LeafTexture(renderer);
            fmi.surface.base_color = glm::vec4(glm::mix(glm::vec3(1.0f), glm::vec3(sp.leafColor), 0.35f), 1.0f);
            fmi.surface.specular_roughness = 0.55f;
            // Leaves are thin + translucent: subsurface gives the back-lit glow, and the alpha
            // cutout turns each card into leaf shapes. Wind flutters them.
            fmi.materialFlags |= rhi::MaterialFlag_Wind | rhi::MaterialFlag_Subsurface |
                                 rhi::MaterialFlag_AlphaTest;
            fmi.surface.subsurface_color = glm::mix(glm::vec3(sp.leafColor), glm::vec3(0.5f, 0.75f, 0.2f), 0.6f);
            fmi.surface.subsurface_radius = 0.6f;
            scene.Registry().emplace<MeshInstance>(fe, fmi);
            scene.Registry().emplace<VegetationInstance>(
                fe, VegetationInstance{sp.name, placements.seed[i], /*foliage=*/true, 1.0f});
            scene.Registry().emplace<MeshRef>(fe, MeshRef{"veg:" + sp.name + "/foliage"});
        }
        ++spawned;
    }
    return spawned;
}

bool PaintTreeAt(Scene& scene, Renderer& renderer, VegetationWorld& world,
                 VegetationMeshLibrary& lib, SpeciesId species, const glm::vec2& worldXZ,
                 u64 seed, const SurfaceQueryFn& surface) {
    if (!world.Species().Valid(species)) return false;
    // One placement through the SAME spawn path as scatter (so tagging/wind/foliage all match).
    PlaceOut one;
    one.Add(worldXZ, species, seed);
    return PopulateForest(scene, renderer, world, lib, one, surface) > 0;
}

u32 EraseVegetationAt(Scene& scene, const glm::vec2& worldXZ, f32 radius) {
    auto& reg = scene.Registry();
    const f32 r2 = radius * radius;
    // Identify painted/scattered plants by their "veg:" MeshRef source (which IS serialized), so
    // erase works on freshly-painted AND loaded trees - the in-session VegetationInstance tag is
    // not persisted. Collect first: reg.destroy mutates the pools the view iterates.
    std::vector<entt::entity> kill;
    auto view = reg.view<MeshRef, Transform>();
    for (const entt::entity e : view) {
        const MeshRef& ref = view.get<MeshRef>(e);
        if (ref.source.rfind("veg:", 0) != 0) continue;
        const Transform& t = view.get<Transform>(e);
        const f32 dx = t.position.x - worldXZ.x;
        const f32 dz = t.position.z - worldXZ.y;
        if (dx * dx + dz * dz <= r2) kill.push_back(e);
    }
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e); // the shared species mesh stays (owned by the library)
    return static_cast<u32>(kill.size());
}

u32 RehydrateVegetation(Scene& scene, Renderer& renderer, VegetationWorld& world,
                        VegetationMeshLibrary& lib) {
    auto& reg = scene.Registry();
    u32 n = 0;
    auto view = reg.view<VegetationInstance, MeshInstance>();
    for (const entt::entity e : view) {
        const VegetationInstance& vi = view.get<VegetationInstance>(e);
        MeshInstance& mi = view.get<MeshInstance>(e);
        const SpeciesId sid = world.Species().Find(vi.species);
        if (!world.Species().Valid(sid)) continue; // species not registered yet - caller's job
        const Species& sp = world.Species().Get(sid);
        const VegetationMeshLibrary::SpeciesMeshes& m = lib.GetOrCreate(renderer, world, sid, sp);
        if (!m.valid) continue;
        const rhi::MeshHandle want = vi.foliage ? m.foliage : m.woody;
        if (!want.IsValid() || mi.mesh.id == want.id) continue; // missing submesh / already correct
        mi.mesh = want;
        ++n;
    }
    return n;
}

u32 RegisterDemoSpecies(VegetationWorld& world) {
    SpeciesRegistry& reg = world.Species();
    Species oak;
    oak.name = "demo_oak";
    oak.strategy = GenStrategy::SpaceColonization;
    oak.maxHeight = 11.0f;
    oak.trunkRadius = 0.35f;
    oak.crownWidth = 7.0f;
    oak.branchDensity = 0.7f;
    oak.barkColor = {0.32f, 0.22f, 0.14f, 1.0f};
    oak.leafColor = {0.24f, 0.44f, 0.16f, 1.0f};
    oak.altitudeRange = {-100.0f, 100.0f};
    oak.slopeToleranceDeg = {0.0f, 32.0f};
    reg.Add(oak); // Add interns by name -> idempotent

    Species pine;
    pine.name = "demo_pine";
    pine.strategy = GenStrategy::LSystem;
    pine.maxHeight = 16.0f;
    pine.trunkRadius = 0.28f;
    pine.crownWidth = 4.5f;
    pine.branchDensity = 0.5f;
    pine.maxBranchOrder = 3;
    pine.barkColor = {0.26f, 0.18f, 0.12f, 1.0f};
    pine.leafColor = {0.16f, 0.34f, 0.20f, 1.0f};
    pine.altitudeRange = {-100.0f, 100.0f};
    pine.slopeToleranceDeg = {0.0f, 45.0f};
    reg.Add(pine);
    return reg.Count();
}

bool BakeSpeciesToUaf(VegetationWorld& world, SpeciesId species,
                      const std::filesystem::path& outPath) {
    if (!world.Species().Valid(species)) return false;
    const Species& sp = world.Species().Get(species);
    IPlantGenerator* gen = world.GeneratorForStrategy(sp.strategy);
    if (!gen) return false;
    PlantGenParams p;
    p.species = species;
    p.seed = 0xA11CE5EEDull ^ (static_cast<u64>(species.v) * 0x9E3779B97F4A7C15ull);
    p.age01 = 1.0f;
    PlantSkeleton skel;
    if (!gen->Generate(p, sp, skel)) return false;
    const Model model = BuildTreeMesh(skel, sp);
    if (model.empty()) return false;
    return uaf::WriteMesh(outPath, model, /*guid=*/0);
}

bool BakeSelfTest() {
    VegetationWorld world;
    RegisterDemoSpecies(world);
    const SpeciesId oak = world.Species().Find("demo_oak");
    if (!world.Species().Valid(oak)) return false;
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "hbe_vegbake_test.uaf";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    bool ok = BakeSpeciesToUaf(world, oak, tmp);
    if (ok) {
        // Round-trip: the baked asset must read back with intact woody geometry.
        const std::optional<Model> model = uaf::ReadMesh(tmp);
        ok = model && !model->empty() && !(*model)[0].vertices.empty() &&
             !(*model)[0].indices.empty();
    }
    std::filesystem::remove(tmp, ec);
    return ok;
}

bool PaintSelfTest() {
    Scene scene;
    auto& reg = scene.Registry();
    const auto plant = [&](f32 x, f32 z, const char* src) {
        const entt::entity e = scene.CreateEntity("Tree");
        Transform t;
        t.position = {x, 0.0f, z};
        reg.emplace<Transform>(e, t);
        reg.emplace<MeshRef>(e, MeshRef{src});
        return e;
    };
    plant(0.0f, 0.0f, "veg:demo_oak/woody");
    plant(0.0f, 0.0f, "veg:demo_oak/foliage");
    plant(1.0f, 0.0f, "veg:demo_oak/woody");
    plant(5.0f, 0.0f, "veg:demo_pine/woody");
    plant(10.0f, 10.0f, "veg:demo_oak/woody");
    // A non-vegetation mesh entity that must NEVER be erased.
    const entt::entity rock = scene.CreateEntity("Rock");
    Transform rt;
    rt.position = {0.5f, 0.0f, 0.5f};
    reg.emplace<Transform>(rock, rt);
    reg.emplace<MeshRef>(rock, MeshRef{"prim:cube"});

    const auto vegCount = [&]() {
        u32 n = 0;
        for (const entt::entity e : reg.view<MeshRef>())
            if (reg.get<MeshRef>(e).source.rfind("veg:", 0) == 0) ++n;
        return n;
    };

    bool ok = (vegCount() == 5);
    // Radius 2 at origin: the two at (0,0) + (1,0) = 3 removed; (5,0) and (10,10) survive.
    ok = ok && (EraseVegetationAt(scene, {0.0f, 0.0f}, 2.0f) == 3);
    ok = ok && reg.valid(rock) && (vegCount() == 2);
    // Big radius clears the rest; the rock (prim:) is never touched.
    ok = ok && (EraseVegetationAt(scene, {0.0f, 0.0f}, 1000.0f) == 2);
    ok = ok && reg.valid(rock) && (vegCount() == 0);

    // PERSISTENCE: a painted "veg:" MeshRef must survive save/load, and the loader must ask the
    // resolver to rebuild it (the whole point of the MeshRef approach - painted forests reload).
    {
        Scene s;
        auto& r2 = s.Registry();
        const entt::entity e = s.CreateEntity("Tree");
        Transform t;
        t.position = {3.0f, 0.0f, 4.0f};
        r2.emplace<Transform>(e, t);
        r2.emplace<MeshInstance>(e, MeshInstance{}); // invalid handle is fine (device-less)
        r2.emplace<MeshRef>(e, MeshRef{"veg:demo_oak/woody"});

        const std::string text = scene::SaveSceneToString(s);
        ok = ok && text.find("veg:demo_oak/woody") != std::string::npos; // source persisted verbatim

        scene::SceneData data;
        ok = ok && scene::ParseSceneString(text, data);

        // Recording resolver: the load MUST dispatch our exact source through it.
        std::string asked;
        scene::SetVegMeshResolver(
            [&](const std::string& src, Renderer&, AABB&, rhi::TextureHandle&) {
                asked = src;
                return rhi::MeshHandle{};
            });
        Scene loaded;
        Renderer renderer; // device-less: uploads return invalid handles (headless contract)
        scene::StagedAssets staged;
        scene::Instantiate(loaded, renderer, data, staged, scene::LoadMode::Replace);
        ok = ok && (asked == "veg:demo_oak/woody");
        scene::SetVegMeshResolver({}); // clear the hook so it can't leak past this test
    }
    return ok;
}

u32 SpawnDemoForest(Scene& scene, Renderer& renderer, VegetationWorld& world) {
    // --- Terrain: a hilly heightfield (its chunk meshes are built by terrain::Update). ---
    const entt::entity te = scene.CreateEntity("Terrain");
    scene.Registry().emplace<Transform>(te, Transform{});
    TerrainComponent tc;
    tc.chunks = 8;
    tc.resolution = 24;
    tc.chunkSize = 16.0f; // 128 m terrain
    tc.height = 7.0f;     // rolling hills
    tc.frequency = 0.03f;
    tc.octaves = 4;
    tc.color = {0.30f, 0.42f, 0.24f, 1.0f};
    scene.Registry().emplace<TerrainComponent>(te, tc);
    terrain::EnsureHeights(scene.Registry().get<TerrainComponent>(te)); // fill heights now

    // --- Species (programmatic for the demo; shared with the editor paint brush) ---
    SpeciesRegistry& reg = world.Species();
    RegisterDemoSpecies(world);
    const SpeciesId oakId = reg.Find("demo_oak");
    const SpeciesId pineId = reg.Find("demo_pine");

    // --- Biome + scatter ---
    Biome biome;
    biome.name = "demo_forest";
    biome.altitudeRange = {-100.0f, 100.0f};
    biome.slopeRangeDeg = {0.0f, 45.0f};
    biome.moistureRange = {0.0f, 1.0f};
    biome.baseDensity = 0.02f; // ~1 tree / 50 m^2 -> a sparse, walkable forest
    biome.waterExclusion = 0.5f;
    biome.species.push_back({oakId, 1.0f, 1.0f, -1});
    biome.species.push_back({pineId, 0.7f, 1.0f, -1});

    BiomeSet biomes;
    biomes.biomes.push_back(biome);

    SurfaceQueryFn surface = MakeTerrainSurfaceQuery(scene);
    VegShardContext ctx;
    ctx.worldSeed = 0x5EED0FE57ull;
    ctx.shardId = 1;
    const f32 half = tc.chunks * tc.chunkSize * 0.5f - 2.0f;
    ctx.aabbMin = {-half, -half};
    ctx.aabbMax = {half, half};
    ctx.surface = surface;
    ctx.noise = world.DefaultNoise();

    PlaceOut placements;
    if (IVegetationDistribution* dist = world.Distribution("poisson"))
        dist->Scatter(ctx, biomes, reg, placements);

    static VegetationMeshLibrary lib; // lives for the process (demo only)
    const u32 spawned = PopulateForest(scene, renderer, world, lib, placements, surface);

    // NOTE: the grass layer is now the GPU-COMPUTE path (veg::GrassGpuField), driven each
    // frame by the Engine's --vegdemo hook - not the CPU-instanced PopulateGrass patches.

    // A slowly-orbiting camera so the forest is actually viewable in the runtime (which
    // renders only through the scene's game camera).
    const entt::entity center = scene.CreateEntity("ForestCenter");
    Transform centerT;
    centerT.position = {0.0f, 4.0f, 0.0f};
    scene.Registry().emplace<Transform>(center, centerT);

    const entt::entity cam = scene.CreateEntity("DemoCamera");
    scene.Registry().emplace<Transform>(cam, Transform{});
    CameraComponent cc;
    cc.mode = CameraComponent::Mode::Orbit;
    cc.target = "ForestCenter";
    cc.distance = 52.0f;
    cc.pitch = 16.0f;
    cc.spinSpeed = 10.0f;
    cc.playerLook = false;
    cc.collide = false;
    cc.farZ = 400.0f;
    cc.primary = true;
    scene.Registry().emplace<CameraComponent>(cam, cc);
    HBE_INFO("vegdemo: scattered {} candidates, spawned {} trees ({} species meshes)",
             placements.Count(), spawned, lib.SpeciesCount());
    return spawned;
}

} // namespace hbe::veg

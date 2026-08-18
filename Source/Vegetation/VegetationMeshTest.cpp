// Vegetation/VegetationMeshTest.cpp - --test-vegmesh.
//
// Pins P3: the .hbspecies round-trip, the tubular mesher (valid, watertight-ish topology),
// the meshoptimizer LOD chain (present + monotonic), determinism (same skeleton -> identical
// geometry bytes), and sane bounds. Headless (mesh build needs no GPU).
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/TreeMesher.h"
#include "Vegetation/SpeciesAsset.h"
#include "Assets/Mesh.h"
#include "Core/Log.h"

#include <cstring>
#include <cmath>

namespace hbe::veg {
namespace {

int g_fail = 0;
#define VM_CHECK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) { HBE_ERROR("vegmesh: FAIL - {}", msg); ++g_fail; }       \
    } while (0)

bool IndicesInRange(const MeshData& m) {
    if (m.indices.size() % 3 != 0) return false;
    const u32 vc = m.VertexCount();
    for (u32 idx : m.indices)
        if (idx >= vc) return false;
    return true;
}

bool SameGeometry(const MeshData& a, const MeshData& b) {
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size())
        return false;
    if (a.indices != b.indices) return false;
    return std::memcmp(a.vertices.data(), b.vertices.data(),
                       a.vertices.size() * sizeof(Vertex)) == 0;
}

} // namespace

bool MeshSelfTest() {
    g_fail = 0;
    VegetationWorld world;

    // --- .hbspecies round-trip -------------------------------------------------------
    {
        Species s;
        s.name = "oak";
        s.strategy = GenStrategy::LSystem;
        s.maxHeight = 15.5f;
        s.trunkRadius = 0.42f;
        s.branchDensity = 0.7f;
        s.leafColor = {0.2f, 0.6f, 0.15f, 1.0f};
        s.barkMaterial = "materials/bark.hbmat";
        const std::string text = SpeciesToJson(s);
        Species r;
        VM_CHECK(ParseSpeciesJson(text, r), ".hbspecies JSON round-trips (parse)");
        VM_CHECK(r.name == s.name, "species name round-trips");
        VM_CHECK(r.strategy == GenStrategy::LSystem, "species strategy round-trips");
        VM_CHECK(std::abs(r.maxHeight - s.maxHeight) < 1e-4f, "species maxHeight round-trips");
        VM_CHECK(std::abs(r.trunkRadius - s.trunkRadius) < 1e-4f, "species trunkRadius round-trips");
        VM_CHECK(r.leafColor == s.leafColor, "species leafColor round-trips");
        VM_CHECK(r.barkMaterial == s.barkMaterial, "species barkMaterial round-trips");
    }

    // --- Generate a skeleton, mesh it ------------------------------------------------
    Species oak;
    oak.name = "oak";
    oak.maxHeight = 12.0f;
    oak.trunkRadius = 0.35f;
    oak.branchDensity = 0.8f;
    oak.leafDensity = 1.0f;

    IPlantGenerator* gen = world.Generator("minimal");
    VM_CHECK(gen != nullptr, "generator available");
    PlantSkeleton skel;
    if (gen) {
        PlantGenParams p; p.species = SpeciesId{0}; p.seed = 0x5EED1234u; p.age01 = 1.0f;
        VM_CHECK(gen->Generate(p, oak, skel), "generate skeleton");
    }

    Model model = BuildTreeMesh(skel, oak);
    VM_CHECK(!model.empty(), "mesher produced at least one submesh");
    if (!model.empty()) {
        const MeshData& woody = model[0];
        VM_CHECK(woody.VertexCount() > 0 && woody.IndexCount() > 0, "woody submesh has geometry");
        VM_CHECK(IndicesInRange(woody), "woody indices are a valid triangle list");

        // Bounds: the trunk should reach roughly the species height.
        glm::vec3 mn, mx;
        ComputeBounds(woody, mn, mx);
        VM_CHECK(mx.y > oak.maxHeight * 0.5f, "woody mesh reaches a plausible height");
        // Branches form a crown that extends above the trunk top, so the ceiling is the
        // trunk height plus the crown reach - not the bare height.
        VM_CHECK(mx.y <= oak.maxHeight + oak.crownWidth, "woody mesh stays within the crown envelope");

        // LOD chain: present + strictly decreasing triangle counts.
        VM_CHECK(!woody.lods.empty(), "woody mesh has a LOD chain");
        bool decreasing = true;
        usize prev = woody.indices.size();
        for (const MeshLod& l : woody.lods) {
            if (l.indices.size() >= prev) decreasing = false;
            prev = l.indices.size();
        }
        VM_CHECK(decreasing, "LODs have strictly decreasing triangle counts");

        // Foliage submesh present (oak has leaves).
        VM_CHECK(model.size() >= 2, "a foliage submesh was produced for a leafy species");
        if (model.size() >= 2)
            VM_CHECK(model[1].VertexCount() > 0, "foliage submesh has geometry");
    }

    // --- Determinism: same skeleton -> byte-identical geometry -----------------------
    {
        Model a = BuildTreeMesh(skel, oak);
        Model b = BuildTreeMesh(skel, oak);
        VM_CHECK(a.size() == b.size(), "mesher is deterministic (submesh count)");
        bool same = a.size() == b.size();
        for (usize i = 0; same && i < a.size(); ++i) {
            if (!SameGeometry(a[i], b[i])) same = false;
            if (a[i].lods.size() != b[i].lods.size()) same = false;
            for (usize k = 0; same && k < a[i].lods.size(); ++k)
                if (a[i].lods[k].indices != b[i].lods[k].indices) same = false;
        }
        VM_CHECK(same, "same skeleton -> byte-identical mesh + LODs");
    }

    // --- Foliage can be disabled -----------------------------------------------------
    {
        TreeMeshSettings noLeaves;
        noLeaves.foliage = false;
        Model m = BuildTreeMesh(skel, oak, noLeaves);
        VM_CHECK(m.size() == 1, "foliage=false yields only the woody submesh");
    }

    if (g_fail == 0) HBE_INFO("vegmesh: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg

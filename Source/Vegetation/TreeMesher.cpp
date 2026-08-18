// Vegetation/TreeMesher.cpp - PlantSkeleton -> Model (woody tubes + foliage cards).
#include "Vegetation/TreeMesher.h"
#include "Assets/MeshDerive.h"   // RecomputeNormalsTangents
#include "Assets/MeshOptimize.h" // OptimizeForGpu
#include "Assets/MeshSimplify.h" // BuildLodChain

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace hbe::veg {
namespace {

bool IsWoody(PlantPart k) {
    return k == PlantPart::Trunk || k == PlantPart::Branch || k == PlantPart::Twig ||
           k == PlantPart::Root;
}
bool IsFoliage(PlantPart k) {
    return k == PlantPart::LeafCluster || k == PlantPart::Leaf || k == PlantPart::Flower ||
           k == PlantPart::Fruit;
}

// A stable orthonormal frame whose Z is `dir`. Deterministic (no branch on FP compare
// beyond a fixed axis choice), so the whole mesh is reproducible.
void BasisFromDir(const glm::vec3& dir, glm::vec3& right, glm::vec3& up) {
    const glm::vec3 ref = (std::abs(dir.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    right = glm::normalize(glm::cross(ref, dir));
    up = glm::normalize(glm::cross(dir, right));
}

void AddTube(MeshData& md, const glm::vec3& a, const glm::vec3& b, f32 ra, f32 rb,
             u32 ring, f32 vBase) {
    glm::vec3 dir = b - a;
    const f32 len = glm::length(dir);
    if (len < 1e-5f) return;
    dir /= len;
    glm::vec3 right, up;
    BasisFromDir(dir, right, up);

    const u32 base = md.VertexCount();
    for (u32 k = 0; k <= ring; ++k) { // duplicate the seam vertex (k==ring) for clean UVs
        const f32 t = static_cast<f32>(k) / static_cast<f32>(ring);
        const f32 ang = t * glm::two_pi<f32>();
        const glm::vec3 radial = std::cos(ang) * right + std::sin(ang) * up;
        Vertex va, vb;
        va.position = a + radial * ra;
        va.normal = radial;
        va.uv = glm::vec2(t, vBase);
        vb.position = b + radial * rb;
        vb.normal = radial;
        vb.uv = glm::vec2(t, vBase + 1.0f);
        md.vertices.push_back(va);
        md.vertices.push_back(vb);
    }
    // Two triangles per ring quad. Each k contributes verts (2k = a_k, 2k+1 = b_k).
    // WINDING: the ring runs counter-clockwise around +dir, so `cross(dir, tangent)` points
    // radially INWARD - the naive (a0,b0,b1) order therefore faces inward and the trunk is
    // lit from the inside (RecomputeNormals derives the normal from the winding). Emit the
    // FLIPPED order so faces (and the recomputed normals) point outward.
    for (u32 k = 0; k < ring; ++k) {
        const u32 a0 = base + 2 * k;
        const u32 b0 = base + 2 * k + 1;
        const u32 a1 = base + 2 * (k + 1);
        const u32 b1 = base + 2 * (k + 1) + 1;
        md.indices.push_back(a0); md.indices.push_back(b1); md.indices.push_back(b0);
        md.indices.push_back(a0); md.indices.push_back(a1); md.indices.push_back(b1);
    }
}

// A crossed-quad leaf card: two perpendicular quads centered at `p`. EVERY vertex is given the
// same `canopyN` normal (the smooth outward-spherical canopy normal), NOT the card-face normal -
// so the whole canopy shades like a soft sphere instead of a jumble of hard flat cards, which is
// what makes distant/mid foliage read as a natural crown. The card is randomly rolled and its UVs
// flipped per `seed` so a texture cutout does not tile visibly. Normals are authored here and must
// survive to the GPU: Finish() runs a tangents-ONLY recompute on foliage (RecomputeNormals would
// average the crossed quads' opposing faces to ~zero and the leaves would render black).
void AddLeafCard(MeshData& md, const glm::vec3& p, const glm::vec3& canopyN, f32 size, u32 seed) {
    const auto rnd = [&](u32 salt) {
        u32 x = (seed * 747796405u + 2891336453u) ^ (salt * 2654435761u);
        x = (x >> 15) ^ x;
        return static_cast<f32>(x & 0xFFFFu) / 65535.0f;
    };
    glm::vec3 right, up;
    // Orient the card generally toward the canopy normal but biased upright.
    BasisFromDir(glm::normalize(canopyN + glm::vec3(0.0f, 0.5f, 0.0f)), right, up);
    const f32 s2 = size * (0.8f + 0.5f * rnd(1));
    const f32 h = s2 * 0.5f;
    const f32 roll = rnd(2) * glm::two_pi<f32>();
    const glm::vec3 r2 = right * std::cos(roll) + up * std::sin(roll);
    const glm::vec3 u2 = -right * std::sin(roll) + up * std::cos(roll);
    const auto quad = [&](const glm::vec3& u, const glm::vec3& v, bool flipU) {
        const u32 base = md.VertexCount();
        const glm::vec3 corners[4] = {p - u * h - v * h, p + u * h - v * h,
                                      p + u * h + v * h, p - u * h + v * h};
        const f32 u0 = flipU ? 1.0f : 0.0f, u1 = flipU ? 0.0f : 1.0f;
        const glm::vec2 uvs[4] = {{u0, 0.0f}, {u1, 0.0f}, {u1, 1.0f}, {u0, 1.0f}};
        for (int i = 0; i < 4; ++i) {
            Vertex vert;
            vert.position = corners[i];
            vert.normal = canopyN; // soft spherical canopy normal (NOT the flat card face)
            vert.uv = uvs[i];
            md.vertices.push_back(vert);
        }
        md.indices.push_back(base + 0); md.indices.push_back(base + 1); md.indices.push_back(base + 2);
        md.indices.push_back(base + 0); md.indices.push_back(base + 2); md.indices.push_back(base + 3);
    };
    quad(r2, u2, rnd(3) > 0.5f);
    quad(glm::normalize(r2 + u2), glm::normalize(u2 - r2), rnd(4) > 0.5f); // perpendicular cross
}

void Finish(MeshData& md, const TreeMeshSettings& s, bool recomputeNormals) {
    if (md.Empty()) return;
    // Foliage authors its own (canopy) normals; only rebuild tangents there. Woody meshes get
    // the full normal+tangent recompute from their watertight tube geometry.
    if (recomputeNormals) mesh::RecomputeNormalsTangents(md);
    else mesh::RecomputeTangents(md);
    if (s.optimize) mesh::OptimizeForGpu(md);
    if (s.buildLods) mesh::BuildLodChain(md);
}

} // namespace

Model BuildTreeMesh(const PlantSkeleton& skel, const Species& sp, const TreeMeshSettings& s) {
    Model model;
    if (skel.NodeCount() == 0) return model;

    MeshData woody;
    woody.name = sp.name + "_woody";
    woody.material.baseColor = sp.barkColor;
    woody.material.roughness = 0.85f;
    if (!sp.barkMaterial.empty()) woody.material.materialAsset = sp.barkMaterial;

    MeshData leaves;
    leaves.name = sp.name + "_foliage";
    leaves.material.baseColor = sp.leafColor;
    leaves.material.roughness = 0.6f;
    if (!sp.leafMaterial.empty()) leaves.material.materialAsset = sp.leafMaterial;

    const u32 ring = glm::max(3u, s.ringSegments);
    const u32 n = skel.NodeCount();

    // Canopy centroid (mean of foliage nodes) - each leaf's normal points outward from it, so
    // the crown shades as one soft sphere. Falls back to a point up the trunk if there are no
    // leaves yet.
    glm::vec3 canopySum(0.0f);
    u32 leafN = 0;
    for (u32 i = 0; i < n; ++i)
        if (IsFoliage(static_cast<PlantPart>(skel.kind[i]))) { canopySum += skel.pos[i]; ++leafN; }
    const glm::vec3 canopyCenter =
        leafN > 0 ? canopySum / static_cast<f32>(leafN) : glm::vec3(0.0f, sp.maxHeight * 0.6f, 0.0f);

    for (u32 i = 0; i < n; ++i) {
        const PlantPart kind = static_cast<PlantPart>(skel.kind[i]);
        const i32 par = skel.parent[i];

        if (IsWoody(kind) && par >= 0) {
            AddTube(woody, skel.pos[par], skel.pos[i], skel.radius[par], skel.radius[i], ring,
                    static_cast<f32>(i));
        } else if (s.foliage && IsFoliage(kind)) {
            // Outward-and-up spherical canopy normal (always has a +Y component so it never
            // points into the crown or degenerates at the centroid).
            const glm::vec3 d = skel.pos[i] - canopyCenter;
            const glm::vec3 canopyN =
                glm::normalize(glm::vec3(d.x, std::abs(d.y) + 0.6f, d.z));
            AddLeafCard(leaves, skel.pos[i], canopyN, skel.radius[i] * s.leafScale, i + 1u);
        }
    }

    Finish(woody, s, /*recomputeNormals=*/true);
    if (!woody.Empty()) model.push_back(std::move(woody));
    if (s.foliage) {
        Finish(leaves, s, /*recomputeNormals=*/false); // preserve the authored canopy normals
        if (!leaves.Empty()) model.push_back(std::move(leaves));
    }
    return model;
}

} // namespace hbe::veg

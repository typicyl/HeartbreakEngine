// Assets/MeshOptimize.cpp - see MeshOptimize.h.
#include "Assets/MeshOptimize.h"

#include "Core/Log.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace hbe::mesh {

namespace {

f32 AnalyzeAcmr(const std::vector<u32>& indices, u32 vertexCount) {
    if (indices.empty() || vertexCount == 0) return 0.0f;
    // 16-entry FIFO model is meshopt's default reference cache; warp/primgroup 0 = off.
    const meshopt_VertexCacheStatistics s = meshopt_analyzeVertexCache(
        indices.data(), indices.size(), vertexCount, 16, 0, 0);
    return s.acmr;
}

} // namespace

void OptimizeForGpu(MeshData& md, OptimizeStats* out) {
    const usize indexCount = md.indices.size();
    const usize vertexCount = md.vertices.size();
    if (indexCount == 0 || vertexCount == 0) {
        if (out) *out = {};
        return;
    }

    const f32 acmrBefore = AnalyzeAcmr(md.indices, static_cast<u32>(vertexCount));

    // --- Optional weld: dedup binary-identical vertices. SKIPPED when the mesh carries
    // morph targets, because welding could merge two coincident base vertices that hold
    // DIFFERENT morph deltas and corrupt the blendshape. Assimp's JoinIdenticalVertices
    // usually leaves little to weld, so this mainly cleans up soups that were not indexed. ---
    if (md.morphTargets.empty()) {
        std::vector<unsigned int> remap(vertexCount);
        const usize unique = meshopt_generateVertexRemap(
            remap.data(), md.indices.data(), indexCount, md.vertices.data(), vertexCount,
            sizeof(Vertex));
        if (unique < vertexCount) {
            std::vector<Vertex> welded(unique);
            meshopt_remapVertexBuffer(welded.data(), md.vertices.data(), vertexCount,
                                      sizeof(Vertex), remap.data());
            std::vector<u32> weldedIdx(indexCount);
            meshopt_remapIndexBuffer(weldedIdx.data(), md.indices.data(), indexCount,
                                     remap.data());
            md.vertices.swap(welded);
            md.indices.swap(weldedIdx);
        }
    }

    const usize vc = md.vertices.size();

    // --- Reorder passes. Both rewrite only the INDEX buffer, so they never touch the
    // vertex array and are morph-safe by construction. Order matters: overdraw wants a
    // cache-optimized list to start from. Both support in-place (dst == src). ---
    meshopt_optimizeVertexCache(md.indices.data(), md.indices.data(), indexCount, vc);
    meshopt_optimizeOverdraw(md.indices.data(), md.indices.data(), indexCount,
                             &md.vertices[0].position.x, vc, sizeof(Vertex),
                             /*threshold*/ 1.05f); // allow <=5% ACMR loss to cut overdraw

    // --- Vertex fetch: permute the vertex buffer so access order matches the index order
    // (and drop any now-unreferenced vertices). We take the REMAP explicitly rather than
    // let meshopt rewrite the vertex buffer, so the same remap can be applied to every
    // morph target's parallel delta arrays. ---
    std::vector<unsigned int> fetchRemap(vc);
    const usize finalVc =
        meshopt_optimizeVertexFetchRemap(fetchRemap.data(), md.indices.data(), indexCount, vc);

    std::vector<Vertex> fetched(finalVc);
    meshopt_remapVertexBuffer(fetched.data(), md.vertices.data(), vc, sizeof(Vertex),
                              fetchRemap.data());
    meshopt_remapIndexBuffer(md.indices.data(), md.indices.data(), indexCount,
                             fetchRemap.data());
    md.vertices.swap(fetched);

    for (MorphTarget& mt : md.morphTargets) {
        if (mt.posDelta.size() == vc) {
            std::vector<glm::vec3> np(finalVc);
            meshopt_remapVertexBuffer(np.data(), mt.posDelta.data(), vc, sizeof(glm::vec3),
                                      fetchRemap.data());
            mt.posDelta.swap(np);
        }
        if (mt.nrmDelta.size() == vc) {
            std::vector<glm::vec3> nn(finalVc);
            meshopt_remapVertexBuffer(nn.data(), mt.nrmDelta.data(), vc, sizeof(glm::vec3),
                                      fetchRemap.data());
            mt.nrmDelta.swap(nn);
        }
    }

    if (out) {
        out->inputVertices = static_cast<u32>(vertexCount);
        out->outputVertices = static_cast<u32>(finalVc);
        out->triangles = static_cast<u32>(indexCount / 3);
        out->acmrBefore = acmrBefore;
        out->acmrAfter = AnalyzeAcmr(md.indices, static_cast<u32>(finalVc));
    }
}

// ---------------------------------------------------------------------------
// Self-test (--test-meshopt)
// ---------------------------------------------------------------------------
namespace {

// A triangle as its three vertex POSITIONS, made order-independent by sorting the corners.
// Two meshes are the same surface iff their multisets of these match, regardless of how the
// vertices/indices were reordered. Positions are copied verbatim by the optimizer, so an
// exact bit compare is correct here (no arithmetic is applied to them).
using Tri = std::array<std::array<f32, 3>, 3>;

Tri MakeTri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    Tri t{{{{a.x, a.y, a.z}}, {{b.x, b.y, b.z}}, {{c.x, c.y, c.z}}}};
    std::sort(t.begin(), t.end());
    return t;
}

std::vector<Tri> TriSet(const MeshData& m) {
    std::vector<Tri> tris;
    tris.reserve(m.indices.size() / 3);
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        tris.push_back(MakeTri(m.vertices[m.indices[i]].position,
                               m.vertices[m.indices[i + 1]].position,
                               m.vertices[m.indices[i + 2]].position));
    }
    std::sort(tris.begin(), tris.end());
    return tris;
}

// A deterministic "morph" so we can prove each vertex's delta rode along with its vertex.
glm::vec3 MorphOf(const glm::vec3& p) { return glm::vec3(p.x * 2.0f, p.z - 1.0f, 5.0f); }

// An NxN grid of unique-position vertices, triangulated. Deliberately laid out in a
// cache-hostile order (every quad references far-apart rows) so the optimizer has work to do.
MeshData MakeGrid(int n, bool withMorph) {
    MeshData md;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            Vertex v;
            v.position = glm::vec3(static_cast<f32>(x), 0.0f, static_cast<f32>(z));
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            md.vertices.push_back(v);
        }
    }
    for (int z = 0; z < n - 1; ++z) {
        for (int x = 0; x < n - 1; ++x) {
            const u32 i0 = static_cast<u32>(z * n + x);
            const u32 i1 = static_cast<u32>(z * n + x + 1);
            const u32 i2 = static_cast<u32>((z + 1) * n + x);
            const u32 i3 = static_cast<u32>((z + 1) * n + x + 1);
            md.indices.insert(md.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    if (withMorph) {
        MorphTarget mt;
        mt.name = "test";
        mt.posDelta.resize(md.vertices.size());
        mt.nrmDelta.resize(md.vertices.size());
        for (usize i = 0; i < md.vertices.size(); ++i) {
            mt.posDelta[i] = MorphOf(md.vertices[i].position);
            mt.nrmDelta[i] = MorphOf(md.vertices[i].position) * 0.5f;
        }
        md.morphTargets.push_back(std::move(mt));
    }
    return md;
}

bool VecEq(const glm::vec3& a, const glm::vec3& b) {
    return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0; // deltas are copied, so exact
}

} // namespace

bool OptimizeSelfTest() {
    // 1) Topology preserved + morph deltas stay glued to their vertices + determinism.
    {
        MeshData a = MakeGrid(16, /*withMorph*/ true);
        const std::vector<Tri> before = TriSet(a);
        const usize triCount = a.indices.size() / 3;

        MeshData b = a; // copy for the determinism check
        OptimizeForGpu(a);
        OptimizeForGpu(b);

        if (a.indices.size() != triCount * 3) {
            HBE_ERROR("meshopt: triangle count changed ({} -> {}).", triCount * 3,
                      a.indices.size());
            return false;
        }
        if (TriSet(a) != before) {
            HBE_ERROR("meshopt: surface changed - triangle position set differs after optimize.");
            return false;
        }
        // Every surviving vertex must still carry the morph delta computed FROM ITS OWN
        // position. If the remap missed the parallel arrays, this fails.
        if (a.morphTargets.size() != 1 || a.morphTargets[0].posDelta.size() != a.vertices.size() ||
            a.morphTargets[0].nrmDelta.size() != a.vertices.size()) {
            HBE_ERROR("meshopt: morph arrays desized ({} verts vs {}/{} deltas).",
                      a.vertices.size(), a.morphTargets[0].posDelta.size(),
                      a.morphTargets[0].nrmDelta.size());
            return false;
        }
        for (usize i = 0; i < a.vertices.size(); ++i) {
            const glm::vec3 expectP = MorphOf(a.vertices[i].position);
            if (!VecEq(a.morphTargets[0].posDelta[i], expectP) ||
                !VecEq(a.morphTargets[0].nrmDelta[i], expectP * 0.5f)) {
                HBE_ERROR("meshopt: morph delta misaligned at vertex {} after reorder.", i);
                return false;
            }
        }
        // Determinism: same input -> byte-identical output on both index and vertex streams.
        if (a.indices != b.indices || a.vertices.size() != b.vertices.size() ||
            std::memcmp(a.vertices.data(), b.vertices.data(),
                        a.vertices.size() * sizeof(Vertex)) != 0) {
            HBE_ERROR("meshopt: non-deterministic output.");
            return false;
        }
    }

    // 2) Cache order actually improves on a hostile input (and vertex-fetch drops nothing
    //    it should not on a fully-referenced grid).
    {
        MeshData g = MakeGrid(32, /*withMorph*/ false);
        OptimizeStats st;
        const usize vertsIn = g.vertices.size();
        OptimizeForGpu(g, &st);
        if (st.acmrAfter > st.acmrBefore + 1e-4f) {
            HBE_ERROR("meshopt: ACMR got worse ({} -> {}).", st.acmrBefore, st.acmrAfter);
            return false;
        }
        if (g.vertices.size() != vertsIn) { // grid has unique positions + all are referenced
            HBE_ERROR("meshopt: fully-referenced grid lost vertices ({} -> {}).", vertsIn,
                      g.vertices.size());
            return false;
        }
    }

    // 3) Weld path: an UN-indexed triangle soup with shared positions must collapse to far
    //    fewer vertices while describing the exact same surface.
    {
        MeshData grid = MakeGrid(8, /*withMorph*/ false);
        MeshData soup; // expand every triangle into 3 fresh vertices (no sharing)
        for (usize i = 0; i < grid.indices.size(); ++i) {
            soup.vertices.push_back(grid.vertices[grid.indices[i]]);
            soup.indices.push_back(static_cast<u32>(i));
        }
        const std::vector<Tri> before = TriSet(soup);
        const usize soupVerts = soup.vertices.size();
        OptimizeForGpu(soup);
        if (soup.vertices.size() >= soupVerts) {
            HBE_ERROR("meshopt: weld did not reduce a duplicated soup ({} -> {}).", soupVerts,
                      soup.vertices.size());
            return false;
        }
        if (TriSet(soup) != before) {
            HBE_ERROR("meshopt: weld changed the surface.");
            return false;
        }
    }

    HBE_INFO("meshopt: OptimizeSelfTest passed.");
    return true;
}

} // namespace hbe::mesh

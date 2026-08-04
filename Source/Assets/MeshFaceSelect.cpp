#include "Assets/MeshFaceSelect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace hbe::mesh {

namespace {

glm::vec3 FaceNormal(const MeshData& m, u32 tri) {
    const u32 i = tri * 3;
    if (i + 2 >= m.indices.size()) return glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3& a = m.vertices[m.indices[i]].position;
    const glm::vec3& b = m.vertices[m.indices[i + 1]].position;
    const glm::vec3& c = m.vertices[m.indices[i + 2]].position;
    const glm::vec3 n = glm::cross(b - a, c - a);
    const f32 len = glm::length(n);
    return len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Adjacency is built over POSITION-welded vertices. An imported mesh is split at UV and
// normal seams, so two triangles that visibly share an edge often do not share indices - and
// adjacency built on raw indices would stop dead at every seam, making "select this panel"
// select a fraction of it.
std::vector<u32> WeldPositions(const MeshData& m) {
    struct Key {
        i64 x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct Hash {
        usize operator()(const Key& k) const {
            u64 h = 1469598103934665603ull;
            for (i64 v : {k.x, k.y, k.z}) { h ^= static_cast<u64>(v); h *= 1099511628211ull; }
            return static_cast<usize>(h);
        }
    };
    std::unordered_map<Key, u32, Hash> map;
    std::vector<u32> weld(m.vertices.size());
    map.reserve(m.vertices.size());
    constexpr f64 kQuant = 1e5; // 0.01 mm
    u32 next = 0;
    for (usize i = 0; i < m.vertices.size(); ++i) {
        const glm::vec3& p = m.vertices[i].position;
        const Key k{static_cast<i64>(std::llround(p.x * kQuant)),
                    static_cast<i64>(std::llround(p.y * kQuant)),
                    static_cast<i64>(std::llround(p.z * kQuant))};
        auto it = map.find(k);
        if (it == map.end()) { map.emplace(k, next); weld[i] = next++; }
        else weld[i] = it->second;
    }
    return weld;
}

} // namespace

i32 RaycastTriangle(const MeshData& mesh, const glm::vec3& origin, const glm::vec3& dir,
                    f32* outT) {
    i32 best = -1;
    f32 bestT = 1e30f;
    const u32 triCount = static_cast<u32>(mesh.indices.size() / 3);
    for (u32 t = 0; t < triCount; ++t) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t * 3]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
        // Moller-Trumbore. Two-sided deliberately: a mesh being authored may have inverted
        // or single-sided faces, and refusing to select them is worse than selecting them.
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 pv = glm::cross(dir, e2);
        const f32 det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-12f) continue;
        const f32 inv = 1.0f / det;
        const glm::vec3 tv = origin - a;
        const f32 u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 qv = glm::cross(tv, e1);
        const f32 v = glm::dot(dir, qv) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const f32 tt = glm::dot(e2, qv) * inv;
        if (tt <= 1e-6f || tt >= bestT) continue;
        bestT = tt;
        best = static_cast<i32>(t);
    }
    if (best >= 0 && outT) *outT = bestT;
    return best;
}

std::vector<u32> SelectConnected(const MeshData& mesh, u32 seed, f32 maxAngleDegrees) {
    const u32 triCount = static_cast<u32>(mesh.indices.size() / 3);
    if (seed >= triCount) return {};
    const std::vector<u32> weld = WeldPositions(mesh);

    // Edge -> the (up to two) triangles using it.
    std::unordered_map<u64, std::pair<i32, i32>> edgeTris;
    edgeTris.reserve(triCount * 3);
    const auto key = [](u32 a, u32 b) {
        return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
    };
    for (u32 t = 0; t < triCount; ++t) {
        const u32 v[3] = {weld[mesh.indices[t * 3]], weld[mesh.indices[t * 3 + 1]],
                          weld[mesh.indices[t * 3 + 2]]};
        for (int e = 0; e < 3; ++e) {
            auto& slot = edgeTris[key(v[e], v[(e + 1) % 3])];
            if (slot.first == 0 && slot.second == 0) { slot = {static_cast<i32>(t), -1}; }
            else if (slot.second < 0) slot.second = static_cast<i32>(t);
        }
    }

    const f32 cosLimit = std::cos(glm::radians(std::clamp(maxAngleDegrees, 0.0f, 180.0f)));
    std::vector<bool> visited(triCount, false);
    std::vector<u32> out, stack{seed};
    visited[seed] = true;
    while (!stack.empty()) {
        const u32 t = stack.back();
        stack.pop_back();
        out.push_back(t);
        const glm::vec3 n = FaceNormal(mesh, t);
        const u32 v[3] = {weld[mesh.indices[t * 3]], weld[mesh.indices[t * 3 + 1]],
                          weld[mesh.indices[t * 3 + 2]]};
        for (int e = 0; e < 3; ++e) {
            const auto it = edgeTris.find(key(v[e], v[(e + 1) % 3]));
            if (it == edgeTris.end()) continue;
            for (const i32 nb : {it->second.first, it->second.second}) {
                if (nb < 0 || static_cast<u32>(nb) == t) continue;
                if (visited[static_cast<usize>(nb)]) continue;
                // The DIHEDRAL angle across the shared edge is what makes this useful: it
                // stops at the corner of a box and flows across a curved panel.
                if (glm::dot(n, FaceNormal(mesh, static_cast<u32>(nb))) < cosLimit) continue;
                visited[static_cast<usize>(nb)] = true;
                stack.push_back(static_cast<u32>(nb));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u32> SelectSimilarFacing(const MeshData& mesh, u32 seed, f32 maxAngleDegrees) {
    const u32 triCount = static_cast<u32>(mesh.indices.size() / 3);
    if (seed >= triCount) return {};
    const glm::vec3 n = FaceNormal(mesh, seed);
    const f32 cosLimit = std::cos(glm::radians(std::clamp(maxAngleDegrees, 0.0f, 180.0f)));
    std::vector<u32> out;
    for (u32 t = 0; t < triCount; ++t)
        if (glm::dot(n, FaceNormal(mesh, t)) >= cosLimit) out.push_back(t);
    return out;
}

SplitResult SplitByFaces(const MeshData& src, const std::vector<u32>& faces) {
    SplitResult r;
    const u32 triCount = static_cast<u32>(src.indices.size() / 3);
    std::vector<bool> take(triCount, false);
    u32 taken = 0;
    for (const u32 f : faces)
        if (f < triCount && !take[f]) { take[f] = true; ++taken; }

    r.remainder.name = src.name;
    r.remainder.material = src.material;
    r.extracted.name = src.name + " (split)";
    r.extracted.material = src.material; // the caller replaces this with the new one
    if (taken == 0) { r.remainder = src; return r; }
    if (taken == triCount) {
        // NOTHING TO SPLIT. Every face is selected, so the honest operation is to change this
        // submesh's material - splitting would leave an empty submesh behind, and an empty
        // submesh shifts nothing but is a permanent piece of litter in the asset.
        r.tookEverything = true;
        r.extracted = src;
        r.extracted.name = src.name;
        return r;
    }

    // Each half gets its OWN vertex buffer, so a vertex used by both is duplicated. That is
    // not waste - it is what a submesh is.
    std::vector<u32> mapKeep(src.vertices.size(), 0xFFFFFFFFu);
    std::vector<u32> mapTake(src.vertices.size(), 0xFFFFFFFFu);
    // Morph targets index the ORIGINAL vertex set, so they cannot survive a resplit intact.
    // Dropping them is deliberate: a silently misaligned blendshape is far worse than a
    // missing one, and the same rule the decimator follows.
    const auto emit = [&](MeshData& dst, std::vector<u32>& map, u32 srcIndex) {
        if (map[srcIndex] == 0xFFFFFFFFu) {
            map[srcIndex] = static_cast<u32>(dst.vertices.size());
            dst.vertices.push_back(src.vertices[srcIndex]);
        }
        dst.indices.push_back(map[srcIndex]);
    };
    for (u32 t = 0; t < triCount; ++t) {
        MeshData& dst = take[t] ? r.extracted : r.remainder;
        std::vector<u32>& map = take[t] ? mapTake : mapKeep;
        for (u32 k = 0; k < 3; ++k) emit(dst, map, src.indices[t * 3 + k]);
    }
    return r;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("faceselect FAIL: %s\n", what); ++g_fails; }
}

// A unit cube, 12 triangles, with every face split at its edges the way an importer would
// leave it (24 vertices, not 8) - so the tests exercise the welding path.
MeshData Cube() {
    MeshData m;
    const glm::vec3 n[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    const glm::vec3 u[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
    for (int f = 0; f < 6; ++f) {
        const glm::vec3 v = glm::cross(n[f], u[f]);
        for (int c = 0; c < 4; ++c) {
            Vertex vert;
            const f32 su = (c == 1 || c == 2) ? 1.0f : -1.0f;
            const f32 sv = (c >= 2) ? 1.0f : -1.0f;
            vert.position = (n[f] + u[f] * su + v * sv) * 0.5f;
            vert.normal = n[f];
            vert.uv = {su * 0.5f + 0.5f, sv * 0.5f + 0.5f};
            m.vertices.push_back(vert);
        }
        const u32 b = static_cast<u32>(f * 4);
        m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    return m;
}
} // namespace

bool FaceSelectSelfTest() {
    g_fails = 0;
    const MeshData cube = Cube();
    Check(cube.indices.size() == 36, "the test cube must have 12 triangles");

    // ---- picking ----
    {
        f32 t = 0.0f;
        // Straight at the +Z face from outside.
        const i32 hit = RaycastTriangle(cube, {0.0f, 0.0f, 5.0f}, {0, 0, -1}, &t);
        Check(hit >= 0, "a ray at the cube must hit something");
        if (hit >= 0) {
            Check(std::abs(t - 4.5f) < 1e-3f, "the hit must be on the NEAR face, not the far one");
            const glm::vec3 n = FaceNormal(cube, static_cast<u32>(hit));
            Check(n.z > 0.9f, "the hit face must be the one facing the ray");
        }
        Check(RaycastTriangle(cube, {5.0f, 5.0f, 5.0f}, {0, 1, 0}, nullptr) < 0,
              "a ray that misses must report no hit");
        // A ray from INSIDE must still hit - meshes being authored are often inside-out.
        Check(RaycastTriangle(cube, {0, 0, 0}, {0, 0, 1}, nullptr) >= 0,
              "picking must be two-sided; refusing back faces makes authoring impossible");
    }

    // ---- connected selection stops at a corner ----
    {
        const i32 seed = RaycastTriangle(cube, {0.0f, 0.0f, 5.0f}, {0, 0, -1}, nullptr);
        Check(seed >= 0, "seed");
        const std::vector<u32> face = SelectConnected(cube, static_cast<u32>(seed), 30.0f);
        Check(face.size() == 2,
              "SELECTING A CUBE FACE MUST GIVE EXACTLY ITS 2 TRIANGLES - the 90 degree corner "
              "must stop the flood, or 'select this panel' selects the whole object");
        // With the limit opened past 90 degrees it should flow round the whole cube.
        const std::vector<u32> all = SelectConnected(cube, static_cast<u32>(seed), 120.0f);
        Check(all.size() == 12, "a wide angle limit must flow across every edge of the cube");
    }

    // ---- similar-facing selection ----
    {
        const i32 seed = RaycastTriangle(cube, {0.0f, 5.0f, 0.0f}, {0, -1, 0}, nullptr);
        Check(seed >= 0, "top seed");
        const std::vector<u32> up = SelectSimilarFacing(cube, static_cast<u32>(seed), 10.0f);
        Check(up.size() == 2, "only the two top triangles face up");
    }

    // ---- the split ----
    {
        const i32 seed = RaycastTriangle(cube, {0.0f, 0.0f, 5.0f}, {0, 0, -1}, nullptr);
        const std::vector<u32> face = SelectConnected(cube, static_cast<u32>(seed), 30.0f);
        const SplitResult r = SplitByFaces(cube, face);

        Check(!r.tookEverything, "a partial selection is a real split");
        Check(r.extracted.indices.size() == 6, "the extracted submesh must hold the 2 faces");
        Check(r.remainder.indices.size() == 30, "the remainder must hold the other 10");
        Check(r.extracted.indices.size() + r.remainder.indices.size() == cube.indices.size(),
              "NO TRIANGLE MAY BE LOST OR DUPLICATED by a split");
        for (const u32 i : r.extracted.indices)
            Check(i < r.extracted.vertices.size(), "extracted indices must be in range");
        for (const u32 i : r.remainder.indices)
            Check(i < r.remainder.vertices.size(), "remainder indices must be in range");

        // The geometry must be UNMOVED - a split regroups triangles, it does not edit them.
        const auto centroid = [](const MeshData& m, u32 tri) {
            return (m.vertices[m.indices[tri * 3]].position +
                    m.vertices[m.indices[tri * 3 + 1]].position +
                    m.vertices[m.indices[tri * 3 + 2]].position) / 3.0f;
        };
        Check(glm::length(centroid(r.extracted, 0) - centroid(cube, face[0])) < 1e-6f,
              "a split face must be in exactly the same place afterwards");
        Check(r.remainder.material.name == cube.material.name,
              "the remainder keeps the ORIGINAL material");
    }

    // ---- selecting everything is a material swap, not a split ----
    {
        std::vector<u32> all(12);
        for (u32 i = 0; i < 12; ++i) all[i] = i;
        const SplitResult r = SplitByFaces(cube, all);
        Check(r.tookEverything,
              "selecting every face must report that the caller should SWAP the material "
              "rather than leave an empty submesh behind");
        Check(r.remainder.indices.empty(), "nothing remains");
        Check(r.extracted.indices.size() == cube.indices.size(), "everything moved across");
    }

    // ---- guards ----
    {
        const SplitResult none = SplitByFaces(cube, {});
        Check(none.remainder.indices.size() == cube.indices.size(),
              "an empty selection must leave the mesh untouched");
        Check(none.extracted.indices.empty(), "...and extract nothing");
        // Out-of-range and duplicate face ids must not corrupt anything.
        const SplitResult odd = SplitByFaces(cube, {0, 0, 0, 9999});
        Check(odd.extracted.indices.size() == 3, "duplicates count once, bad ids are ignored");
        Check(odd.remainder.indices.size() == 33, "and the remainder is exact");
    }

    // ---- morph targets are dropped rather than misaligned ----
    {
        MeshData withMorph = cube;
        MorphTarget mt;
        mt.name = "t";
        mt.posDelta.assign(withMorph.vertices.size(), glm::vec3(1.0f));
        withMorph.morphTargets.push_back(mt);
        const SplitResult r = SplitByFaces(withMorph, {0, 1});
        Check(r.extracted.morphTargets.empty() && r.remainder.morphTargets.empty(),
              "A SPLIT MUST DROP MORPH TARGETS - their deltas index the original vertex set, "
              "so carrying them across would misalign every blendshape");
    }

    if (g_fails == 0)
        std::printf("faceselect: two-sided triangle picking, flood select that stops at a "
                    "90 degree corner, similar-facing select, and a split that loses no "
                    "triangle, moves no vertex, keeps the original material on the remainder "
                    "and reports when a swap is the right operation instead\n");
    return g_fails == 0;
}

} // namespace hbe::mesh

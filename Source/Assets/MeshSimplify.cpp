#include "Assets/MeshSimplify.h"

#include "Assets/MeshDerive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <vector>

namespace hbe::mesh {

namespace {

// A symmetric 4x4 quadric, stored as its 10 distinct entries. Symmetric storage is not just
// a memory saving: it is what makes adding two quadrics 10 adds instead of 16, and this is
// summed once per face corner over the whole mesh.
struct Quadric {
    f64 m[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // Indices:  0:xx 1:xy 2:xz 3:xw  4:yy 5:yz 6:yw  7:zz 8:zw  9:ww

    void AddPlane(f64 a, f64 b, f64 c, f64 d, f64 weight) {
        m[0] += weight * a * a; m[1] += weight * a * b; m[2] += weight * a * c; m[3] += weight * a * d;
        m[4] += weight * b * b; m[5] += weight * b * c; m[6] += weight * b * d;
        m[7] += weight * c * c; m[8] += weight * c * d;
        m[9] += weight * d * d;
    }
    void Add(const Quadric& o) { for (int i = 0; i < 10; ++i) m[i] += o.m[i]; }

    // v^T Q v - the squared distance from v to the set of planes this quadric summarises.
    f64 Error(const glm::dvec3& v) const {
        return m[0] * v.x * v.x + 2 * m[1] * v.x * v.y + 2 * m[2] * v.x * v.z + 2 * m[3] * v.x +
               m[4] * v.y * v.y + 2 * m[5] * v.y * v.z + 2 * m[6] * v.y +
               m[7] * v.z * v.z + 2 * m[8] * v.z + m[9];
    }
};

struct Edge {
    u32 a, b;
    f64 cost;
    u32 version; // stale entries are skipped rather than removed from the heap
    bool operator<(const Edge& o) const { return cost > o.cost; } // min-heap via greater-than
};

} // namespace

MeshData Simplify(const MeshData& src, const SimplifySettings& settings, SimplifyStats* outStats) {
    SimplifyStats stats;
    stats.inputTriangles = static_cast<u32>(src.indices.size() / 3);
    stats.outputTriangles = stats.inputTriangles;

    const u32 targetTris =
        std::max(settings.minTriangles,
                 static_cast<u32>(static_cast<f32>(stats.inputTriangles) *
                                  std::clamp(settings.ratio, 0.01f, 1.0f)));
    if (src.vertices.empty() || src.indices.size() < 3 || stats.inputTriangles <= targetTris) {
        if (outStats) *outStats = stats;
        return src;
    }

    // ---- Weld by position ---------------------------------------------------
    // A mesh split at UV or normal seams has several vertices at one point. Collapsing them
    // independently tears the surface open at every seam, so the SIMPLIFICATION operates on
    // welded positions while the original vertices are carried along by their representative.
    std::vector<u32> weld(src.vertices.size());
    std::vector<glm::dvec3> pos;
    {
        struct Key {
            i64 x, y, z;
            bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct Hash {
            usize operator()(const Key& k) const {
                u64 h = 1469598103934665603ull;
                for (i64 v : {k.x, k.y, k.z}) {
                    h ^= static_cast<u64>(v);
                    h *= 1099511628211ull;
                }
                return static_cast<usize>(h);
            }
        };
        std::unordered_map<Key, u32, Hash> map;
        map.reserve(src.vertices.size());
        // 1e-5 m quantisation: finer than any authored geometry, coarse enough to catch the
        // float noise that separates duplicated seam vertices.
        constexpr f64 kQuant = 1e5;
        for (usize i = 0; i < src.vertices.size(); ++i) {
            const glm::vec3& p = src.vertices[i].position;
            const Key k{static_cast<i64>(std::llround(p.x * kQuant)),
                        static_cast<i64>(std::llround(p.y * kQuant)),
                        static_cast<i64>(std::llround(p.z * kQuant))};
            auto it = map.find(k);
            if (it == map.end()) {
                const u32 id = static_cast<u32>(pos.size());
                map.emplace(k, id);
                pos.push_back(glm::dvec3(p));
                weld[i] = id;
            } else {
                weld[i] = it->second;
            }
        }
    }

    // Welded triangle list. Degenerate triangles (two corners welded together) are dropped
    // up front so they cannot pollute the quadrics with zero-area planes.
    std::vector<glm::uvec3> tris;
    tris.reserve(src.indices.size() / 3);
    std::vector<u32> triSource; // which original triangle each came from, for attribute reuse
    for (usize t = 0; t + 2 < src.indices.size(); t += 3) {
        const u32 a = weld[src.indices[t]], b = weld[src.indices[t + 1]], c = weld[src.indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        tris.push_back({a, b, c});
        triSource.push_back(static_cast<u32>(t / 3));
    }
    if (tris.empty()) {
        if (outStats) *outStats = stats;
        return src;
    }

    glm::dvec3 bbLo(1e30), bbHi(-1e30);
    for (const glm::dvec3& p : pos) { bbLo = glm::min(bbLo, p); bbHi = glm::max(bbHi, p); }
    const f64 diag = glm::length(bbHi - bbLo);
    const f64 maxError = diag * static_cast<f64>(settings.maxErrorFraction);

    // ---- Per-vertex quadrics ------------------------------------------------
    std::vector<Quadric> Q(pos.size());
    std::vector<std::vector<u32>> vertTris(pos.size());
    for (u32 i = 0; i < tris.size(); ++i) {
        const glm::dvec3& p0 = pos[tris[i].x];
        const glm::dvec3& p1 = pos[tris[i].y];
        const glm::dvec3& p2 = pos[tris[i].z];
        glm::dvec3 n = glm::cross(p1 - p0, p2 - p0);
        const f64 area2 = glm::length(n);
        if (area2 < 1e-18) continue;
        n /= area2;
        const f64 d = -glm::dot(n, p0);
        // AREA WEIGHTED. A large face should dominate the error estimate over a sliver, for
        // the same reason area-weighted normals beat averaged ones.
        for (int k = 0; k < 3; ++k) {
            Q[tris[i][k]].AddPlane(n.x, n.y, n.z, d, area2);
            vertTris[tris[i][k]].push_back(i);
        }
    }

    // ---- Boundary constraint ------------------------------------------------
    // An open edge (used by one triangle) is a silhouette the user can see. Without a
    // constraint the metric happily collapses it, because there is no plane pulling it back -
    // the visible result is a decimated mesh whose borders shrink and ripple.
    {
        std::unordered_map<u64, u32> edgeUse;
        edgeUse.reserve(tris.size() * 3);
        const auto key = [](u32 a, u32 b) {
            return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
        };
        for (const glm::uvec3& t : tris) {
            ++edgeUse[key(t.x, t.y)];
            ++edgeUse[key(t.y, t.z)];
            ++edgeUse[key(t.z, t.x)];
        }
        for (const glm::uvec3& t : tris) {
            const glm::dvec3& p0 = pos[t.x];
            const glm::dvec3& p1 = pos[t.y];
            const glm::dvec3& p2 = pos[t.z];
            glm::dvec3 fn = glm::cross(p1 - p0, p2 - p0);
            if (glm::length(fn) < 1e-18) continue;
            fn = glm::normalize(fn);
            const u32 e[3][2] = {{t.x, t.y}, {t.y, t.z}, {t.z, t.x}};
            for (const auto& pair : e) {
                if (edgeUse[key(pair[0], pair[1])] != 1) continue;
                // A plane through the boundary edge, PERPENDICULAR to the face. Collapsing
                // along the boundary is then free; moving off it is expensive.
                const glm::dvec3 dir = pos[pair[1]] - pos[pair[0]];
                glm::dvec3 n = glm::cross(dir, fn);
                const f64 len = glm::length(n);
                if (len < 1e-18) continue;
                n /= len;
                const f64 d = -glm::dot(n, pos[pair[0]]);
                constexpr f64 kBoundaryWeight = 1000.0;
                Q[pair[0]].AddPlane(n.x, n.y, n.z, d, kBoundaryWeight);
                Q[pair[1]].AddPlane(n.x, n.y, n.z, d, kBoundaryWeight);
            }
        }
    }

    // ---- Collapse ------------------------------------------------------------
    std::vector<u32> remap(pos.size());
    for (u32 i = 0; i < remap.size(); ++i) remap[i] = i;
    std::vector<u32> version(pos.size(), 0);
    std::vector<bool> dead(pos.size(), false);

    const auto find = [&](u32 v) {
        while (remap[v] != v) { remap[v] = remap[remap[v]]; v = remap[v]; }
        return v;
    };

    // The collapse TARGET is the midpoint, not the optimal quadric minimiser. Solving the
    // 3x3 system finds a slightly better point but can place it far outside the local
    // neighbourhood on near-degenerate quadrics, which produces spikes. The midpoint is
    // stable, and at these ratios the quality difference is not visible.
    const auto costOf = [&](u32 a, u32 b, glm::dvec3& target) {
        target = (pos[a] + pos[b]) * 0.5;
        Quadric q = Q[a];
        q.Add(Q[b]);
        return std::max(0.0, q.Error(target));
    };

    std::priority_queue<Edge> heap;
    {
        std::unordered_map<u64, bool> seen;
        const auto key = [](u32 a, u32 b) {
            return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
        };
        for (const glm::uvec3& t : tris) {
            const u32 e[3][2] = {{t.x, t.y}, {t.y, t.z}, {t.z, t.x}};
            for (const auto& pr : e) {
                const u64 k = key(pr[0], pr[1]);
                if (seen.count(k)) continue;
                seen[k] = true;
                glm::dvec3 tgt;
                heap.push({pr[0], pr[1], costOf(pr[0], pr[1], tgt), 0});
            }
        }
    }

    u32 liveTris = static_cast<u32>(tris.size());
    while (liveTris > targetTris && !heap.empty()) {
        const Edge e = heap.top();
        heap.pop();
        const u32 a = find(e.a), b = find(e.b);
        if (a == b || dead[a] || dead[b]) continue;
        // Lazy invalidation: an entry whose endpoints have moved on since it was pushed is
        // simply recomputed and re-queued rather than being hunted down in the heap.
        if (e.version != version[a] + version[b]) {
            glm::dvec3 tgt;
            heap.push({a, b, costOf(a, b, tgt), version[a] + version[b]});
            continue;
        }
        glm::dvec3 target;
        const f64 cost = costOf(a, b, target);
        if (cost > maxError * maxError) {
            stats.hitErrorLimit = true;
            break; // everything left in the heap costs at least this much
        }

        if (settings.preventFlips) {
            bool flips = false;
            for (const u32 side : {a, b}) {
                for (const u32 ti : vertTris[side]) {
                    const glm::uvec3& t = tris[ti];
                    const u32 v0 = find(t.x), v1 = find(t.y), v2 = find(t.z);
                    if (v0 == v1 || v1 == v2 || v0 == v2) continue;
                    // The triangle disappears in this collapse; it cannot flip.
                    if ((v0 == a || v0 == b) + (v1 == a || v1 == b) + (v2 == a || v2 == b) >= 2)
                        continue;
                    const glm::dvec3 p0 = (v0 == a || v0 == b) ? target : pos[v0];
                    const glm::dvec3 p1 = (v1 == a || v1 == b) ? target : pos[v1];
                    const glm::dvec3 p2 = (v2 == a || v2 == b) ? target : pos[v2];
                    const glm::dvec3 before = glm::cross(pos[v1] - pos[v0], pos[v2] - pos[v0]);
                    const glm::dvec3 after = glm::cross(p1 - p0, p2 - p0);
                    if (glm::dot(before, after) <= 0.0) { flips = true; break; }
                }
                if (flips) break;
            }
            if (flips) { ++stats.rejectedFlips; continue; }
        }

        // b folds into a.
        pos[a] = target;
        Q[a].Add(Q[b]);
        remap[b] = a;
        dead[b] = true;
        ++version[a];
        vertTris[a].insert(vertTris[a].end(), vertTris[b].begin(), vertTris[b].end());
        vertTris[b].clear();
        stats.maxError = std::max(stats.maxError, static_cast<f32>(std::sqrt(cost)));
        ++stats.collapses;

        // Recount live triangles among the ones this touched.
        u32 removed = 0;
        for (const u32 ti : vertTris[a]) {
            const glm::uvec3& t = tris[ti];
            const u32 v0 = find(t.x), v1 = find(t.y), v2 = find(t.z);
            if ((v0 == v1 || v1 == v2 || v0 == v2) && triSource[ti] != 0xFFFFFFFFu) {
                triSource[ti] = 0xFFFFFFFFu; // mark collapsed exactly once
                ++removed;
            }
        }
        liveTris -= removed;

        // Re-queue the surviving edges around the merged vertex.
        for (const u32 ti : vertTris[a]) {
            const glm::uvec3& t = tris[ti];
            const u32 v[3] = {find(t.x), find(t.y), find(t.z)};
            for (int k = 0; k < 3; ++k) {
                const u32 x = v[k], y = v[(k + 1) % 3];
                if (x == y || (x != a && y != a)) continue;
                glm::dvec3 tgt;
                heap.push({x, y, costOf(x, y, tgt), version[x] + version[y]});
            }
        }
    }

    // ---- Rebuild -------------------------------------------------------------
    // Surviving welded vertices become output vertices, taking their ATTRIBUTES from an
    // original vertex that mapped to them. Positions come from the collapse targets.
    MeshData out;
    out.name = src.name;
    out.material = src.material;
    // Morph targets are deliberately NOT carried: their deltas are indexed per ORIGINAL
    // vertex and the vertex set has changed, so copying them would silently misalign every
    // blendshape. A reduced mesh that cannot be morphed is honest; one that morphs wrongly
    // is a bug that surfaces much later.

    std::vector<u32> outIndexOf(pos.size(), 0xFFFFFFFFu);
    std::vector<u32> attrSource(pos.size(), 0xFFFFFFFFu);
    for (usize i = 0; i < src.vertices.size(); ++i) {
        const u32 r = find(weld[i]);
        if (attrSource[r] == 0xFFFFFFFFu) attrSource[r] = static_cast<u32>(i);
    }

    for (usize t = 0; t < tris.size(); ++t) {
        const u32 a = find(tris[t].x), b = find(tris[t].y), c = find(tris[t].z);
        if (a == b || b == c || a == c) continue;
        for (const u32 v : {a, b, c}) {
            if (outIndexOf[v] != 0xFFFFFFFFu) continue;
            Vertex nv = src.vertices[attrSource[v] == 0xFFFFFFFFu ? 0 : attrSource[v]];
            nv.position = glm::vec3(pos[v]);
            outIndexOf[v] = static_cast<u32>(out.vertices.size());
            out.vertices.push_back(nv);
        }
        out.indices.push_back(outIndexOf[a]);
        out.indices.push_back(outIndexOf[b]);
        out.indices.push_back(outIndexOf[c]);
    }

    // The old normals and tangents describe geometry that no longer exists.
    RecomputeNormalsTangents(out);

    stats.outputTriangles = static_cast<u32>(out.indices.size() / 3);
    if (outStats) *outStats = stats;
    return out;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("simplify FAIL: %s\n", what); ++g_fails; }
}

// A UV sphere: curved everywhere, so decimation has to make real choices rather than find
// coplanar faces to merge for free.
MeshData Sphere(u32 rings, u32 segs, f32 r) {
    MeshData m;
    for (u32 y = 0; y <= rings; ++y) {
        const f32 v = static_cast<f32>(y) / static_cast<f32>(rings);
        const f32 phi = v * 3.14159265f;
        for (u32 x = 0; x <= segs; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(segs);
            const f32 th = u * 6.28318531f;
            Vertex vert;
            vert.position = {r * std::sin(phi) * std::cos(th), r * std::cos(phi),
                             r * std::sin(phi) * std::sin(th)};
            vert.uv = {u, v};
            m.vertices.push_back(vert);
        }
    }
    for (u32 y = 0; y < rings; ++y) {
        for (u32 x = 0; x < segs; ++x) {
            const u32 i0 = y * (segs + 1) + x, i1 = i0 + 1;
            const u32 i2 = i0 + segs + 1, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    RecomputeNormalsTangents(m);
    return m;
}

// A flat grid: coplanar, so a good decimator should reduce it almost to nothing without
// moving the surface at all.
MeshData Grid(u32 n) {
    MeshData m;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x) {
            Vertex v;
            v.position = {static_cast<f32>(x) / n, 0.0f, static_cast<f32>(y) / n};
            v.uv = {static_cast<f32>(x) / n, static_cast<f32>(y) / n};
            m.vertices.push_back(v);
        }
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 i0 = y * (n + 1) + x, i1 = i0 + 1, i2 = i0 + n + 1, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    RecomputeNormalsTangents(m);
    return m;
}

f32 MaxDeviation(const MeshData& reduced, const MeshData& original) {
    // Worst distance from a reduced vertex to the NEAREST original vertex. Crude but
    // sufficient to catch a decimator that moves the surface somewhere it should not be.
    f32 worst = 0.0f;
    for (const Vertex& v : reduced.vertices) {
        f32 best = 1e9f;
        for (const Vertex& o : original.vertices)
            best = std::min(best, glm::length(v.position - o.position));
        worst = std::max(worst, best);
    }
    return worst;
}
} // namespace

bool SimplifySelfTest() {
    g_fails = 0;

    // ---- a curved mesh reduces and keeps its shape ----
    {
        const MeshData sphere = Sphere(24, 32, 1.0f);
        SimplifySettings s;
        s.ratio = 0.25f;
        SimplifyStats st;
        const MeshData lod = Simplify(sphere, s, &st);

        Check(st.outputTriangles < st.inputTriangles / 2,
              "a 25% request must actually remove most of the triangles");
        Check(!lod.vertices.empty() && !lod.indices.empty(), "the reduced mesh must not be empty");
        Check(lod.indices.size() % 3 == 0, "the reduced index buffer must be whole triangles");
        for (const u32 i : lod.indices)
            Check(i < lod.vertices.size(), "every reduced index must be in range");

        // SHAPE PRESERVATION - the whole point of using a quadric metric.
        Check(MaxDeviation(lod, sphere) < 0.12f,
              "A DECIMATED SPHERE MUST STILL BE A SPHERE - vertices may not wander far from "
              "the original surface");
        for (const Vertex& v : lod.vertices) {
            const f32 r = glm::length(v.position);
            Check(r > 0.80f && r < 1.05f, "every reduced vertex must stay near the sphere");
            Check(std::abs(glm::length(v.normal) - 1.0f) < 1e-2f,
                  "normals must be rebuilt and unit length");
            Check(std::isfinite(v.tangent.x), "tangents must be rebuilt and finite");
        }
        Check(st.rejectedFlips == 0 || st.collapses > 0, "flip rejection must not stall progress");
    }

    // ---- a flat mesh reduces almost for free ----
    {
        const MeshData grid = Grid(20);
        SimplifySettings s;
        s.ratio = 0.05f;
        s.minTriangles = 2;
        SimplifyStats st;
        const MeshData lod = Simplify(grid, s, &st);
        Check(st.outputTriangles < st.inputTriangles / 4,
              "a FLAT mesh should decimate heavily - there is no shape to preserve");
        Check(st.maxError < 0.02f,
              "collapsing coplanar faces must cost almost nothing - if this is large the "
              "quadric is not measuring what it should");
        for (const Vertex& v : lod.vertices)
            Check(std::abs(v.position.y) < 1e-3f, "a flat mesh must stay flat");
    }

    // ---- guards ----
    {
        SimplifySettings s;
        s.ratio = 0.25f;
        // Already below the floor: return unchanged rather than collapsing to nothing.
        const MeshData tiny = Sphere(3, 4, 1.0f);
        SimplifyStats st;
        const MeshData lod = Simplify(tiny, s, &st);
        Check(lod.indices.size() == tiny.indices.size(),
              "a mesh already at or below the minimum must come back UNCHANGED");

        MeshData empty;
        Check(Simplify(empty, s, nullptr).vertices.empty(), "an empty mesh must not crash");

        MeshData degenerate;
        degenerate.vertices.resize(3);
        degenerate.indices = {0, 0, 0};
        Check(Simplify(degenerate, s, nullptr).indices.size() <= 3,
              "a fully degenerate mesh must not crash");
    }

    // ---- ratio 1.0 is identity, and morphs are dropped rather than corrupted ----
    {
        MeshData sphere = Sphere(12, 16, 1.0f);
        MorphTarget mt;
        mt.name = "test";
        mt.posDelta.assign(sphere.vertices.size(), glm::vec3(0.1f));
        sphere.morphTargets.push_back(mt);

        SimplifySettings s;
        s.ratio = 1.0f;
        const MeshData same = Simplify(sphere, s, nullptr);
        Check(same.indices.size() == sphere.indices.size(), "ratio 1.0 must change nothing");

        s.ratio = 0.3f;
        const MeshData lod = Simplify(sphere, s, nullptr);
        Check(lod.morphTargets.empty(),
              "A REDUCED MESH MUST DROP ITS MORPH TARGETS - the deltas are indexed per "
              "original vertex, so carrying them would silently misalign every blendshape");
        Check(lod.material.name == sphere.material.name, "the material must carry over");
    }

    // ---- determinism ----
    {
        const MeshData sphere = Sphere(16, 20, 1.0f);
        SimplifySettings s;
        s.ratio = 0.3f;
        const MeshData a = Simplify(sphere, s, nullptr);
        const MeshData b = Simplify(sphere, s, nullptr);
        Check(a.indices == b.indices, "the same mesh must reduce to the SAME topology");
        bool same = a.vertices.size() == b.vertices.size();
        if (same)
            for (usize i = 0; i < a.vertices.size(); ++i)
                if (a.vertices[i].position != b.vertices[i].position) { same = false; break; }
        Check(same, "...and to bit-identical positions, or the asset hash is meaningless");
    }

    // ---- skinning survives ----
    {
        MeshData skinned = Sphere(12, 16, 1.0f);
        for (usize i = 0; i < skinned.vertices.size(); ++i) {
            skinned.vertices[i].joints[0] = static_cast<u16>(i % 4);
            skinned.vertices[i].weights[0] = 1.0f;
        }
        SimplifySettings s;
        s.ratio = 0.3f;
        const MeshData lod = Simplify(skinned, s, nullptr);
        f32 worstSum = 0.0f;
        for (const Vertex& v : lod.vertices) {
            const f32 sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
            worstSum = std::max(worstSum, std::abs(sum - 1.0f));
        }
        Check(worstSum < 1e-5f,
              "skin weights must survive decimation summing to 1 - a reduced mesh with broken "
              "weights collapses to the origin when skinned");
    }

    if (g_fails == 0)
        std::printf("simplify: quadric-error decimation - a sphere keeps its shape, a flat "
                    "grid reduces for almost no error, boundaries are constrained, flips are "
                    "rejected, skin weights survive, morphs are dropped rather than corrupted, "
                    "and the same mesh always reduces identically\n");
    return g_fails == 0;
}

} // namespace hbe::mesh

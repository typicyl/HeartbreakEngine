// Assets/SeamWeld.cpp
#include "Assets/SeamWeld.h"

#include "Assets/Animation.h"
#include "Assets/Mesh.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace hbe::weld {

namespace {

// --- bone-name canonicalization (mirror of AnimationSystem CanonicalJointName) ---
std::string CanonicalBone(const std::string& name) {
    std::string s = name;
    const usize sep = s.find_last_of(":|");
    if (sep != std::string::npos) s = s.substr(sep + 1);
    if (const usize fbx = s.find("$AssimpFbx$"); fbx != std::string::npos) {
        s = s.substr(0, fbx);
        while (!s.empty() && s.back() == '_') s.pop_back();
    }
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Quantized-position key for merging coincident vertices (position topology).
struct PosKey {
    i64 x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
    usize operator()(const PosKey& k) const {
        // 64-bit mix (splitmix-ish) over the three components.
        u64 h = 1469598103934665603ull;
        for (i64 v : {k.x, k.y, k.z}) {
            h ^= static_cast<u64>(v);
            h *= 1099511628211ull;
        }
        return static_cast<usize>(h);
    }
};
PosKey Quantize(const glm::vec3& p, f32 eps) {
    const f32 inv = 1.0f / eps;
    return {static_cast<i64>(std::llround(p.x * inv)), static_cast<i64>(std::llround(p.y * inv)),
            static_cast<i64>(std::llround(p.z * inv))};
}

// Maps each local vertex of `mesh` to a canonical position-id (UV/normal duplicates
// at the same position collapse to one id). Returns the per-vertex ids + the id count.
std::vector<u32> PositionTopology(const MeshData& mesh, f32 eps, u32& outCount) {
    std::vector<u32> canon(mesh.vertices.size(), 0);
    std::unordered_map<PosKey, u32, PosKeyHash> ids;
    ids.reserve(mesh.vertices.size());
    u32 next = 0;
    for (usize i = 0; i < mesh.vertices.size(); ++i) {
        const PosKey k = Quantize(mesh.vertices[i].position, eps);
        auto [it, inserted] = ids.try_emplace(k, next);
        if (inserted) ++next;
        canon[i] = it->second;
    }
    outCount = next;
    return canon;
}

// Boundary canonical-position ids of a mesh (edges used by exactly one triangle),
// plus a count of non-manifold edges (>2 triangles).
std::vector<bool> BoundaryCanon(const MeshData& mesh, const std::vector<u32>& canon, u32 canonCount,
                                u32& outNonManifold) {
    std::unordered_map<u64, u32> edgeCount; // packed (min<<32|max) -> triangle count
    edgeCount.reserve(mesh.indices.size());
    const auto edge = [](u32 a, u32 b) -> u64 {
        const u32 lo = a < b ? a : b, hi = a < b ? b : a;
        return (static_cast<u64>(lo) << 32) | hi;
    };
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const u32 c0 = canon[mesh.indices[t]], c1 = canon[mesh.indices[t + 1]],
                  c2 = canon[mesh.indices[t + 2]];
        if (c0 == c1 || c1 == c2 || c0 == c2) continue; // degenerate in position topology
        ++edgeCount[edge(c0, c1)];
        ++edgeCount[edge(c1, c2)];
        ++edgeCount[edge(c2, c0)];
    }
    std::vector<bool> isBoundary(canonCount, false);
    u32 nonManifold = 0;
    for (const auto& [key, count] : edgeCount) {
        if (count == 1) {
            isBoundary[static_cast<u32>(key >> 32)] = true;
            isBoundary[static_cast<u32>(key & 0xFFFFFFFFu)] = true;
        } else if (count >= 3) {
            ++nonManifold;
        }
    }
    outNonManifold = nonManifold;
    return isBoundary;
}

// --- disjoint set (union-find) over global boundary-vertex indices ---
struct DSU {
    std::vector<u32> parent;
    explicit DSU(u32 n) : parent(n) {
        for (u32 i = 0; i < n; ++i) parent[i] = i;
    }
    u32 Find(u32 a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }
    void Union(u32 a, u32 b) {
        a = Find(a);
        b = Find(b);
        if (a != b) parent[a > b ? a : b] = (a < b ? a : b); // attach higher to lower (stable)
    }
};

// Canonical (joint,weight) binding: merge duplicate joints, sort by joint ascending,
// renormalize once with a fixed summation order, zero-pad to 4.
void CanonicalBinding(const u16 srcJoints[4], const f32 srcWeights[4], u16 outJoints[4],
                      f32 outWeights[4]) {
    std::pair<u16, f32> infl[4];
    int n = 0;
    for (int k = 0; k < 4; ++k) {
        if (srcWeights[k] <= 0.0f) continue;
        // merge duplicate joint indices
        bool merged = false;
        for (int m = 0; m < n; ++m)
            if (infl[m].first == srcJoints[k]) {
                infl[m].second += srcWeights[k];
                merged = true;
                break;
            }
        if (!merged) infl[n++] = {srcJoints[k], srcWeights[k]};
    }
    std::sort(infl, infl + n, [](const auto& a, const auto& b) { return a.first < b.first; });
    f32 sum = 0.0f;
    for (int k = 0; k < n; ++k) sum += infl[k].second;
    for (int k = 0; k < 4; ++k) {
        outJoints[k] = 0;
        outWeights[k] = 0.0f;
    }
    for (int k = 0; k < n; ++k) {
        outJoints[k] = infl[k].first;
        outWeights[k] = sum > 0.0f ? infl[k].second / sum : infl[k].second;
    }
}

} // namespace

std::vector<i32> BuildJointRemap(const Skeleton& src, const Skeleton& canon) {
    std::vector<i32> map(src.joints.size(), -1);
    // Precompute canonical names of the target once.
    std::vector<std::string> canonNames(canon.joints.size());
    for (usize j = 0; j < canon.joints.size(); ++j) canonNames[j] = CanonicalBone(canon.joints[j].name);
    for (usize s = 0; s < src.joints.size(); ++s) {
        const std::string want = CanonicalBone(src.joints[s].name);
        for (usize j = 0; j < canonNames.size(); ++j)
            if (canonNames[j] == want) {
                map[s] = static_cast<i32>(j);
                break;
            }
    }
    return map;
}

void RemapJoints(MeshData& mesh, const std::vector<i32>& srcToCanon) {
    // Identity mapping -> leave the mesh bit-unchanged.
    bool identity = true;
    for (usize i = 0; i < srcToCanon.size(); ++i)
        if (srcToCanon[i] != static_cast<i32>(i)) {
            identity = false;
            break;
        }
    if (identity) return;

    for (Vertex& v : mesh.vertices) {
        bool dropped = false;
        for (int k = 0; k < 4; ++k) {
            if (v.weights[k] <= 0.0f) {
                v.joints[k] = 0;
                continue;
            }
            const i32 j = v.joints[k] < srcToCanon.size() ? srcToCanon[v.joints[k]] : -1;
            if (j < 0) {
                v.weights[k] = 0.0f; // unmatched bone -> drop this influence
                v.joints[k] = 0;
                dropped = true;
            } else {
                v.joints[k] = static_cast<u16>(j);
            }
        }
        if (dropped) {
            f32 sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
            if (sum > 0.0f)
                for (int k = 0; k < 4; ++k) v.weights[k] /= sum;
        }
    }
}

std::vector<u32> OpenBoundaryVertices(const MeshData& mesh, f32 mergeEps) {
    u32 canonCount = 0;
    const std::vector<u32> canon = PositionTopology(mesh, mergeEps, canonCount);
    u32 nonManifold = 0;
    const std::vector<bool> isBoundary = BoundaryCanon(mesh, canon, canonCount, nonManifold);
    std::vector<u32> out;
    for (u32 i = 0; i < mesh.vertices.size(); ++i)
        if (isBoundary[canon[i]]) out.push_back(i);
    return out;
}

Stats WeldSeams(std::vector<Part>& parts, f32 toleranceScale,
                std::vector<glm::vec3>* outOpenPositions) {
    Stats stats;
    if (parts.empty()) return stats;

    // Combined bounding box -> grouping tolerance.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Part& p : parts) {
        if (!p.mesh) continue;
        for (const Vertex& v : p.mesh->vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
    }
    const f32 diag = glm::length(hi - lo);
    const f32 tol = glm::max(diag * toleranceScale, 1e-6f);
    stats.tolerance = tol;
    const f32 mergeEps = glm::max(tol * 0.01f, 1e-6f); // position-merge grid (tighter than grouping)

    // Gather every part's open-boundary vertices into a flat list.
    struct BV {
        u32 part;
        u32 local;
        glm::vec3 pos;
    };
    std::vector<BV> bvs;
    for (u32 pi = 0; pi < parts.size(); ++pi) {
        const Part& p = parts[pi];
        if (!p.mesh) continue;
        u32 canonCount = 0;
        const std::vector<u32> canon = PositionTopology(*p.mesh, mergeEps, canonCount);
        u32 nonManifold = 0;
        const std::vector<bool> isBoundary = BoundaryCanon(*p.mesh, canon, canonCount, nonManifold);
        stats.nonManifoldEdges += nonManifold;
        for (u32 i = 0; i < p.mesh->vertices.size(); ++i)
            if (isBoundary[canon[i]]) bvs.push_back({pi, i, p.mesh->vertices[i].position});
    }
    stats.boundaryVertices = static_cast<u32>(bvs.size());
    if (bvs.empty()) return stats;

    // Spatial hash (cell = tolerance) -> union boundary verts within `tol`.
    DSU dsu(static_cast<u32>(bvs.size()));
    const f32 cell = tol;
    const auto cellKey = [cell](const glm::vec3& p) -> PosKey {
        const f32 inv = 1.0f / cell;
        return {static_cast<i64>(std::floor(p.x * inv)), static_cast<i64>(std::floor(p.y * inv)),
                static_cast<i64>(std::floor(p.z * inv))};
    };
    std::unordered_map<PosKey, std::vector<u32>, PosKeyHash> grid;
    grid.reserve(bvs.size());
    for (u32 i = 0; i < bvs.size(); ++i) grid[cellKey(bvs[i].pos)].push_back(i);
    const f32 tol2 = tol * tol;
    for (u32 i = 0; i < bvs.size(); ++i) {
        const PosKey base = cellKey(bvs[i].pos);
        for (i64 dz = -1; dz <= 1; ++dz)
            for (i64 dy = -1; dy <= 1; ++dy)
                for (i64 dx = -1; dx <= 1; ++dx) {
                    const auto it = grid.find({base.x + dx, base.y + dy, base.z + dz});
                    if (it == grid.end()) continue;
                    for (const u32 j : it->second) {
                        if (j <= i) continue;
                        const glm::vec3 d = bvs[i].pos - bvs[j].pos;
                        if (glm::dot(d, d) <= tol2) dsu.Union(i, j);
                    }
                }
    }

    // Bucket boundary verts by group root.
    std::unordered_map<u32, std::vector<u32>> groups;
    for (u32 i = 0; i < bvs.size(); ++i) groups[dsu.Find(i)].push_back(i);

    for (auto& [root, members] : groups) {
        // Distinct parts in the group; a lone part = an open boundary (hem) or a
        // topology mismatch (drift risk) - flagged, not welded.
        u32 firstPart = bvs[members[0]].part;
        bool multiPart = false;
        for (const u32 m : members)
            if (bvs[m].part != firstPart) {
                multiPart = true;
                break;
            }
        if (!multiPart) {
            stats.openBoundary += static_cast<u32>(members.size());
            if (outOpenPositions)
                for (const u32 m : members) outOpenPositions->push_back(bvs[m].pos);
            continue;
        }
        ++stats.groups;

        // Canonical source: a master member if present, else lowest (part,local).
        u32 srcM = members[0];
        for (const u32 m : members) {
            const bool mMaster = parts[bvs[m].part].isMaster;
            const bool sMaster = parts[bvs[srcM].part].isMaster;
            if (mMaster && !sMaster) {
                srcM = m;
            } else if (mMaster == sMaster) {
                if (bvs[m].part < bvs[srcM].part ||
                    (bvs[m].part == bvs[srcM].part && bvs[m].local < bvs[srcM].local))
                    srcM = m;
            }
        }
        const Vertex& src = parts[bvs[srcM].part].mesh->vertices[bvs[srcM].local];

        // One canonical position + skin binding, stamped bit-identically to all.
        const glm::vec3 canonPos = src.position;
        u16 canonJoints[4];
        f32 canonWeights[4];
        CanonicalBinding(src.joints, src.weights, canonJoints, canonWeights);

        // Smooth normal/tangent: average over the CONTINUOUS members only (an
        // overlap part keeps its own hard edge). Single handedness for the group.
        glm::vec3 nSum(0.0f), tSum(0.0f);
        f32 handed = src.tangent.w;
        int contCount = 0;
        for (const u32 m : members)
            if (parts[bvs[m].part].seamMode == SeamMode::Continuous) {
                const Vertex& mv = parts[bvs[m].part].mesh->vertices[bvs[m].local];
                nSum += mv.normal;
                tSum += glm::vec3(mv.tangent);
                if (contCount == 0) handed = mv.tangent.w;
                ++contCount;
            }
        const bool haveNormal = contCount > 0 && glm::dot(nSum, nSum) > 1e-12f;
        const glm::vec3 canonN = haveNormal ? glm::normalize(nSum) : glm::vec3(0.0f);
        const bool haveTan = contCount > 0 && glm::dot(tSum, tSum) > 1e-12f;
        const glm::vec3 canonT = haveTan ? glm::normalize(tSum) : glm::vec3(0.0f);

        for (const u32 m : members) {
            Vertex& v = parts[bvs[m].part].mesh->vertices[bvs[m].local];
            v.position = canonPos;
            std::memcpy(v.joints, canonJoints, sizeof(canonJoints));
            std::memcpy(v.weights, canonWeights, sizeof(canonWeights));
            if (parts[bvs[m].part].seamMode == SeamMode::Continuous) {
                if (haveNormal) v.normal = canonN;
                if (haveTan) v.tangent = glm::vec4(canonT, handed);
            }
            ++stats.weldedVertices;
        }
    }
    return stats;
}

// ---------------------------------------------------------------------------
// SelfTest: two 1-triangle parts sharing an edge, with DIFFERENT authored seam
// bindings. After WeldSeams the shared verts must skin bit-identically.
namespace {
// CPU mirror of Shaders/MeshPBR.hlsl SkinMatrix (same 4-term order).
glm::vec3 SkinPos(const std::vector<glm::mat4>& pal, const Vertex& v) {
    const glm::mat4 skin = pal[v.joints[0]] * v.weights[0] + pal[v.joints[1]] * v.weights[1] +
                           pal[v.joints[2]] * v.weights[2] + pal[v.joints[3]] * v.weights[3];
    return glm::vec3(skin * glm::vec4(v.position, 1.0f));
}
bool BitEqual(const glm::vec3& a, const glm::vec3& b) {
    return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0;
}
} // namespace

bool SelfTest() {
    // Part A (master): triangle whose edge (v1,v2) is the seam. Seam verts skinned
    // {j0:0.7, j1:0.3}.
    MeshData a;
    a.vertices = {
        {{0.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}},
        {{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {0, 1, 0, 0}, {0.7f, 0.3f, 0, 0}},
        {{1.0f, 1.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {0, 1, 0, 0}, {0.7f, 0.3f, 0, 0}},
    };
    a.indices = {0, 1, 2};

    // Part B: shares the edge at (1,0,0)-(1,1,0) but authored DIFFERENTLY: reversed
    // slot order AND only 2 influences renormalized on its own -> a real mismatch
    // that would drift without the weld.
    MeshData b;
    b.vertices = {
        {{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 0, 0, 0}, {0.3f, 0.7f, 0, 0}},
        {{1.0f, 1.0f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 0, 0, 0}, {0.3f, 0.7f, 0, 0}},
        {{2.0f, 0.5f, 0.0f}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}},
    };
    b.indices = {0, 1, 2};

    std::vector<Part> parts = {{&a, SeamMode::Continuous, true},
                               {&b, SeamMode::Continuous, false}};
    const Stats st = WeldSeams(parts, 1e-3f);

    // A distinctive palette (order-sensitive if the binding were wrong).
    std::vector<glm::mat4> pal(2);
    pal[0] = glm::mat4(2.0f, 0.1f, 0.0f, 0.0f, 0.0f, 1.5f, 0.2f, 0.0f, 0.3f, 0.0f, 1.1f, 0.0f, 0.4f,
                       0.5f, 0.6f, 1.0f);
    pal[1] = glm::mat4(0.7f, 0.0f, 0.2f, 0.0f, 0.1f, 0.9f, 0.0f, 0.0f, 0.0f, 0.3f, 1.3f, 0.0f, 1.0f,
                       2.0f, 0.5f, 1.0f);

    // The two shared seam vertices: A[1]/A[2] at (1,0,0)/(1,1,0), B[0]/B[1] likewise.
    bool ok = true;
    ok &= BitEqual(SkinPos(pal, a.vertices[1]), SkinPos(pal, b.vertices[0]));
    ok &= BitEqual(SkinPos(pal, a.vertices[2]), SkinPos(pal, b.vertices[1]));
    // And the raw bindings must be byte-identical.
    ok &= std::memcmp(a.vertices[1].joints, b.vertices[0].joints, sizeof(u16) * 4) == 0;
    ok &= std::memcmp(a.vertices[1].weights, b.vertices[0].weights, sizeof(f32) * 4) == 0;

    if (ok) {
        HBE_INFO("[SeamWeld] SelfTest PASS - {} group(s), {} verts welded, tol={}.", st.groups,
                 st.weldedVertices, st.tolerance);
    } else {
        HBE_ERROR("[SeamWeld] SelfTest FAIL - seam verts did not become bit-identical "
                  "(groups={}, welded={}).",
                  st.groups, st.weldedVertices);
    }
    return ok;
}

} // namespace hbe::weld

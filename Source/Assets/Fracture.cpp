// Assets/Fracture.cpp
#include "Assets/Fracture.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring> // std::memcpy in the binary (de)serialisers
#include <fstream>
#include <random>
#include <unordered_map>

namespace hbe {

const char* FracturePatternName(FracturePattern p) {
    switch (p) {
        case FracturePattern::Uniform:   return "Uniform";
        case FracturePattern::Clustered: return "Clustered";
        case FracturePattern::Radial:    return "Radial";
        case FracturePattern::Slabs:     return "Slabs";
    }
    return "Uniform";
}

namespace assets {
namespace {

using json = nlohmann::json;

constexpr f32 kEps = 1e-5f;

// A convex polyhedron as a face soup. Each face is a CCW loop of positions
// (viewed from outside) plus the plane it lies on. Storing explicit loops rather
// than a half-edge mesh keeps the clip routine simple and robust: clipping a
// convex solid never needs adjacency, only per-face polygon clipping plus one
// new cap face stitched from the cut segments.
struct Face {
    std::vector<glm::vec3> loop;
    glm::vec3 normal{0.0f};
    // Which OTHER Voronoi site produced this face by bisection (-1 = an original
    // face of the source bounds, i.e. an exterior surface). This is what yields
    // the adjacency graph for free: a face tagged with site j means "I touch j".
    int fromSite = -1;
};

struct Poly {
    std::vector<Face> faces;
};

glm::vec3 FaceNormal(const std::vector<glm::vec3>& loop) {
    // Newell's method: robust for non-planar / near-degenerate loops, unlike a
    // single cross product of the first three (which explodes on collinear runs).
    glm::vec3 n(0.0f);
    const usize n0 = loop.size();
    for (usize i = 0; i < n0; ++i) {
        const glm::vec3& a = loop[i];
        const glm::vec3& b = loop[(i + 1) % n0];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const f32 len = glm::length(n);
    return len > kEps ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

Poly BoxPoly(const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
    };
    // CCW when viewed from outside.
    const int idx[6][4] = {
        {0, 3, 2, 1}, // -Z
        {4, 5, 6, 7}, // +Z
        {0, 4, 7, 3}, // -X
        {1, 2, 6, 5}, // +X
        {0, 1, 5, 4}, // -Y
        {3, 7, 6, 2}, // +Y
    };
    Poly p;
    p.faces.reserve(6);
    for (const auto& f : idx) {
        Face face;
        face.loop = {c[f[0]], c[f[1]], c[f[2]], c[f[3]]};
        face.normal = FaceNormal(face.loop);
        p.faces.push_back(std::move(face));
    }
    return p;
}

// Clips `p` by the half-space { x : dot(n,x) <= d } (keeps the NEGATIVE side).
// Returns false when the result is empty.
//
// Per face this is Sutherland-Hodgman. The extra work over 2D clipping is
// collecting the cut segments and stitching them into ONE new cap face - that cap
// is the freshly exposed interior surface of the break.
bool ClipPoly(Poly& p, const glm::vec3& n, f32 d, int fromSite, f32 weldTol) {
    std::vector<Face> kept;
    kept.reserve(p.faces.size() + 1);
    std::vector<std::pair<glm::vec3, glm::vec3>> cuts; // segments on the cut plane

    for (const Face& f : p.faces) {
        std::vector<glm::vec3> out;
        out.reserve(f.loop.size() + 2);
        glm::vec3 cutA(0.0f), cutB(0.0f);
        int cutCount = 0;
        const usize m = f.loop.size();
        for (usize i = 0; i < m; ++i) {
            const glm::vec3& cur = f.loop[i];
            const glm::vec3& nxt = f.loop[(i + 1) % m];
            const f32 dc = glm::dot(n, cur) - d;
            const f32 dn = glm::dot(n, nxt) - d;
            const bool inCur = dc <= kEps;
            const bool inNxt = dn <= kEps;
            if (inCur) out.push_back(cur);
            if (inCur != inNxt) {
                const f32 t = dc / (dc - dn); // exact crossing parameter
                const glm::vec3 x = cur + (nxt - cur) * t;
                out.push_back(x);
                if (cutCount == 0) cutA = x;
                else cutB = x;
                ++cutCount;
            }
        }
        if (out.size() >= 3) {
            Face nf;
            nf.loop = std::move(out);
            nf.normal = f.normal; // clipping a planar face keeps its plane
            nf.fromSite = f.fromSite;
            kept.push_back(std::move(nf));
        }
        if (cutCount == 2) cuts.emplace_back(cutA, cutB);
    }

    if (kept.empty()) return false;

    // Stitch the cut segments into the cap loop. Segments arrive in arbitrary
    // order and orientation, so walk them by matching endpoints within a
    // tolerance. Convexity guarantees a single closed loop.
    if (cuts.size() >= 3) {
        std::vector<glm::vec3> loop;
        std::vector<bool> used(cuts.size(), false);
        loop.push_back(cuts[0].first);
        glm::vec3 cursor = cuts[0].second;
        used[0] = true;
        loop.push_back(cursor);
        // Endpoint matching uses a tolerance SCALED TO THE OBJECT (weldTol), not a
        // fixed 1e-4. On a 50 m wall, float error at those coordinates already
        // approaches 3e-4, so an absolute epsilon fails to match, the chain stays
        // open, and the chunk gets a malformed cap - a visible hole in the break.
        for (usize step = 1; step < cuts.size(); ++step) {
            bool found = false;
            for (usize i = 0; i < cuts.size(); ++i) {
                if (used[i]) continue;
                if (glm::distance(cuts[i].first, cursor) < weldTol) {
                    cursor = cuts[i].second;
                } else if (glm::distance(cuts[i].second, cursor) < weldTol) {
                    cursor = cuts[i].first;
                } else {
                    continue;
                }
                used[i] = true;
                found = true;
                // Closing the loop: stop rather than re-adding the start point.
                if (glm::distance(cursor, loop.front()) < weldTol) { step = cuts.size(); break; }
                loop.push_back(cursor);
                break;
            }
            if (!found) break; // open chain (numerically degenerate) - take what we have
        }
        if (loop.size() >= 3) {
            Face cap;
            cap.loop = std::move(loop);
            cap.normal = FaceNormal(cap.loop);
            // Orient outward: the cap's outward normal is +n (we kept the side
            // where dot(n,x) <= d, so the solid lies behind the plane).
            if (glm::dot(cap.normal, n) < 0.0f) {
                std::reverse(cap.loop.begin(), cap.loop.end());
                cap.normal = -cap.normal;
            }
            cap.fromSite = fromSite; // records adjacency with the bisecting site
            kept.push_back(std::move(cap));
        }
    }

    p.faces = std::move(kept);
    return p.faces.size() >= 4; // a solid needs at least a tetrahedron
}

// Signed volume + centroid of a closed convex polyhedron (tetrahedron fan from
// the origin over each triangulated face).
void PolyVolumeCentroid(const Poly& p, f32& outVolume, glm::vec3& outCentroid) {
    f64 vol = 0.0;
    glm::dvec3 acc(0.0);
    for (const Face& f : p.faces) {
        for (usize i = 1; i + 1 < f.loop.size(); ++i) {
            const glm::dvec3 a(f.loop[0]), b(f.loop[i]), c(f.loop[i + 1]);
            const f64 v = glm::dot(a, glm::cross(b, c)) / 6.0;
            vol += v;
            acc += (a + b + c) * (v / 4.0);
        }
    }
    outVolume = static_cast<f32>(std::abs(vol));
    outCentroid = std::abs(vol) > 1e-9 ? glm::vec3(acc / vol) : glm::vec3(0.0f);
}

// Triangulates the polyhedron into a MeshData. Exterior faces keep the source
// material; cap faces (fromSite >= 0) are the broken interior.
MeshData PolyToMesh(const Poly& p, const Material& exterior, const glm::vec3& mn,
                    const glm::vec3& mx) {
    MeshData m;
    const glm::vec3 ext = glm::max(mx - mn, glm::vec3(1e-4f));
    for (const Face& f : p.faces) {
        if (f.loop.size() < 3) continue;
        const u32 base = static_cast<u32>(m.vertices.size());
        for (const glm::vec3& pos : f.loop) {
            Vertex v;
            v.position = pos;
            v.normal = f.normal;
            // Flat triplanar-ish UV from the dominant axis: chunks have no
            // meaningful inherited parameterisation once cut, and the interior
            // needs *some* UV for its material to tile sanely.
            const glm::vec3 a = glm::abs(f.normal);
            const glm::vec3 rel = (pos - mn) / ext;
            v.uv = (a.x > a.y && a.x > a.z) ? glm::vec2(rel.z, rel.y)
                 : (a.y > a.z)              ? glm::vec2(rel.x, rel.z)
                                            : glm::vec2(rel.x, rel.y);
            // Tangent along the first edge, orthogonalised against the normal.
            glm::vec3 t = f.loop.size() > 1 ? f.loop[1] - f.loop[0] : glm::vec3(1, 0, 0);
            t = t - f.normal * glm::dot(t, f.normal);
            const f32 tl = glm::length(t);
            v.tangent = glm::vec4(tl > kEps ? t / tl : glm::vec3(1, 0, 0), 1.0f);
            m.vertices.push_back(v);
        }
        // Fan-triangulate the convex loop.
        for (u32 i = 1; i + 1 < static_cast<u32>(f.loop.size()); ++i) {
            m.indices.push_back(base);
            m.indices.push_back(base + i);
            m.indices.push_back(base + i + 1);
        }
    }
    m.material = exterior;
    return m;
}

// Scatters fracture sites per the chosen pattern. Deterministic in `seed`.
std::vector<glm::vec3> ScatterSites(const FractureSettings& s, const glm::vec3& mn,
                                    const glm::vec3& mx) {
    std::mt19937 rng(s.seed);
    std::uniform_real_distribution<f32> u01(0.0f, 1.0f);
    const glm::vec3 ext = mx - mn;
    const u32 n = glm::max(s.cellCount, 2u);
    std::vector<glm::vec3> sites;
    sites.reserve(n);

    const auto randPoint = [&]() {
        return mn + glm::vec3(u01(rng), u01(rng), u01(rng)) * ext;
    };

    switch (s.pattern) {
        case FracturePattern::Uniform:
            for (u32 i = 0; i < n; ++i) sites.push_back(randPoint());
            break;

        case FracturePattern::Clustered: {
            // A handful of cluster centres, most sites drawn tightly around them:
            // gives a few large survivors plus concentrated rubble.
            const u32 clusters = glm::max(2u, n / 6u);
            std::vector<glm::vec3> centres;
            for (u32 i = 0; i < clusters; ++i) centres.push_back(randPoint());
            std::normal_distribution<f32> g(0.0f, 0.12f);
            for (u32 i = 0; i < n; ++i) {
                const glm::vec3& c = centres[i % clusters];
                sites.push_back(glm::clamp(c + glm::vec3(g(rng), g(rng), g(rng)) * ext, mn, mx));
            }
            break;
        }

        case FracturePattern::Radial: {
            // Density falls off with distance from the impact: r = u^falloff maps
            // a uniform variate to a centre-weighted radius.
            const f32 maxR = glm::length(ext) * 0.5f;
            std::normal_distribution<f32> g(0.0f, 1.0f);
            for (u32 i = 0; i < n; ++i) {
                glm::vec3 dir(g(rng), g(rng), g(rng));
                const f32 dl = glm::length(dir);
                dir = dl > kEps ? dir / dl : glm::vec3(0, 1, 0);
                const f32 r = std::pow(u01(rng), glm::max(s.radialFalloff, 1.0f)) * maxR;
                sites.push_back(glm::clamp(s.impactPoint + dir * r, mn, mx));
            }
            break;
        }

        case FracturePattern::Slabs: {
            // Compress the site distribution along one axis so the bisector planes
            // between sites end up mostly PERPENDICULAR to it -> plank-like slabs.
            glm::vec3 axis = glm::length(s.slabAxis) > kEps ? glm::normalize(s.slabAxis)
                                                            : glm::vec3(0, 1, 0);
            const f32 bias = glm::max(s.slabBias, 1.0f);
            for (u32 i = 0; i < n; ++i) {
                glm::vec3 p = randPoint();
                const glm::vec3 centre = (mn + mx) * 0.5f;
                const f32 along = glm::dot(p - centre, axis);
                // Quantise the along-axis coordinate into `bias` bands.
                const f32 band = std::round(along * bias / glm::length(ext)) *
                                 glm::length(ext) / bias;
                p += axis * (band - along);
                sites.push_back(glm::clamp(p, mn, mx));
            }
            break;
        }
    }
    return sites;
}

} // namespace

std::optional<FractureAsset> FractureMesh(const MeshData& source,
                                          const FractureSettings& settings) {
    if (source.vertices.size() < 4 || source.indices.size() < 3) {
        HBE_WARN("Fracture: source mesh is degenerate ({} verts, {} indices).",
                 source.vertices.size(), source.indices.size());
        return std::nullopt;
    }
    glm::vec3 mn, mx;
    ComputeBounds(source, mn, mx);
    const glm::vec3 ext = mx - mn;
    if (ext.x < kEps || ext.y < kEps || ext.z < kEps) {
        HBE_WARN("Fracture: source mesh has zero extent on an axis; cannot fracture.");
        return std::nullopt;
    }

    const std::vector<glm::vec3> sites = ScatterSites(settings, mn, mx);
    const u32 n = static_cast<u32>(sites.size());

    FractureAsset out;
    out.boundsMin = mn;
    out.boundsMax = mx;
    out.settings = settings;
    out.chunks.reserve(n);

    // Build each Voronoi cell by clipping the bounding box with the perpendicular
    // bisector against every other site. O(n^2) planes, but n is a few dozen and
    // this is an offline bake.
    //
    // INDEX DISCIPLINE (this was a real bug): a cell can DIE - clipped to nothing,
    // or zero volume, or coincident sites - so the chunk index is NOT the site
    // index. Cap faces are tagged with the SITE that bisected them, so adjacency
    // must be stored in site space and translated once at the end. Mixing the two
    // silently produces a garbage support graph, and it only shows up when a cell
    // actually dies (which the all-cells-survive test case never exercised).
    const f32 weldTol = glm::max(1e-5f, 1e-4f * glm::length(ext));
    std::vector<std::vector<int>> adjacencyBySite(n); // indexed by SITE, holds SITE ids
    std::vector<int> siteToChunk(n, -1);              // SITE -> chunk index (-1 = died)
    for (u32 i = 0; i < n; ++i) {
        Poly cell = BoxPoly(mn, mx);
        bool alive = true;
        for (u32 j = 0; j < n && alive; ++j) {
            if (i == j) continue;
            const glm::vec3 d = sites[j] - sites[i];
            const f32 len = glm::length(d);
            if (len < kEps) continue; // coincident sites: skip (degenerate bisector)
            const glm::vec3 nrm = d / len;
            // Bisector: points equidistant from i and j. Keep the side nearer i.
            const f32 plane = glm::dot(nrm, (sites[i] + sites[j]) * 0.5f);
            alive = ClipPoly(cell, nrm, plane, static_cast<int>(j), weldTol);
        }
        if (!alive) continue;

        FractureChunk chunk;
        PolyVolumeCentroid(cell, chunk.volume, chunk.centroid);
        if (chunk.volume <= 1e-8f) continue;
        chunk.mesh = PolyToMesh(cell, source.material, mn, mx);
        if (chunk.mesh.Empty()) continue;
        chunk.mesh.name = "chunk_" + std::to_string(out.chunks.size());

        // Adjacency: any surviving cap face tagged with site j means cells i and j
        // share a cut surface, so they are welded until one breaks. Recorded
        // against SITE i, in SITE ids.
        std::vector<int>& adj = adjacencyBySite[i];
        for (const Face& f : cell.faces)
            if (f.fromSite >= 0 &&
                std::find(adj.begin(), adj.end(), f.fromSite) == adj.end())
                adj.push_back(f.fromSite);

        siteToChunk[i] = static_cast<int>(out.chunks.size());
        out.chunks.push_back(std::move(chunk));
    }

    if (out.chunks.empty()) {
        HBE_WARN("Fracture: produced no chunks (all cells clipped away).");
        return std::nullopt;
    }

    // Drop slivers: Voronoi always leaves a few near-zero-volume shards at the
    // hull, and each costs a body + collider + draw for no visual gain.
    f32 meanVol = 0.0f;
    for (const FractureChunk& c : out.chunks) meanVol += c.volume;
    meanVol /= static_cast<f32>(out.chunks.size());
    const f32 minVol = meanVol * glm::clamp(settings.minChunkVolumeFrac, 0.0f, 0.9f);
    const usize before = out.chunks.size();
    // Sliver cull: chunk index -> final index (-1 = culled).
    std::vector<int> chunkRemap(before, -1);
    std::vector<FractureChunk> keep;
    keep.reserve(before);
    for (usize i = 0; i < before; ++i) {
        if (out.chunks[i].volume < minVol) continue;
        chunkRemap[i] = static_cast<int>(keep.size());
        keep.push_back(std::move(out.chunks[i]));
    }
    if (keep.empty()) { // never return nothing
        keep.push_back(std::move(out.chunks[0]));
        chunkRemap[0] = 0;
    }
    out.chunks = std::move(keep);

    // Compose the two mappings ONCE: site -> chunk -> final. Doing it in one place
    // is what keeps the index spaces from being confused again.
    const auto siteToFinal = [&](int site) -> int {
        if (site < 0 || static_cast<usize>(site) >= siteToChunk.size()) return -1;
        const int chunk = siteToChunk[static_cast<usize>(site)];
        if (chunk < 0 || static_cast<usize>(chunk) >= chunkRemap.size()) return -1;
        return chunkRemap[static_cast<usize>(chunk)];
    };

    for (u32 site = 0; site < n; ++site) {
        const int dst = siteToFinal(static_cast<int>(site));
        if (dst < 0) continue;
        std::vector<u32>& nb = out.chunks[static_cast<usize>(dst)].neighbours;
        for (const int otherSite : adjacencyBySite[site]) {
            const int mapped = siteToFinal(otherSite);
            if (mapped >= 0 && mapped != dst) nb.push_back(static_cast<u32>(mapped));
        }
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    }

    // Symmetrise: numerically, one side's cap can survive clipping while the
    // other's is culled, and a direction-dependent support graph makes the
    // structural flood fill give different answers depending on where it starts.
    for (usize i = 0; i < out.chunks.size(); ++i) {
        for (const u32 j : out.chunks[i].neighbours) {
            if (j >= out.chunks.size()) continue;
            std::vector<u32>& back = out.chunks[j].neighbours;
            if (std::find(back.begin(), back.end(), static_cast<u32>(i)) == back.end())
                back.push_back(static_cast<u32>(i));
        }
    }
    for (FractureChunk& c : out.chunks) {
        std::sort(c.neighbours.begin(), c.neighbours.end());
        c.neighbours.erase(std::unique(c.neighbours.begin(), c.neighbours.end()),
                           c.neighbours.end());
    }

    HBE_INFO("Fracture: {} -> {} chunks ({} pattern, seed {}, {} slivers culled).",
             source.name.empty() ? "mesh" : source.name, out.chunks.size(),
             FracturePatternName(settings.pattern), settings.seed,
             before - out.chunks.size());
    return out;
}

// --- .hbfrac serialization --------------------------------------------------
// Binary, not JSON: a 40-chunk fracture is tens of thousands of vertices and the
// JSON form would be megabytes of text to parse at load.

namespace {
constexpr u32 kFracMagic = 0x43415246u; // 'FRAC'
constexpr u32 kFracVersion = 2; // v2: full FractureSettings round-trip

template <typename T>
void W(std::vector<u8>& b, const T& v) {
    const u8* p = reinterpret_cast<const u8*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
void WStr(std::vector<u8>& b, const std::string& s) {
    W(b, static_cast<u32>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
struct Reader {
    const u8* p = nullptr;
    usize left = 0;
    bool ok = true;
    template <typename T>
    T R() {
        T v{};
        if (left < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return v;
    }
    std::string RStr() {
        const u32 n = R<u32>();
        if (!ok || left < n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        left -= n;
        return s;
    }
};
} // namespace

bool SaveFracture(const std::filesystem::path& path, const FractureAsset& a) {
    std::vector<u8> b;
    W(b, kFracMagic);
    W(b, kFracVersion);
    W(b, a.boundsMin);
    W(b, a.boundsMax);
    WStr(b, a.interiorMaterial);
    // FULL settings: the header promises these are kept "for re-fracturing in the
    // editor", which only works if every field survives the round trip.
    W(b, static_cast<u32>(a.settings.pattern));
    W(b, a.settings.cellCount);
    W(b, a.settings.seed);
    W(b, a.settings.impactPoint);
    W(b, a.settings.radialFalloff);
    W(b, a.settings.slabAxis);
    W(b, a.settings.slabBias);
    W(b, a.settings.minChunkVolumeFrac);
    W(b, static_cast<u32>(a.chunks.size()));
    for (const FractureChunk& c : a.chunks) {
        W(b, c.centroid);
        W(b, c.volume);
        W(b, static_cast<u32>(c.anchored ? 1u : 0u));
        W(b, static_cast<u32>(c.neighbours.size()));
        for (const u32 nb : c.neighbours) W(b, nb);
        W(b, static_cast<u32>(c.mesh.vertices.size()));
        if (!c.mesh.vertices.empty()) {
            const u8* vp = reinterpret_cast<const u8*>(c.mesh.vertices.data());
            b.insert(b.end(), vp, vp + c.mesh.vertices.size() * sizeof(Vertex));
        }
        W(b, static_cast<u32>(c.mesh.indices.size()));
        if (!c.mesh.indices.empty()) {
            const u8* ip = reinterpret_cast<const u8*>(c.mesh.indices.data());
            b.insert(b.end(), ip, ip + c.mesh.indices.size() * sizeof(u32));
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        HBE_ERROR("Fracture: cannot write '{}'.", path.string());
        return false;
    }
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

std::optional<FractureAsset> LoadFracture(const std::filesystem::path& path) {
    // Pack-aware: a shipped build serves this from the mounted .uap packs.
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes || bytes->empty()) {
        HBE_ERROR("Fracture: cannot read '{}'.", path.string());
        return std::nullopt;
    }
    Reader r{bytes->data(), bytes->size()};
    if (r.R<u32>() != kFracMagic) {
        HBE_ERROR("Fracture: '{}' is not a .hbfrac.", path.string());
        return std::nullopt;
    }
    const u32 version = r.R<u32>();
    if (version > kFracVersion) {
        HBE_WARN("Fracture: '{}' is version {} (newer than {}).", path.string(), version,
                 kFracVersion);
    }
    FractureAsset a;
    a.boundsMin = r.R<glm::vec3>();
    a.boundsMax = r.R<glm::vec3>();
    a.interiorMaterial = r.RStr();
    a.settings.pattern = static_cast<FracturePattern>(glm::min(r.R<u32>(), 3u));
    a.settings.cellCount = r.R<u32>();
    a.settings.seed = r.R<u32>();
    a.settings.impactPoint = r.R<glm::vec3>();
    a.settings.radialFalloff = r.R<f32>();
    a.settings.slabAxis = r.R<glm::vec3>();
    a.settings.slabBias = r.R<f32>();
    a.settings.minChunkVolumeFrac = r.R<f32>();
    const u32 chunkCount = r.R<u32>();
    if (!r.ok || chunkCount > 100000u) {
        HBE_ERROR("Fracture: '{}' is corrupt.", path.string());
        return std::nullopt;
    }
    a.chunks.resize(chunkCount);
    for (FractureChunk& c : a.chunks) {
        c.centroid = r.R<glm::vec3>();
        c.volume = r.R<f32>();
        c.anchored = r.R<u32>() != 0;
        const u32 nn = r.R<u32>();
        if (!r.ok || nn > chunkCount) { r.ok = false; break; }
        c.neighbours.resize(nn);
        for (u32& nb : c.neighbours) nb = r.R<u32>();
        const u32 vc = r.R<u32>();
        if (!r.ok || r.left < static_cast<usize>(vc) * sizeof(Vertex)) { r.ok = false; break; }
        c.mesh.vertices.resize(vc);
        std::memcpy(c.mesh.vertices.data(), r.p, vc * sizeof(Vertex));
        r.p += vc * sizeof(Vertex);
        r.left -= vc * sizeof(Vertex);
        const u32 ic = r.R<u32>();
        if (!r.ok || r.left < static_cast<usize>(ic) * sizeof(u32)) { r.ok = false; break; }
        c.mesh.indices.resize(ic);
        std::memcpy(c.mesh.indices.data(), r.p, ic * sizeof(u32));
        r.p += ic * sizeof(u32);
        r.left -= ic * sizeof(u32);
    }
    if (!r.ok) {
        HBE_ERROR("Fracture: '{}' truncated or corrupt.", path.string());
        return std::nullopt;
    }
    return a;
}

} // namespace assets
} // namespace hbe

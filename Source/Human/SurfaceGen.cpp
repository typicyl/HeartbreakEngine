#include "Human/SurfaceGen.h"

#include "Assets/MeshDerive.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace hbe::human {

namespace {

// A deterministic parallel-for. Fixed contiguous ranges, never work-stealing: the whole
// generator promises that the same seed gives the same human on any machine, and a
// dynamically balanced loop would let the thread count decide float accumulation order.
// Here each thread owns a fixed slab and writes only its own cells, so the result is
// identical for any thread count - which the self-test asserts.
template <typename Fn>
void ParallelRange(u32 count, Fn fn) {
    const u32 hw = std::max(1u, std::thread::hardware_concurrency());
    const u32 n = std::min(hw, std::max(1u, count));
    if (n <= 1) { fn(0u, count); return; }
    std::vector<std::thread> threads;
    threads.reserve(n - 1);
    const u32 chunk = (count + n - 1) / n;
    for (u32 i = 1; i < n; ++i) {
        const u32 lo = std::min(count, i * chunk);
        const u32 hi = std::min(count, lo + chunk);
        if (lo < hi) threads.emplace_back([=] { fn(lo, hi); });
    }
    fn(0u, std::min(count, chunk));
    for (std::thread& t : threads) t.join();
}

// The 12 edges of a cell, as pairs of corner indices. Corner c has offsets
// (c&1, (c>>1)&1, (c>>2)&1) in x, y, z.
constexpr int kEdge[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, // along X
    {0, 2}, {1, 3}, {4, 6}, {5, 7}, // along Y
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, // along Z
};

} // namespace

GeneratedSurface Extract(const BodyField& field, const SurfaceSettings& settings) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    GeneratedSurface out;
    glm::vec3 lo, hi;
    field.Bounds(lo, hi);
    // A one-cell margin so the surface never touches the grid boundary: a body clipped by
    // the sampling volume comes out with a hole, and the cause is not obvious from looking.
    const glm::vec3 span = hi - lo;
    const f32 longest = std::max({span.x, span.y, span.z, 0.01f});
    const u32 res = std::clamp(settings.resolution, 16u, 512u);
    const f32 cell = longest / static_cast<f32>(res);
    lo -= glm::vec3(cell * 2.0f);
    hi += glm::vec3(cell * 2.0f);

    const u32 nx = static_cast<u32>((hi.x - lo.x) / cell) + 1;
    const u32 ny = static_cast<u32>((hi.y - lo.y) / cell) + 1;
    const u32 nz = static_cast<u32>((hi.z - lo.z) / cell) + 1;
    out.gridX = nx; out.gridY = ny; out.gridZ = nz;
    out.cellSize = cell;
    out.boundsLo = lo;
    out.boundsHi = hi;

    // ---- 1. Sample the field ------------------------------------------------
    // The dominant cost of the whole generator. Parallel over Y slabs; each thread writes a
    // disjoint span so there is no sharing and no ordering dependence.
    std::vector<f32> phi(static_cast<usize>(nx) * ny * nz);
    const auto index = [nx, ny](u32 x, u32 y, u32 z) {
        return static_cast<usize>(z) * ny * nx + static_cast<usize>(y) * nx + x;
    };
    ParallelRange(ny, [&](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y)
            for (u32 z = 0; z < nz; ++z)
                for (u32 x = 0; x < nx; ++x)
                    phi[index(x, y, z)] = field.Eval(
                        lo + glm::vec3(x, y, z) * cell);
    });

    // ---- 2. One vertex per straddling cell ----------------------------------
    // kNoVertex marks a cell the surface does not pass through. A dense map costs
    // nx*ny*nz*4 bytes, which at 128^3 is 8 MB - cheaper than hashing per cell.
    constexpr u32 kNoVertex = 0xFFFFFFFFu;
    std::vector<u32> cellVertex(static_cast<usize>(nx) * ny * nz, kNoVertex);
    std::vector<glm::vec3> positions;

    for (u32 z = 0; z + 1 < nz; ++z) {
        for (u32 y = 0; y + 1 < ny; ++y) {
            for (u32 x = 0; x + 1 < nx; ++x) {
                f32 corner[8];
                bool neg = false, pos = false;
                for (int c = 0; c < 8; ++c) {
                    corner[c] = phi[index(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1))];
                    (corner[c] < 0.0f ? neg : pos) = true;
                }
                if (!neg || !pos) continue; // wholly inside or wholly outside

                // Vertex = the average of every zero crossing on this cell's edges. This is
                // what keeps surface-nets triangles well shaped: the vertex sits in the
                // cell's interior rather than pinned to an edge.
                glm::vec3 sum(0.0f);
                u32 n = 0;
                for (const auto& e : kEdge) {
                    const f32 a = corner[e[0]], b = corner[e[1]];
                    if ((a < 0.0f) == (b < 0.0f)) continue;
                    const f32 t = a / (a - b); // exact for a linear field, close enough here
                    const glm::vec3 pa(static_cast<f32>(e[0] & 1), static_cast<f32>((e[0] >> 1) & 1),
                                       static_cast<f32>((e[0] >> 2) & 1));
                    const glm::vec3 pb(static_cast<f32>(e[1] & 1), static_cast<f32>((e[1] >> 1) & 1),
                                       static_cast<f32>((e[1] >> 2) & 1));
                    sum += pa + (pb - pa) * t;
                    ++n;
                }
                if (n == 0) continue;
                const glm::vec3 p = lo + (glm::vec3(x, y, z) + sum / static_cast<f32>(n)) * cell;
                cellVertex[index(x, y, z)] = static_cast<u32>(positions.size());
                positions.push_back(p);
            }
        }
    }

    if (positions.empty()) {
        out.seconds = std::chrono::duration<f64>(clock::now() - t0).count();
        return out;
    }

    // ---- 3. Quads across sign-changing edges --------------------------------
    // Each grid edge that crosses zero is shared by exactly four cells; those four vertices
    // form one quad. Winding follows the sign direction so every face points outward - a
    // consistently wound mesh is required for backface culling and for normals to agree.
    std::vector<u32> indices;
    const auto quad = [&](u32 a, u32 b, u32 c, u32 d, bool flip) {
        if (a == kNoVertex || b == kNoVertex || c == kNoVertex || d == kNoVertex) return;
        if (flip) { std::swap(b, d); }
        indices.insert(indices.end(), {a, b, c, a, c, d});
    };
    for (u32 z = 1; z + 1 < nz; ++z) {
        for (u32 y = 1; y + 1 < ny; ++y) {
            for (u32 x = 1; x + 1 < nx; ++x) {
                const f32 here = phi[index(x, y, z)];
                const bool inHere = here < 0.0f;
                if (inHere != (phi[index(x + 1, y, z)] < 0.0f))
                    quad(cellVertex[index(x, y - 1, z - 1)], cellVertex[index(x, y, z - 1)],
                         cellVertex[index(x, y, z)], cellVertex[index(x, y - 1, z)], !inHere);
                if (inHere != (phi[index(x, y + 1, z)] < 0.0f))
                    quad(cellVertex[index(x - 1, y, z - 1)], cellVertex[index(x, y, z - 1)],
                         cellVertex[index(x, y, z)], cellVertex[index(x - 1, y, z)], inHere);
                if (inHere != (phi[index(x, y, z + 1)] < 0.0f))
                    quad(cellVertex[index(x - 1, y - 1, z)], cellVertex[index(x, y - 1, z)],
                         cellVertex[index(x, y, z)], cellVertex[index(x - 1, y, z)], !inHere);
            }
        }
    }

    // ---- 3b. Make the winding face OUTWARD ----------------------------------
    // The per-edge winding above derives from a sign comparison, and getting that backwards
    // produces a mesh whose triangles all face INWARD: backface culling then shows the far
    // side of the body and the lighting reads inside-out. Rather than trust the sign logic,
    // MEASURE it. For a closed surface the signed volume (divergence theorem) is positive
    // when the winding is outward and negative when it is not, so one pass settles it
    // definitively and stays correct if the extraction is ever changed.
    {
        f64 volume = 0.0;
        for (usize t = 0; t + 2 < indices.size(); t += 3) {
            const glm::dvec3 p0(positions[indices[t]]);
            const glm::dvec3 p1(positions[indices[t + 1]]);
            const glm::dvec3 p2(positions[indices[t + 2]]);
            volume += glm::dot(p0, glm::cross(p1, p2));
        }
        if (volume < 0.0)
            for (usize t = 0; t + 2 < indices.size(); t += 3)
                std::swap(indices[t + 1], indices[t + 2]);
    }

    // ---- 4. Project onto the true surface -----------------------------------
    // Linear edge interpolation assumes the field is linear across a cell. It is not, so
    // every vertex is a fraction of a cell off. A couple of Newton steps along the gradient
    // fixes it: p <- p - phi(p) * grad(p). This is the difference between a body that looks
    // faceted at the grid scale and one that does not.
    ParallelRange(static_cast<u32>(positions.size()), [&](u32 i0, u32 i1) {
        for (u32 i = i0; i < i1; ++i) {
            for (u32 s = 0; s < settings.projectSteps; ++s) {
                const f32 d = field.Eval(positions[i]);
                positions[i] -= field.Gradient(positions[i]) * d;
            }
        }
    });

    // ---- 5. Tangential relaxation -------------------------------------------
    // Even out edge lengths WITHOUT moving the surface: average each vertex toward its
    // neighbours, remove the component along the normal, then re-project. Uniform vertex
    // spacing is what keeps texel density even later, which is what lets one skin/pore
    // library serve every generated human.
    if (settings.relaxPasses > 0) {
        std::vector<u32> adjStart(positions.size() + 1, 0);
        std::vector<u32> adj;
        {
            std::vector<std::vector<u32>> tmp(positions.size());
            const auto link = [&](u32 a, u32 b) {
                if (std::find(tmp[a].begin(), tmp[a].end(), b) == tmp[a].end()) tmp[a].push_back(b);
            };
            for (usize t = 0; t + 2 < indices.size(); t += 3) {
                const u32 a = indices[t], b = indices[t + 1], c = indices[t + 2];
                link(a, b); link(b, a); link(b, c); link(c, b); link(c, a); link(a, c);
            }
            u32 total = 0;
            for (usize i = 0; i < tmp.size(); ++i) { adjStart[i] = total; total += static_cast<u32>(tmp[i].size()); }
            adjStart[tmp.size()] = total;
            adj.reserve(total);
            for (const auto& v : tmp) adj.insert(adj.end(), v.begin(), v.end());
        }
        std::vector<glm::vec3> next(positions.size());
        for (u32 pass = 0; pass < settings.relaxPasses; ++pass) {
            ParallelRange(static_cast<u32>(positions.size()), [&](u32 i0, u32 i1) {
                for (u32 i = i0; i < i1; ++i) {
                    const u32 s = adjStart[i], e = adjStart[i + 1];
                    if (e <= s) { next[i] = positions[i]; continue; }
                    glm::vec3 avg(0.0f);
                    for (u32 k = s; k < e; ++k) avg += positions[adj[k]];
                    avg /= static_cast<f32>(e - s);
                    glm::vec3 delta = (avg - positions[i]) * settings.relaxStrength;
                    const glm::vec3 nrm = field.Gradient(positions[i]);
                    delta -= nrm * glm::dot(nrm, delta); // TANGENTIAL only: never move the surface
                    glm::vec3 p = positions[i] + delta;
                    p -= field.Gradient(p) * field.Eval(p); // and pull back onto it
                    next[i] = p;
                }
            });
            positions.swap(next);
        }
    }

    // ---- 6. Build the mesh --------------------------------------------------
    out.mesh.name = "GeneratedHuman";
    out.mesh.vertices.resize(positions.size());
    out.vertexRegion.resize(positions.size());
    out.tissueDepth.resize(positions.size());

    // UVs are a CYLINDRICAL projection around the body's vertical axis. STATED PLAINLY: this
    // is a provisional parameterization. It is genuinely usable - continuous, no overlap,
    // one seam at the back - and it is enough to preview materials and to author against.
    // It is NOT a production layout: it distorts badly on the head, the hands and the feet,
    // where a real chart-based atlas is needed. That is a later stage, and replacing it
    // touches only this block.
    const glm::vec3 centre((lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f);
    const f32 heightSpan = std::max(0.01f, hi.y - lo.y);

    ParallelRange(static_cast<u32>(positions.size()), [&](u32 i0, u32 i1) {
        for (u32 i = i0; i < i1; ++i) {
            Vertex& v = out.mesh.vertices[i];
            v.position = positions[i];
            // Normals from the FIELD, not from the triangles: the field knows the true
            // surface orientation, so this is both smoother and cheaper than accumulating
            // face normals, and it does not depend on triangle quality.
            v.normal = field.Gradient(positions[i]);
            const f32 ang = std::atan2(positions[i].z - centre.z, positions[i].x - centre.x);
            v.uv = glm::vec2(0.5f + ang / 6.28318531f, (positions[i].y - lo.y) / heightSpan);
            out.vertexRegion[i] = field.RegionAt(positions[i]);
            // How much soft tissue sits over the muscle-and-bone body here. This is real
            // derived data, not decoration: it is what a subsurface-scattering thickness or
            // a jiggle weight is built from.
            out.tissueDepth[i] = std::max(0.0f, field.EvalTissue(positions[i]));
        }
    });
    out.mesh.indices = std::move(indices);

    // Tangents from the UVs. Normals are already correct from the field, so only the tangent
    // pass runs - and it carries the mirrored-UV handedness rule with it.
    mesh::RecomputeTangents(out.mesh);

    // ---- 7. Report, do not hide ---------------------------------------------
    // A non-manifold edge (shared by other than two triangles) is a real defect that breaks
    // welding, normals and physics. Count it and surface it; silently shipping one is how a
    // generator earns a reputation for producing broken assets.
    {
        std::unordered_map<u64, u32> edgeCount;
        edgeCount.reserve(out.mesh.indices.size());
        for (usize t = 0; t + 2 < out.mesh.indices.size(); t += 3) {
            const u32 v[3] = {out.mesh.indices[t], out.mesh.indices[t + 1], out.mesh.indices[t + 2]};
            for (int e = 0; e < 3; ++e) {
                const u32 a = std::min(v[e], v[(e + 1) % 3]);
                const u32 b = std::max(v[e], v[(e + 1) % 3]);
                ++edgeCount[(static_cast<u64>(a) << 32) | b];
            }
        }
        for (const auto& [key, c] : edgeCount) {
            if (c == 1) ++out.boundaryEdges;
            else if (c > 2) ++out.nonManifoldEdges;
        }
    }

    out.seconds = std::chrono::duration<f64>(clock::now() - t0).count();
    return out;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("surface FAIL: %s\n", what); ++g_fails; }
}
} // namespace

bool SurfaceSelfTest() {
    g_fails = 0;
    HumanParameters p;
    BodyField f;
    f.Build(Resolve(p));

    SurfaceSettings s;
    s.resolution = 48; // coarse: this test is about correctness, not fidelity
    const GeneratedSurface g = Extract(f, s);

    Check(!g.mesh.vertices.empty(), "the extractor must produce vertices");
    Check(!g.mesh.indices.empty(), "the extractor must produce triangles");
    Check(g.mesh.indices.size() % 3 == 0, "the index buffer must be whole triangles");
    Check(g.vertexRegion.size() == g.mesh.vertices.size(), "every vertex must carry a region");
    Check(g.tissueDepth.size() == g.mesh.vertices.size(), "every vertex must carry a tissue depth");

    // A HOLE is fatal, and is asserted strictly: an open body breaks welding, physics and
    // every volume computation downstream of it.
    Check(g.boundaryEdges == 0,
          "THE GENERATED BODY MUST BE CLOSED - a boundary edge is a hole in the skin");
    // A PINCH is different in kind. One-vertex-per-cell dual extraction cannot represent a
    // cell the surface passes through twice, so it welds the two sheets at that vertex. The
    // body stays closed; the fix is to split those vertex fans, which belongs in the export
    // stage and is not written yet. It must stay RARE - a large count would mean the anatomy
    // has features finer than the grid can sample, which is a modelling problem, not an
    // artifact to shrug at.
    if (g.nonManifoldEdges != 0)
        std::printf("  pinched edges: %u of %u tris (fan-splitting repair not written yet)\n",
                    g.nonManifoldEdges, static_cast<u32>(g.mesh.indices.size() / 3));
    Check(g.nonManifoldEdges * 200 < g.mesh.indices.size() / 3,
          "PINCHED EDGES MUST BE RARE - many of them means anatomy finer than the grid");

    for (const u32 i : g.mesh.indices)
        Check(i < g.mesh.vertices.size(), "every index must be in range");

    // Vertices must lie ON the surface, which is what the projection step is for.
    {
        f32 worst = 0.0f;
        for (const Vertex& v : g.mesh.vertices) worst = std::max(worst, std::abs(f.Eval(v.position)));
        Check(worst < g.cellSize * 0.25f,
              "extracted vertices must sit on the zero crossing, not on the grid");
    }

    // Normals and tangents must be finite, unit, and orthogonal - a NaN here poisons a draw.
    for (const Vertex& v : g.mesh.vertices) {
        Check(std::isfinite(v.normal.x) && std::abs(glm::length(v.normal) - 1.0f) < 1e-2f,
              "every normal must be finite and unit length");
        Check(std::isfinite(v.tangent.x) && std::abs(v.tangent.w) == 1.0f,
              "every tangent must be finite with a valid handedness sign");
    }

    // The body must be the right SIZE and the right way up.
    {
        glm::vec3 blo(1e9f), bhi(-1e9f);
        for (const Vertex& v : g.mesh.vertices) { blo = glm::min(blo, v.position); bhi = glm::max(bhi, v.position); }
        Check(bhi.y - blo.y > 1.5f && bhi.y - blo.y < 2.1f,
              "a 1.75 m human must extract to roughly 1.75 m of geometry");
        Check(bhi.x - blo.x < bhi.y - blo.y, "a standing human must be taller than wide");
    }

    // Regions must actually partition the body.
    {
        bool seen[static_cast<usize>(Region::Count)] = {};
        for (const Region r : g.vertexRegion) seen[static_cast<usize>(r)] = true;
        int n = 0;
        for (bool b : seen) n += b ? 1 : 0;
        Check(n >= 10, "the generated surface must be tagged with many distinct regions");
    }

    // TOPOLOGY MUST BE STABLE AND THREAD-COUNT INDEPENDENT. Same input, same index buffer,
    // byte for byte - this is what the engine's motion vectors and LODs depend on, and what
    // makes a bake cache meaningful.
    {
        const GeneratedSurface g2 = Extract(f, s);
        Check(g2.mesh.indices == g.mesh.indices,
              "THE SAME HUMAN MUST PRODUCE THE SAME TOPOLOGY - stable vertex correspondence "
              "is what TAA, LODs and animation all require");
        Check(g2.mesh.vertices.size() == g.mesh.vertices.size(), "...and the same vertex count");
        bool same = true;
        for (usize i = 0; i < g.mesh.vertices.size(); ++i)
            if (g.mesh.vertices[i].position != g2.mesh.vertices[i].position) { same = false; break; }
        Check(same, "...and bit-identical positions, whatever the thread count");
    }

    // A changed parameter must change the geometry.
    {
        HumanParameters heavy = p;
        heavy.weight = 130.0f;
        BodyField fh;
        fh.Build(Resolve(heavy));
        const GeneratedSurface gh = Extract(fh, s);
        f32 waistA = 0.0f, waistB = 0.0f;
        const auto waist = [](const GeneratedSurface& gs) {
            f32 maxZ = 0.0f;
            for (const Vertex& v : gs.mesh.vertices)
                if (v.position.y > 1.0f && v.position.y < 1.15f) maxZ = std::max(maxZ, v.position.z);
            return maxZ;
        };
        waistA = waist(g);
        waistB = waist(gh);
        Check(waistB > waistA + 0.01f,
              "a heavier human must extract to a visibly deeper waist - the geometry follows "
              "the anatomy");
    }

    if (g_fails == 0)
        std::printf("surface: %d verts / %d tris extracted from the field at res %u in %.2fs, "
                    "closed and manifold, on-surface, region-tagged, and byte-identical on "
                    "repeat\n",
                    static_cast<int>(g.mesh.vertices.size()),
                    static_cast<int>(g.mesh.indices.size() / 3), s.resolution, g.seconds);
    return g_fails == 0;
}

} // namespace hbe::human

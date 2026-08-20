// Assets/MeshCsg.cpp - see MeshCsg.h. BSP constructive solid geometry over convex box brushes.
#include "Assets/MeshCsg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace hbe::csg {
namespace {

constexpr f32 kEps = 1e-5f;

// A convex polygon: ordered vertices + its supporting plane (normal, w) with plane eq n.p == w.
// Fragments produced by clipping stay coplanar with their parent, so they INHERIT the parent plane
// rather than recomputing it (recomputing from a sliver is numerically unstable).
struct Poly {
    std::vector<glm::vec3> v;
    glm::vec3 n{0.0f, 1.0f, 0.0f};
    f32 w = 0.0f;
    void Flip() {
        std::reverse(v.begin(), v.end());
        n = -n;
        w = -w;
    }
};

Poly MakePoly(std::vector<glm::vec3> verts) {
    Poly p;
    p.v = std::move(verts);
    // Plane from the first non-degenerate corner triple (box faces are well-conditioned).
    for (usize i = 2; i < p.v.size(); ++i) {
        const glm::vec3 c = glm::cross(p.v[1] - p.v[0], p.v[i] - p.v[0]);
        const f32 len = glm::length(c);
        if (len > kEps) {
            p.n = c / len;
            p.w = glm::dot(p.n, p.v[0]);
            break;
        }
    }
    return p;
}

// Classify + split `poly` against a plane, appending each fragment to the matching bucket. Mirrors
// csg.js Plane.splitPolygon exactly (coplanar handling by facing, spanning by edge interpolation).
enum : int { COPLANAR = 0, FRONT = 1, BACK = 2, SPANNING = 3 };

void SplitPolygon(const glm::vec3& pn, f32 pw, const Poly& poly, std::vector<Poly>& coplanarFront,
                  std::vector<Poly>& coplanarBack, std::vector<Poly>& front,
                  std::vector<Poly>& back) {
    int polyType = 0;
    const usize n = poly.v.size();
    std::vector<int> types(n);
    for (usize i = 0; i < n; ++i) {
        const f32 t = glm::dot(pn, poly.v[i]) - pw;
        const int type = t < -kEps ? BACK : (t > kEps ? FRONT : COPLANAR);
        polyType |= type;
        types[i] = type;
    }
    switch (polyType) {
        case COPLANAR:
            (glm::dot(pn, poly.n) > 0.0f ? coplanarFront : coplanarBack).push_back(poly);
            break;
        case FRONT:
            front.push_back(poly);
            break;
        case BACK:
            back.push_back(poly);
            break;
        default: { // SPANNING
            std::vector<glm::vec3> f, b;
            for (usize i = 0; i < n; ++i) {
                const usize j = (i + 1) % n;
                const int ti = types[i], tj = types[j];
                const glm::vec3& vi = poly.v[i];
                const glm::vec3& vj = poly.v[j];
                if (ti != BACK) f.push_back(vi);
                if (ti != FRONT) b.push_back(vi);
                if ((ti | tj) == SPANNING) {
                    const f32 denom = glm::dot(pn, vj - vi);
                    const f32 t = std::abs(denom) > 1e-12f ? (pw - glm::dot(pn, vi)) / denom : 0.0f;
                    const glm::vec3 mid = vi + (vj - vi) * t;
                    f.push_back(mid);
                    b.push_back(mid);
                }
            }
            if (f.size() >= 3) {
                Poly pf = poly; // inherit parent plane (fragment is coplanar with the parent)
                pf.v = std::move(f);
                front.push_back(std::move(pf));
            }
            if (b.size() >= 3) {
                Poly pb = poly;
                pb.v = std::move(b);
                back.push_back(std::move(pb));
            }
            break;
        }
    }
}

// A BSP node (csg.js Node): partition plane + coplanar polys + front/back subtrees.
struct Node {
    bool hasPlane = false;
    glm::vec3 pn{0.0f, 1.0f, 0.0f};
    f32 pw = 0.0f;
    std::vector<Poly> polys;
    std::unique_ptr<Node> front, back;

    void Build(std::vector<Poly>& list) {
        if (list.empty()) return;
        if (!hasPlane) {
            hasPlane = true;
            pn = list[0].n;
            pw = list[0].w;
        }
        std::vector<Poly> f, b;
        for (const Poly& p : list) SplitPolygon(pn, pw, p, polys, polys, f, b);
        if (!f.empty()) {
            if (!front) front = std::make_unique<Node>();
            front->Build(f);
        }
        if (!b.empty()) {
            if (!back) back = std::make_unique<Node>();
            back->Build(b);
        }
    }

    std::vector<Poly> ClipPolygons(const std::vector<Poly>& list) const {
        if (!hasPlane) return list;
        std::vector<Poly> f, b;
        for (const Poly& p : list) SplitPolygon(pn, pw, p, f, b, f, b);
        std::vector<Poly> fr = front ? front->ClipPolygons(f) : f;
        std::vector<Poly> bk = back ? back->ClipPolygons(b) : std::vector<Poly>{};
        fr.insert(fr.end(), std::make_move_iterator(bk.begin()), std::make_move_iterator(bk.end()));
        return fr;
    }

    void ClipTo(const Node& bsp) {
        polys = bsp.ClipPolygons(polys);
        if (front) front->ClipTo(bsp);
        if (back) back->ClipTo(bsp);
    }

    void Invert() {
        for (Poly& p : polys) p.Flip();
        pn = -pn;
        pw = -pw;
        if (front) front->Invert();
        if (back) back->Invert();
        std::swap(front, back);
    }

    void All(std::vector<Poly>& out) const {
        out.insert(out.end(), polys.begin(), polys.end());
        if (front) front->All(out);
        if (back) back->All(out);
    }
};

// result = A - B (csg.js CSG.subtract).
std::vector<Poly> Subtract(std::vector<Poly> a, std::vector<Poly> b) {
    Node na, nb;
    na.Build(a);
    nb.Build(b);
    na.Invert();
    na.ClipTo(nb);
    nb.ClipTo(na);
    nb.Invert();
    nb.ClipTo(na);
    nb.Invert();
    std::vector<Poly> bp;
    nb.All(bp);
    na.Build(bp);
    na.Invert();
    std::vector<Poly> out;
    na.All(out);
    return out;
}

// The 6 faces of a Box, CCW-outward (matching the engine cube winding), transformed to the box's
// space. Each face plane is recomputed from the transformed corners, so rotation + non-uniform
// scale are exact.
std::vector<Poly> BoxToPolys(const Box& box) {
    const glm::vec3 h = box.half;
    // Face corner sets in CCW-outward order (verified against GenerateCube's winding).
    const std::array<std::array<glm::vec3, 4>, 6> faces = {{
        {{{h.x, -h.y, -h.z}, {h.x, h.y, -h.z}, {h.x, h.y, h.z}, {h.x, -h.y, h.z}}},     // +X
        {{{-h.x, -h.y, h.z}, {-h.x, h.y, h.z}, {-h.x, h.y, -h.z}, {-h.x, -h.y, -h.z}}}, // -X
        {{{-h.x, h.y, -h.z}, {-h.x, h.y, h.z}, {h.x, h.y, h.z}, {h.x, h.y, -h.z}}},     // +Y
        {{{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z}}}, // -Y
        {{{-h.x, -h.y, h.z}, {h.x, -h.y, h.z}, {h.x, h.y, h.z}, {-h.x, h.y, h.z}}},     // +Z
        {{{h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x, h.y, -h.z}, {h.x, h.y, -h.z}}}, // -Z
    }};
    std::vector<Poly> out;
    out.reserve(6);
    for (const auto& f : faces) {
        std::vector<glm::vec3> verts(4);
        for (int i = 0; i < 4; ++i) verts[i] = glm::vec3(box.transform * glm::vec4(f[i], 1.0f));
        out.push_back(MakePoly(std::move(verts)));
    }
    return out;
}

// World-planar tiling UV from a point + face normal (project onto the two non-dominant axes).
glm::vec2 PlanarUV(const glm::vec3& p, const glm::vec3& n, f32 uvScale) {
    const glm::vec3 a = glm::abs(n);
    const f32 inv = uvScale > 1e-6f ? 1.0f / uvScale : 1.0f;
    if (a.x >= a.y && a.x >= a.z) return {p.z * inv, p.y * inv}; // dominant X -> ZY
    if (a.y >= a.x && a.y >= a.z) return {p.x * inv, p.z * inv}; // dominant Y -> XZ
    return {p.x * inv, p.y * inv};                                // dominant Z -> XY
}

glm::vec4 PlanarTangent(const glm::vec3& n) {
    const glm::vec3 a = glm::abs(n);
    if (a.x >= a.y && a.x >= a.z) return {0, 0, 1, 1}; // U runs along +Z
    return {1, 0, 0, 1};                                // U runs along +X
}

MeshData PolysToMesh(const std::vector<Poly>& polys, f32 uvScale) {
    MeshData m;
    m.name = "Brush";
    for (const Poly& p : polys) {
        if (p.v.size() < 3) continue;
        const glm::vec3 n = p.n;
        const glm::vec4 tan = PlanarTangent(n);
        const u32 base = m.VertexCount();
        for (const glm::vec3& pos : p.v) {
            Vertex vtx;
            vtx.position = pos;
            vtx.normal = n;
            vtx.tangent = tan;
            vtx.uv = PlanarUV(pos, n, uvScale);
            m.vertices.push_back(vtx);
        }
        // Fan-triangulate the convex polygon.
        for (u32 i = 1; i + 1 < static_cast<u32>(p.v.size()); ++i) {
            m.indices.push_back(base);
            m.indices.push_back(base + i);
            m.indices.push_back(base + i + 1);
        }
    }
    return m;
}

} // namespace

MeshData CarveBox(const Box& body, const std::vector<Box>& cutters, f32 uvScale) {
    std::vector<Poly> solid = BoxToPolys(body);
    for (const Box& cutter : cutters) {
        // Skip cutters that cannot possibly overlap the body (cheap AABB reject in body space would
        // need bounds; the BSP handles non-overlap correctly anyway, just skip empty half-extents).
        if (cutter.half.x <= kEps || cutter.half.y <= kEps || cutter.half.z <= kEps) continue;
        solid = Subtract(std::move(solid), BoxToPolys(cutter));
        if (solid.empty()) break; // fully carved away
    }
    return PolysToMesh(solid, uvScale);
}

bool PointSolid(const glm::vec3& p, const Box& body, const std::vector<Box>& cutters) {
    const auto inBox = [&](const Box& box) {
        const glm::vec3 local = glm::vec3(glm::inverse(box.transform) * glm::vec4(p, 1.0f));
        return std::abs(local.x) <= box.half.x + kEps && std::abs(local.y) <= box.half.y + kEps &&
               std::abs(local.z) <= box.half.z + kEps;
    };
    if (!inBox(body)) return false;
    for (const Box& c : cutters)
        if (inBox(c)) return false;
    return true;
}

} // namespace hbe::csg

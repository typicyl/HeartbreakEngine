// Assets/MeshGenerator.cpp
#include "Assets/MeshGenerator.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace hbe::mesh {

MeshData GenerateCube(f32 size) {
    const f32 h = size * 0.5f;
    MeshData m;
    m.name = "Cube";

    // Per face: normal, tangent (u-axis), and the 4 corners in CCW order.
    struct Face { glm::vec3 n, t, origin, du, dv; };
    const Face faces[6] = {
        // +X
        {{ 1, 0, 0}, {0, 0,-1}, { h,-h, h}, {0, 0,-1}, {0, 1, 0}},
        // -X
        {{-1, 0, 0}, {0, 0, 1}, {-h,-h,-h}, {0, 0, 1}, {0, 1, 0}},
        // +Y
        {{ 0, 1, 0}, {1, 0, 0}, {-h, h, h}, {1, 0, 0}, {0, 0,-1}},
        // -Y
        {{ 0,-1, 0}, {1, 0, 0}, {-h,-h,-h}, {1, 0, 0}, {0, 0, 1}},
        // +Z
        {{ 0, 0, 1}, {1, 0, 0}, {-h,-h, h}, {1, 0, 0}, {0, 1, 0}},
        // -Z
        {{ 0, 0,-1}, {-1,0, 0}, { h,-h,-h}, {-1,0, 0}, {0, 1, 0}},
    };

    // Each face gets its OWN square cell in a 4x4 UV grid (6 of 16 cells used),
    // so the faces don't share texture space - painting (or texturing) one face
    // never bleeds onto the others, and a uniform cube maps without stretch.
    const int cellCol[6] = {0, 1, 2, 3, 0, 1};
    const int cellRow[6] = {0, 0, 0, 0, 1, 1};
    const f32 cw = 0.25f;
    int fi = 0;
    for (const Face& f : faces) {
        const u32 base = m.VertexCount();
        const glm::vec2 luv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; // corner UVs in the cell
        const f32 ou = cellCol[fi] * cw, ov = cellRow[fi] * cw;
        const glm::vec3 corners[4] = {
            f.origin,
            f.origin + f.du,
            f.origin + f.du + f.dv,
            f.origin + f.dv,
        };
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = corners[i];
            v.normal   = f.n;
            v.tangent  = glm::vec4(f.t, 1.0f);
            v.uv       = glm::vec2(ou + luv[i].x * cw, ov + luv[i].y * cw);
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(),
                         {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
        ++fi;
    }
    return m;
}

MeshData GenerateSphere(f32 radius, u32 segments, u32 rings) {
    if (segments < 3) segments = 3;
    if (rings < 2) rings = 2;

    MeshData m;
    m.name = "Sphere";
    m.vertices.reserve(static_cast<usize>(rings + 1) * (segments + 1));

    const f32 pi = glm::pi<f32>();
    for (u32 y = 0; y <= rings; ++y) {
        const f32 v   = static_cast<f32>(y) / static_cast<f32>(rings);
        const f32 phi = v * pi;                 // 0 (top) .. PI (bottom)
        const f32 sinPhi = std::sin(phi);
        const f32 cosPhi = std::cos(phi);

        for (u32 x = 0; x <= segments; ++x) {
            const f32 u     = static_cast<f32>(x) / static_cast<f32>(segments);
            const f32 theta = u * 2.0f * pi;
            const f32 sinT  = std::sin(theta);
            const f32 cosT  = std::cos(theta);

            const glm::vec3 n{sinPhi * cosT, cosPhi, sinPhi * sinT};
            // Tangent = d(position)/d(theta), pointing along +u.
            const glm::vec3 t{-sinT, 0.0f, cosT};

            Vertex vert;
            vert.position = n * radius;
            vert.normal   = n;
            vert.tangent  = glm::vec4(t, 1.0f);
            vert.uv       = {u, v};
            m.vertices.push_back(vert);
        }
    }

    const u32 stride = segments + 1;
    for (u32 y = 0; y < rings; ++y) {
        for (u32 x = 0; x < segments; ++x) {
            const u32 i0 = y * stride + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1;
            // CCW-front: rows run top->bottom (+phi), so wind i0->i1->i2 outward.
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    return m;
}

MeshData GeneratePlane(f32 size, u32 subdivisions) {
    if (subdivisions < 1) subdivisions = 1;
    const f32 h = size * 0.5f;
    const u32 n = subdivisions;
    MeshData m;
    m.name = "Plane";
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            const f32 fx = static_cast<f32>(x) / n;
            const f32 fz = static_cast<f32>(z) / n;
            Vertex v;
            v.position = {-h + fx * size, 0.0f, -h + fz * size};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            v.uv = {fx, fz};
            m.vertices.push_back(v);
        }
    }
    const u32 stride = n + 1;
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 i0 = z * stride + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1;
            // Wind so the geometric front faces +Y (up), matching the +Y shading
            // normal and the cube's top face. The previous order ({i0,i1,i3,...})
            // faced -Y, so back-face culling dropped the floor when viewed from
            // above. Verified front-normal = (i3-i0)x(i1-i0) = +Y.
            m.indices.insert(m.indices.end(), {i0, i3, i1, i0, i2, i3});
        }
    }
    return m;
}

namespace {
// Adds a triangle fan cap (a disc) at height `y` with the given outward normal.
// `uvCenter`/`uvScale` place the disc within the canvas so caps + side wall get
// DISJOINT UV regions (else painting/texturing one bleeds onto the others).
void AddDisc(MeshData& m, f32 radius, f32 y, u32 segments, glm::vec3 normal,
             glm::vec2 uvCenter = {0.5f, 0.5f}, f32 uvScale = 0.5f) {
    const f32 pi = glm::pi<f32>();
    const u32 center = m.VertexCount();
    Vertex c;
    c.position = {0.0f, y, 0.0f};
    c.normal = normal;
    c.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    c.uv = uvCenter;
    m.vertices.push_back(c);
    for (u32 i = 0; i <= segments; ++i) {
        const f32 t = static_cast<f32>(i) / segments * 2.0f * pi;
        Vertex v;
        v.position = {std::cos(t) * radius, y, std::sin(t) * radius};
        v.normal = normal;
        v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
        v.uv = {std::cos(t) * uvScale + uvCenter.x, std::sin(t) * uvScale + uvCenter.y};
        m.vertices.push_back(v);
    }
    for (u32 i = 0; i < segments; ++i) {
        const u32 a = center + 1 + i;
        const u32 b = center + 1 + i + 1;
        // Wind so the face points along `normal`.
        if (normal.y >= 0.0f) m.indices.insert(m.indices.end(), {center, b, a});
        else m.indices.insert(m.indices.end(), {center, a, b});
    }
}
} // namespace

MeshData GenerateCylinder(f32 radius, f32 height, u32 segments) {
    if (segments < 3) segments = 3;
    const f32 pi = glm::pi<f32>();
    const f32 hy = height * 0.5f;
    MeshData m;
    m.name = "Cylinder";
    // Side wall (two rings).
    const u32 stride = segments + 1;
    for (u32 ring = 0; ring < 2; ++ring) {
        const f32 y = ring == 0 ? -hy : hy;
        for (u32 i = 0; i <= segments; ++i) {
            const f32 t = static_cast<f32>(i) / segments * 2.0f * pi;
            const glm::vec3 n{std::cos(t), 0.0f, std::sin(t)};
            Vertex v;
            v.position = {n.x * radius, y, n.z * radius};
            v.normal = n;
            v.tangent = {-std::sin(t), 0.0f, std::cos(t), 1.0f};
            // UV atlas (no overlap): side wall fills the bottom band v[0,0.48];
            // the two caps live in the top-left / top-right discs below.
            v.uv = {static_cast<f32>(i) / segments, static_cast<f32>(ring) * 0.48f};
            m.vertices.push_back(v);
        }
    }
    for (u32 i = 0; i < segments; ++i) {
        const u32 i0 = i, i1 = i + 1, i2 = stride + i, i3 = stride + i + 1;
        m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
    }
    AddDisc(m, radius, hy, segments, {0, 1, 0}, {0.25f, 0.75f}, 0.23f);  // top cap
    AddDisc(m, radius, -hy, segments, {0, -1, 0}, {0.75f, 0.75f}, 0.23f); // bottom cap
    return m;
}

MeshData GenerateCone(f32 radius, f32 height, u32 segments) {
    if (segments < 3) segments = 3;
    const f32 pi = glm::pi<f32>();
    const f32 hy = height * 0.5f;
    MeshData m;
    m.name = "Cone";
    // Side: apex duplicated per segment so each face gets a proper normal.
    const f32 slope = radius / glm::max(height, 1e-4f);
    for (u32 i = 0; i < segments; ++i) {
        const f32 t0 = static_cast<f32>(i) / segments * 2.0f * pi;
        const f32 t1 = static_cast<f32>(i + 1) / segments * 2.0f * pi;
        const f32 tm = (t0 + t1) * 0.5f;
        const glm::vec3 base0{std::cos(t0) * radius, -hy, std::sin(t0) * radius};
        const glm::vec3 base1{std::cos(t1) * radius, -hy, std::sin(t1) * radius};
        const glm::vec3 apex{0.0f, hy, 0.0f};
        const glm::vec3 nApex = glm::normalize(glm::vec3(std::cos(tm), slope, std::sin(tm)));
        const glm::vec3 n0 = glm::normalize(glm::vec3(std::cos(t0), slope, std::sin(t0)));
        const glm::vec3 n1 = glm::normalize(glm::vec3(std::cos(t1), slope, std::sin(t1)));
        const u32 base = m.VertexCount();
        auto push = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
            Vertex v;
            v.position = p;
            v.normal = n;
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            v.uv = uv;
            m.vertices.push_back(v);
        };
        // Side fills the bottom band v[0,0.48]; the base disc sits above it so
        // they don't share UV space (no paint/texture bleed between them).
        push(apex, nApex, {0.5f, 0.48f});
        push(base0, n0, {static_cast<f32>(i) / segments, 0.0f});
        push(base1, n1, {static_cast<f32>(i + 1) / segments, 0.0f});
        m.indices.insert(m.indices.end(), {base, base + 2, base + 1}); // CCW-front (outward)
    }
    AddDisc(m, radius, -hy, segments, {0, -1, 0}, {0.5f, 0.75f}, 0.23f); // base
    return m;
}

MeshData GenerateCapsule(f32 radius, f32 height, u32 segments, u32 rings) {
    if (segments < 3) segments = 3;
    if (rings < 1) rings = 1;
    const f32 pi = glm::pi<f32>();
    const f32 cyl = glm::max(height - 2.0f * radius, 0.0f); // mid-section length
    const f32 halfCyl = cyl * 0.5f;
    MeshData m;
    m.name = "Capsule";
    const u32 stride = segments + 1;

    // Rows of latitude: top hemisphere (offset +halfCyl), bottom (offset -halfCyl).
    // Build a single vertical strip of rings from top pole to bottom pole, with
    // the cylinder inserted at the equator by shifting hemisphere centers.
    auto pushRing = [&](f32 phi, f32 yOffset, f32 vCoord) {
        const f32 sp = std::sin(phi), cp = std::cos(phi);
        for (u32 x = 0; x <= segments; ++x) {
            const f32 t = static_cast<f32>(x) / segments * 2.0f * pi;
            const glm::vec3 n{sp * std::cos(t), cp, sp * std::sin(t)};
            Vertex v;
            v.position = n * radius + glm::vec3(0.0f, yOffset, 0.0f);
            v.normal = n;
            v.tangent = {-std::sin(t), 0.0f, std::cos(t), 1.0f};
            v.uv = {static_cast<f32>(x) / segments, vCoord};
            m.vertices.push_back(v);
        }
    };
    // Top hemisphere: phi 0 (pole) -> pi/2 (equator).
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = (static_cast<f32>(r) / rings) * (pi * 0.5f);
        pushRing(phi, halfCyl, static_cast<f32>(r) / (rings * 2 + 1));
    }
    // Bottom hemisphere: phi pi/2 (equator) -> pi (pole).
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = (pi * 0.5f) + (static_cast<f32>(r) / rings) * (pi * 0.5f);
        pushRing(phi, -halfCyl, static_cast<f32>(rings + 1 + r) / (rings * 2 + 1));
    }
    const u32 totalRows = (rings + 1) * 2;
    for (u32 y = 0; y < totalRows - 1; ++y) {
        for (u32 x = 0; x < segments; ++x) {
            const u32 i0 = y * stride + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1;
            // CCW-front: rows run top->bottom, so wind i0->i1->i2 outward.
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    return m;
}

MeshData GenerateTorus(f32 major, f32 minor, u32 segments, u32 sides) {
    if (segments < 3) segments = 3;
    if (sides < 3) sides = 3;
    const f32 pi = glm::pi<f32>();
    MeshData m;
    m.name = "Torus";
    const u32 stride = sides + 1;
    for (u32 i = 0; i <= segments; ++i) {
        const f32 u = static_cast<f32>(i) / segments * 2.0f * pi;
        const glm::vec3 center{std::cos(u) * major, 0.0f, std::sin(u) * major};
        const glm::vec3 outward{std::cos(u), 0.0f, std::sin(u)};
        for (u32 j = 0; j <= sides; ++j) {
            const f32 v = static_cast<f32>(j) / sides * 2.0f * pi;
            const glm::vec3 n = outward * std::cos(v) + glm::vec3(0.0f, std::sin(v), 0.0f);
            Vertex vert;
            vert.position = center + n * minor;
            vert.normal = n;
            vert.tangent = {-std::sin(u), 0.0f, std::cos(u), 1.0f};
            vert.uv = {static_cast<f32>(i) / segments, static_cast<f32>(j) / sides};
            m.vertices.push_back(vert);
        }
    }
    for (u32 i = 0; i < segments; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 i0 = i * stride + j;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1;
            // CCW-front (outward); was wound inward.
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    return m;
}

MeshData GeneratePrimitive(const std::string& name) {
    if (name == "cube") return GenerateCube(1.0f);
    if (name == "sphere") return GenerateSphere(0.5f, 48, 24);
    if (name == "plane") return GeneratePlane(1.0f, 1);
    if (name == "cylinder") return GenerateCylinder(0.5f, 1.0f, 24);
    if (name == "cone") return GenerateCone(0.5f, 1.0f, 24);
    if (name == "capsule") return GenerateCapsule(0.5f, 2.0f, 24, 8);
    if (name == "torus") return GenerateTorus(0.5f, 0.2f, 32, 16);
    return {};
}

} // namespace hbe::mesh

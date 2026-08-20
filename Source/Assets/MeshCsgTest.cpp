// Assets/MeshCsgTest.cpp - `--test-csg`: BSP CSG correctness for the blockout box brush.
//
// The strongest headless check for a boolean mesh operation is the CLOSED-MESH SIGNED VOLUME
// (divergence theorem: V = 1/6 * sum over triangles of dot(a, cross(b,c))). A correct, closed,
// CCW-outward result reproduces the analytic volume of the carve exactly and is POSITIVE (a negative
// volume would mean inverted winding = an inside-out brush that back-face culling would drop). We
// cross-check with the exact PointSolid oracle and a determinism pass.
#include "Assets/MeshCsg.h"
#include "Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace hbe::csg {
namespace {

int g_fail = 0;
#define CCHECK(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_fail;                                                                              \
            HBE_ERROR("[csg test] FAIL: {}", msg);                                                 \
        }                                                                                          \
    } while (0)

// Signed volume of a (closed) triangle mesh via the divergence theorem.
f64 SignedVolume(const MeshData& m) {
    f64 v = 0.0;
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        const glm::dvec3 a(m.vertices[m.indices[i + 0]].position);
        const glm::dvec3 b(m.vertices[m.indices[i + 1]].position);
        const glm::dvec3 c(m.vertices[m.indices[i + 2]].position);
        v += glm::dot(a, glm::cross(b, c));
    }
    return v / 6.0;
}

Box UnitBox(glm::vec3 center, glm::vec3 half) {
    Box b;
    b.transform = glm::translate(glm::mat4(1.0f), center);
    b.half = half;
    return b;
}

bool Close(f64 a, f64 b, f64 eps = 2e-3) { return std::abs(a - b) < eps; }

void TestPlainBox() {
    const Box body = UnitBox({0, 0, 0}, {0.5f, 0.5f, 0.5f});
    const MeshData m = CarveBox(body, {}, 1.0f);
    CCHECK(!m.Empty(), "plain box produced no geometry");
    const f64 v = SignedVolume(m);
    CCHECK(v > 0.0, "plain box winding is inside-out (negative signed volume)");
    CCHECK(Close(v, 1.0), "plain unit box volume should be 1.0");
    // A unit box is 6 quads -> 12 tris -> 36 indices, 24 fan verts.
    CCHECK(m.IndexCount() == 36, "plain box should triangulate to 12 triangles");
}

void TestInteriorVoid() {
    // Box(2^3=8) minus a fully-enclosed cutter(0.6^3=0.216): a solid shell with an interior cavity.
    const Box body = UnitBox({0, 0, 0}, {1.0f, 1.0f, 1.0f});
    const std::vector<Box> cut = {UnitBox({0, 0, 0}, {0.3f, 0.3f, 0.3f})};
    const MeshData m = CarveBox(body, cut, 1.0f);
    CCHECK(!m.Empty(), "interior-void carve produced no geometry");
    const f64 v = SignedVolume(m);
    CCHECK(Close(v, 8.0 - 0.216), "box-minus-interior-box volume should be 7.784");
    // The cavity centre is carved out; a point in the shell is still solid.
    CCHECK(!PointSolid({0, 0, 0}, body, cut), "cavity centre must not be solid");
    CCHECK(PointSolid({0.9f, 0, 0}, body, cut), "shell wall must stay solid");
}

void TestDoorway() {
    // A thin wall (2 x 2 x 0.2, volume 0.8) with a doorway punched clean through it.
    const Box wall = UnitBox({0, 0, 0}, {1.0f, 1.0f, 0.1f});
    const std::vector<Box> door = {UnitBox({0, 0, 0}, {0.3f, 0.5f, 0.5f})};
    const MeshData m = CarveBox(wall, door, 1.0f);
    CCHECK(!m.Empty(), "doorway carve produced no geometry");
    const f64 v = SignedVolume(m);
    // overlap = 0.6 (x) * 1.0 (y) * 0.2 (z) = 0.12 removed -> 0.68 remains.
    CCHECK(Close(v, 0.8 - 0.12), "wall-minus-doorway volume should be 0.68");
    CCHECK(!PointSolid({0, 0, 0}, wall, door), "doorway opening must be empty");
    CCHECK(PointSolid({0.9f, 0.9f, 0}, wall, door), "wall away from the door must be solid");
}

void TestRotatedCutter() {
    // A rotated cutter still carves the right volume (planes are recomputed from transformed corners).
    const Box body = UnitBox({0, 0, 0}, {1.0f, 1.0f, 1.0f});
    Box cut;
    cut.transform = glm::rotate(glm::mat4(1.0f), 0.7853981f /*45deg*/, glm::vec3(0, 1, 0));
    cut.half = {0.25f, 2.0f, 0.25f}; // a tall thin post fully spanning Y, cutting a hole top-to-bottom
    const MeshData m = CarveBox(body, {cut}, 1.0f);
    CCHECK(!m.Empty(), "rotated carve produced no geometry");
    const f64 v = SignedVolume(m);
    // Post cross-section 0.5x0.5 within the 2x2x2 body, spanning full height 2 -> removes 0.5*0.5*2=0.5.
    CCHECK(Close(v, 8.0 - 0.5, 5e-3), "rotated post carve volume should be 7.5");
}

void TestDeterminism() {
    const Box body = UnitBox({0, 0, 0}, {1.0f, 1.0f, 0.1f});
    const std::vector<Box> door = {UnitBox({0, 0, 0}, {0.3f, 0.5f, 0.5f})};
    const MeshData a = CarveBox(body, door, 2.0f);
    const MeshData b = CarveBox(body, door, 2.0f);
    bool same = a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size();
    if (same)
        for (usize i = 0; i < a.vertices.size(); ++i)
            same = same && a.vertices[i].position == b.vertices[i].position;
    CCHECK(same, "CarveBox is not deterministic");
}

} // namespace

bool SelfTest() {
    g_fail = 0;
    TestPlainBox();
    TestInteriorVoid();
    TestDoorway();
    TestRotatedCutter();
    TestDeterminism();
    if (g_fail == 0)
        HBE_INFO("[csg test] all CSG box-brush blocks passed");
    else
        HBE_ERROR("[csg test] {} check(s) failed", g_fail);
    return g_fail == 0;
}

} // namespace hbe::csg

#include "Assets/MeshDerive.h"

#include "Core/Rng.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace hbe::mesh {

void RecomputeNormals(MeshData& mesh) {
    for (Vertex& v : mesh.vertices) v.normal = glm::vec3(0.0f);

    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
            continue;
        const glm::vec3& p0 = mesh.vertices[a].position;
        const glm::vec3& p1 = mesh.vertices[b].position;
        const glm::vec3& p2 = mesh.vertices[c].position;
        // NOT normalised: the cross product's LENGTH is twice the triangle area, so
        // accumulating it raw is exactly the area weighting we want. Normalising here would
        // let a fan of slivers outvote one large face.
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        mesh.vertices[a].normal += n;
        mesh.vertices[b].normal += n;
        mesh.vertices[c].normal += n;
    }

    for (Vertex& v : mesh.vertices) {
        const f32 len = glm::length(v.normal);
        // A vertex touched by no triangle, or only by degenerate ones, has no defensible
        // normal. Pick a valid one rather than emitting a zero the shader will divide by.
        v.normal = (len > 1e-12f) ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

void RecomputeTangents(MeshData& mesh) {
    const usize n = mesh.vertices.size();
    std::vector<glm::vec3> tan(n, glm::vec3(0.0f));
    std::vector<glm::vec3> bitan(n, glm::vec3(0.0f));

    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        if (ia >= n || ib >= n || ic >= n) continue;
        const Vertex& v0 = mesh.vertices[ia];
        const Vertex& v1 = mesh.vertices[ib];
        const Vertex& v2 = mesh.vertices[ic];

        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        const glm::vec2 d1 = v1.uv - v0.uv;
        const glm::vec2 d2 = v2.uv - v0.uv;

        // THE SIGN OF THIS DETERMINANT IS THE HANDEDNESS. A mirrored UV chart has a
        // negative UV area, which flips the tangent frame - and because it survives the
        // accumulation below, a vertex shared between a mirrored and an unmirrored chart
        // ends up with the two contributions cancelling. That is correct: such a vertex has
        // no consistent frame and must be split in the template, not papered over here.
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue; // collapsed chart: contributes nothing
        const f32 r = 1.0f / det;

        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        tan[ia] += t;  tan[ib] += t;  tan[ic] += t;
        bitan[ia] += b; bitan[ib] += b; bitan[ic] += b;
    }

    for (usize i = 0; i < n; ++i) {
        const glm::vec3 nrm = mesh.vertices[i].normal;
        glm::vec3 t = tan[i];

        // Gram-Schmidt: remove whatever part of the tangent runs along the normal, so the
        // frame stays orthonormal even where the UV mapping is skewed.
        t -= nrm * glm::dot(nrm, t);
        const f32 len = glm::length(t);
        if (len > 1e-8f) {
            t /= len;
        } else {
            // No usable UV gradient here (no UVs at all, a seam pole, or cancelling
            // mirrored contributions). Build an ARBITRARY BUT VALID frame perpendicular to
            // the normal: a NaN or zero tangent silently poisons the whole draw, and a
            // wrong-but-finite frame only affects normal mapping at this one vertex.
            const glm::vec3 axis =
                std::abs(nrm.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            t = glm::normalize(glm::cross(nrm, axis));
        }

        // w = +1 when (N, T, B) is right-handed, -1 when the chart is mirrored. The shader
        // reconstructs the bitangent as cross(N, T) * w, so this one sign is the whole
        // difference between a normal map lighting correctly and lighting inside-out on the
        // mirrored half of a body.
        const f32 w = (glm::dot(glm::cross(nrm, t), bitan[i]) < 0.0f) ? -1.0f : 1.0f;
        mesh.vertices[i].tangent = glm::vec4(t, w);
    }
}

// --- self test ----------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("meshderive FAIL: %s\n", what);
        ++g_fails;
    }
}

Vertex V(glm::vec3 p, glm::vec2 uv) {
    Vertex v;
    v.position = p;
    v.uv = uv;
    return v;
}
} // namespace

bool SelfTest() {
    g_fails = 0;

    // A flat quad in the XY plane, normal +Z, U running +X and V running +Y.
    {
        MeshData m;
        m.vertices = {V({0, 0, 0}, {0, 0}), V({1, 0, 0}, {1, 0}),
                      V({1, 1, 0}, {1, 1}), V({0, 1, 0}, {0, 1})};
        m.indices = {0, 1, 2, 0, 2, 3};
        RecomputeNormalsTangents(m);
        for (const Vertex& v : m.vertices) {
            Check(std::abs(v.normal.z - 1.0f) < 1e-4f, "a flat +Z quad must have a +Z normal");
            Check(std::abs(v.tangent.x - 1.0f) < 1e-4f, "U runs +X, so the tangent must be +X");
            Check(v.tangent.w > 0.0f, "an unmirrored chart must have handedness +1");
            Check(std::abs(glm::dot(glm::vec3(v.tangent), v.normal)) < 1e-5f,
                  "the tangent must be orthogonal to the normal");
        }
    }

    // THE CASE THAT MATTERS: the same geometry with V mirrored. Every human template
    // mirrors its UV islands left-to-right to halve the texture budget, so if this sign is
    // wrong the normal map lights inside-out on exactly half the body.
    {
        MeshData m;
        m.vertices = {V({0, 0, 0}, {0, 1}), V({1, 0, 0}, {1, 1}),
                      V({1, 1, 0}, {1, 0}), V({0, 1, 0}, {0, 0})};
        m.indices = {0, 1, 2, 0, 2, 3};
        RecomputeNormalsTangents(m);
        for (const Vertex& v : m.vertices)
            Check(v.tangent.w < 0.0f,
                  "A MIRRORED UV CHART MUST REPORT HANDEDNESS -1 - this is the sign that "
                  "makes normal maps light correctly on both halves of a body");
    }

    // Area weighting: one big triangle and one sliver meeting at a vertex. An unweighted
    // average would let the sliver pull the normal as hard as the large face.
    {
        MeshData m;
        m.vertices = {V({0, 0, 0}, {0, 0}), V({4, 0, 0}, {1, 0}), V({0, 4, 0}, {0, 1}),
                      V({0.01f, 0, 0.4f}, {0.5f, 0.5f})};
        m.indices = {0, 1, 2, 0, 3, 1};
        RecomputeNormals(m);
        Check(m.vertices[0].normal.z > 0.9f,
              "the large face must dominate a sliver - normals are AREA weighted");
    }

    // Degenerate input must produce a finite, usable frame rather than NaN.
    {
        MeshData m;
        m.vertices = {V({0, 0, 0}, {0, 0}), V({1, 0, 0}, {0, 0}), V({0, 1, 0}, {0, 0})};
        m.indices = {0, 1, 2};
        RecomputeNormalsTangents(m);
        for (const Vertex& v : m.vertices) {
            Check(std::isfinite(v.tangent.x) && std::isfinite(v.tangent.y) &&
                      std::isfinite(v.tangent.z) && std::isfinite(v.tangent.w),
                  "ZERO-AREA UVs MUST NOT PRODUCE NaN - one NaN tangent poisons a whole draw");
            Check(std::abs(glm::length(glm::vec3(v.tangent)) - 1.0f) < 1e-4f,
                  "the fallback frame must still be unit length");
            Check(std::abs(glm::dot(glm::vec3(v.tangent), v.normal)) < 1e-4f,
                  "the fallback frame must still be perpendicular to the normal");
        }
    }

    // An out-of-range index must be skipped, not read.
    {
        MeshData m;
        m.vertices = {V({0, 0, 0}, {0, 0}), V({1, 0, 0}, {1, 0}), V({0, 1, 0}, {0, 1})};
        m.indices = {0, 1, 2, 0, 1, 99};
        RecomputeNormalsTangents(m);
        Check(m.vertices[0].normal.z > 0.9f, "a bad index must be skipped, not crash");
    }

    // Determinism: identical input, identical bytes out. This underwrites every bake key.
    {
        MeshData a, b;
        Rng rng(1234);
        for (int i = 0; i < 64; ++i) {
            const Vertex v = V({rng.Signed(), rng.Signed(), rng.Signed()},
                               {rng.NextFloat(), rng.NextFloat()});
            a.vertices.push_back(v);
            b.vertices.push_back(v);
        }
        for (u32 i = 0; i + 2 < 64; i += 3) {
            for (u32 k = 0; k < 3; ++k) { a.indices.push_back(i + k); b.indices.push_back(i + k); }
        }
        RecomputeNormalsTangents(a);
        RecomputeNormalsTangents(b);
        Check(std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0,
              "the same mesh must derive BYTE-IDENTICAL normals and tangents");
    }

    // The RNG's own contract, checked here because Rng has no other home yet.
    {
        Rng x(42), y(42);
        for (int i = 0; i < 100; ++i) Check(x.NextU64() == y.NextU64(), "same seed, same stream");
        Rng z(43);
        Check(Rng(42).NextU64() != z.NextU64(), "different seeds must diverge");

        Rng base(7);
        Check(base.Split(1).NextU64() != base.Split(2).NextU64(),
              "different salts must give independent streams");
        Check(Rng(7).Split(1).NextU64() == Rng(7).Split(1).NextU64(),
              "Split must be a pure function of (state, salt) - order must not matter");

        Rng f(99);
        for (int i = 0; i < 10000; ++i) {
            const f32 v = f.NextFloat();
            Check(v >= 0.0f && v < 1.0f, "NextFloat must be [0,1) - never exactly 1");
        }
        Hasher h1, h2;
        h1.Mix(0.0f);
        h2.Mix(-0.0f);
        Check(h1.Value() == h2.Value(),
              "-0.0 and 0.0 are the same parameter and must hash the same");
    }

    if (g_fails == 0)
        std::printf("meshderive: area-weighted normals, Gram-Schmidt tangents with MIRRORED-UV "
                    "handedness, finite frames from degenerate input, byte-identical repeats, "
                    "and a seeded stream that splits without renumbering\n");
    return g_fails == 0;
}

} // namespace hbe::mesh

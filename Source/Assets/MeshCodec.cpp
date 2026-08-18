// Assets/MeshCodec.cpp - see MeshCodec.h.
#include "Assets/MeshCodec.h"

#include "Core/Log.h"

#include <glm/glm.hpp>
#include <meshoptimizer.h>

#include <cmath>
#include <cstring>

namespace hbe::meshcodec {
namespace {

// The compact on-disk vertex records. #pragma pack(1) pins the layout so the byte stream
// is compiler-independent (the whole point), and the sizes are multiples of 4 because
// meshopt_encodeVertexBuffer requires stride % 4 == 0.
#pragma pack(push, 1)
struct QVertexStatic {
    u16 px, py, pz;   // position: unorm16 in the submesh AABB
    i16 nx, ny;       // normal:  octahedral snorm16
    i16 tx, ty;       // tangent: octahedral snorm16
    u8  handed;       // tangent.w handedness: 1 => +1, 0 => -1
    u8  pad;          // -> 20 bytes (multiple of 4)
    u16 u, v;         // uv: unorm16 in the submesh UV bounds
};
struct QVertexSkinned {
    QVertexStatic base;
    u16 j0, j1, j2, j3; // joint indices: EXACT
    u8  w0, w1, w2, w3; // skin weights: unorm8 (renormalized on decode)
};
#pragma pack(pop)
static_assert(sizeof(QVertexStatic) == 20, "compact static vertex must be 20 bytes (mult of 4)");
static_assert(sizeof(QVertexSkinned) == 32, "compact skinned vertex must be 32 bytes (mult of 4)");

// Geometry-block flags (u8).
constexpr u8 kFlagSkinned = 1 << 0;

// --- octahedral unit-vector encoding (i16 snorm x2) -----------------------------------
glm::vec2 OctWrap(glm::vec2 v) {
    return (glm::vec2(1.0f) - glm::abs(glm::vec2(v.y, v.x))) *
           glm::vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}
glm::vec2 OctEncode(glm::vec3 n) {
    const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (l1 > 0.0f) n /= l1;
    return (n.z >= 0.0f) ? glm::vec2(n.x, n.y) : OctWrap(glm::vec2(n.x, n.y));
}
glm::vec3 OctDecode(glm::vec2 e) {
    glm::vec3 n(e.x, e.y, 1.0f - std::fabs(e.x) - std::fabs(e.y));
    const float t = glm::max(-n.z, 0.0f);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    const float len = glm::length(n);
    return len > 0.0f ? n / len : glm::vec3(0, 1, 0);
}

i16 SnormToI16(float v) {
    v = glm::clamp(v, -1.0f, 1.0f);
    return static_cast<i16>(std::lround(v * 32767.0f));
}
float I16ToSnorm(i16 v) { return glm::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f); }

u16 QuantUnorm16(float v, float lo, float ext) {
    if (ext <= 0.0f) return 0;
    const float t = glm::clamp((v - lo) / ext, 0.0f, 1.0f);
    return static_cast<u16>(std::lround(t * 65535.0f));
}
float DequantUnorm16(u16 q, float lo, float ext) {
    return lo + (static_cast<float>(q) / 65535.0f) * ext;
}

bool AnySkinning(const std::vector<Vertex>& v) {
    for (const Vertex& x : v) {
        if (x.weights[0] != 0.0f || x.weights[1] != 0.0f || x.weights[2] != 0.0f ||
            x.weights[3] != 0.0f)
            return true;
        if (x.joints[0] || x.joints[1] || x.joints[2] || x.joints[3]) return true;
    }
    return false;
}

void FillStatic(QVertexStatic& q, const Vertex& v, const glm::vec3& pMin, const glm::vec3& pExt,
                const glm::vec2& uvMin, const glm::vec2& uvExt) {
    q.px = QuantUnorm16(v.position.x, pMin.x, pExt.x);
    q.py = QuantUnorm16(v.position.y, pMin.y, pExt.y);
    q.pz = QuantUnorm16(v.position.z, pMin.z, pExt.z);
    const glm::vec2 n = OctEncode(v.normal);
    q.nx = SnormToI16(n.x);
    q.ny = SnormToI16(n.y);
    const glm::vec2 t = OctEncode(glm::vec3(v.tangent));
    q.tx = SnormToI16(t.x);
    q.ty = SnormToI16(t.y);
    q.handed = (v.tangent.w >= 0.0f) ? 1 : 0;
    q.pad = 0;
    q.u = QuantUnorm16(v.uv.x, uvMin.x, uvExt.x);
    q.v = QuantUnorm16(v.uv.y, uvMin.y, uvExt.y);
}

void ReadStatic(const QVertexStatic& q, Vertex& v, const glm::vec3& pMin, const glm::vec3& pExt,
                const glm::vec2& uvMin, const glm::vec2& uvExt) {
    v.position = {DequantUnorm16(q.px, pMin.x, pExt.x), DequantUnorm16(q.py, pMin.y, pExt.y),
                  DequantUnorm16(q.pz, pMin.z, pExt.z)};
    v.normal = OctDecode({I16ToSnorm(q.nx), I16ToSnorm(q.ny)});
    const glm::vec3 t = OctDecode({I16ToSnorm(q.tx), I16ToSnorm(q.ty)});
    v.tangent = glm::vec4(t, q.handed ? 1.0f : -1.0f);
    v.uv = {DequantUnorm16(q.u, uvMin.x, uvExt.x), DequantUnorm16(q.v, uvMin.y, uvExt.y)};
    v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
    v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
}

// A meshopt vertex buffer must have stride % 4 == 0; pin the codec versions so shipped
// bytes never shift under a meshoptimizer upgrade.
std::vector<u8> EncodeVertexStream(const void* data, usize count, usize stride) {
    meshopt_encodeVertexVersion(0);
    std::vector<u8> out(meshopt_encodeVertexBufferBound(count, stride));
    const usize n = meshopt_encodeVertexBuffer(out.data(), out.size(), data, count, stride);
    out.resize(n);
    return out;
}

// The index SEQUENCE codec (not the triangle codec). It compresses slightly less than
// meshopt_encodeIndexBuffer but preserves the EXACT index order - the triangle codec may
// cyclically rotate the three indices within a triangle, which would silently change the
// provoking vertex for any flat-interpolated attribute. Indices are the small part of a
// mesh, so this trades a few bytes for exactness/robustness (the deliberate call).
std::vector<u8> EncodeIndexStream(const std::vector<u32>& indices, usize vertexCount) {
    meshopt_encodeIndexVersion(1);
    std::vector<u8> out(meshopt_encodeIndexSequenceBound(indices.size(), vertexCount));
    const usize n =
        meshopt_encodeIndexSequence(out.data(), out.size(), indices.data(), indices.size());
    out.resize(n);
    return out;
}

} // namespace

void WriteGeometry(BinaryWriter& w, const std::vector<Vertex>& vertices,
                   const std::vector<u32>& indices) {
    const u32 count = static_cast<u32>(vertices.size());
    const bool skinned = AnySkinning(vertices);
    u8 flags = skinned ? kFlagSkinned : 0;

    // Per-block position AABB and UV bounds (LODs move positions, so each block owns its
    // own bounds). Empty submesh -> zero bounds, still valid.
    glm::vec3 pMin(0.0f), pMax(0.0f);
    glm::vec2 uvMin(0.0f), uvMax(0.0f);
    if (count > 0) {
        pMin = pMax = vertices[0].position;
        uvMin = uvMax = vertices[0].uv;
        for (const Vertex& v : vertices) {
            pMin = glm::min(pMin, v.position);
            pMax = glm::max(pMax, v.position);
            uvMin = glm::min(uvMin, v.uv);
            uvMax = glm::max(uvMax, v.uv);
        }
    }
    const glm::vec3 pExt = pMax - pMin;
    const glm::vec2 uvExt = uvMax - uvMin;

    w.Pod<u8>(flags);
    w.Pod<u32>(count);
    w.Pod(pMin);
    w.Pod(pMax);
    w.Pod(uvMin);
    w.Pod(uvMax);

    // Build the compact stream and meshopt-encode it (empty when count == 0).
    std::vector<u8> encVerts;
    if (count > 0) {
        if (skinned) {
            std::vector<QVertexSkinned> q(count);
            for (u32 i = 0; i < count; ++i) {
                const Vertex& v = vertices[i];
                FillStatic(q[i].base, v, pMin, pExt, uvMin, uvExt);
                q[i].j0 = v.joints[0];
                q[i].j1 = v.joints[1];
                q[i].j2 = v.joints[2];
                q[i].j3 = v.joints[3];
                const auto w8 = [](float x) {
                    return static_cast<u8>(std::lround(glm::clamp(x, 0.0f, 1.0f) * 255.0f));
                };
                q[i].w0 = w8(v.weights[0]);
                q[i].w1 = w8(v.weights[1]);
                q[i].w2 = w8(v.weights[2]);
                q[i].w3 = w8(v.weights[3]);
            }
            encVerts = EncodeVertexStream(q.data(), count, sizeof(QVertexSkinned));
        } else {
            std::vector<QVertexStatic> q(count);
            for (u32 i = 0; i < count; ++i)
                FillStatic(q[i], vertices[i], pMin, pExt, uvMin, uvExt);
            encVerts = EncodeVertexStream(q.data(), count, sizeof(QVertexStatic));
        }
    }
    w.Vec(encVerts);

    const u32 indexCount = static_cast<u32>(indices.size());
    w.Pod<u32>(indexCount);
    std::vector<u8> encIdx;
    if (indexCount > 0 && count > 0) encIdx = EncodeIndexStream(indices, count);
    w.Vec(encIdx);
}

bool ReadGeometry(BinaryReader& r, std::vector<Vertex>& vertices, std::vector<u32>& indices) {
    u8 flags = 0;
    u32 count = 0;
    glm::vec3 pMin(0.0f), pMax(0.0f);
    glm::vec2 uvMin(0.0f), uvMax(0.0f);
    r.Pod(flags);
    r.Pod(count);
    r.Pod(pMin);
    r.Pod(pMax);
    r.Pod(uvMin);
    r.Pod(uvMax);
    if (!r.Ok()) return false;
    const glm::vec3 pExt = pMax - pMin;
    const glm::vec2 uvExt = uvMax - uvMin;
    const bool skinned = (flags & kFlagSkinned) != 0;

    std::vector<u8> encVerts;
    r.Vec(encVerts);
    if (!r.Ok()) return false;

    vertices.assign(count, Vertex{});
    if (count > 0) {
        if (skinned) {
            std::vector<QVertexSkinned> q(count);
            if (meshopt_decodeVertexBuffer(q.data(), count, sizeof(QVertexSkinned),
                                           encVerts.data(), encVerts.size()) != 0)
                return false;
            for (u32 i = 0; i < count; ++i) {
                ReadStatic(q[i].base, vertices[i], pMin, pExt, uvMin, uvExt);
                Vertex& v = vertices[i];
                v.joints[0] = q[i].j0;
                v.joints[1] = q[i].j1;
                v.joints[2] = q[i].j2;
                v.joints[3] = q[i].j3;
                float wf[4] = {q[i].w0 / 255.0f, q[i].w1 / 255.0f, q[i].w2 / 255.0f,
                               q[i].w3 / 255.0f};
                const float sum = wf[0] + wf[1] + wf[2] + wf[3];
                if (sum > 0.0f)
                    for (float& f : wf) f /= sum; // restore sum-to-one
                v.weights[0] = wf[0];
                v.weights[1] = wf[1];
                v.weights[2] = wf[2];
                v.weights[3] = wf[3];
            }
        } else {
            std::vector<QVertexStatic> q(count);
            if (meshopt_decodeVertexBuffer(q.data(), count, sizeof(QVertexStatic),
                                           encVerts.data(), encVerts.size()) != 0)
                return false;
            for (u32 i = 0; i < count; ++i)
                ReadStatic(q[i], vertices[i], pMin, pExt, uvMin, uvExt);
        }
    }

    u32 indexCount = 0;
    r.Pod(indexCount);
    std::vector<u8> encIdx;
    r.Vec(encIdx);
    if (!r.Ok()) return false;
    indices.assign(indexCount, 0);
    if (indexCount > 0) {
        if (meshopt_decodeIndexSequence(indices.data(), indexCount, sizeof(u32), encIdx.data(),
                                        encIdx.size()) != 0)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Self-test (--test-meshcodec)
// ---------------------------------------------------------------------------
namespace {

std::vector<Vertex> MakeGrid(int n, bool skinned) {
    std::vector<Vertex> v;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            Vertex vx;
            vx.position = {static_cast<f32>(x) * 0.5f - 1.0f, std::sin(x * 0.3f) * 2.0f,
                           static_cast<f32>(z) * 0.5f};
            vx.normal = glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f));
            vx.tangent = glm::vec4(glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f)),
                                   ((x + z) & 1) ? 1.0f : -1.0f); // both handedness signs
            vx.uv = {static_cast<f32>(x) / n * 3.0f, static_cast<f32>(z) / n}; // tiling u
            if (skinned) {
                vx.joints[0] = static_cast<u16>(x % 40);
                vx.joints[1] = static_cast<u16>((x + 1) % 40);
                vx.weights[0] = 0.7f;
                vx.weights[1] = 0.3f;
            }
            v.push_back(vx);
        }
    return v;
}

std::vector<u32> GridIndices(int n) {
    std::vector<u32> idx;
    for (int z = 0; z < n - 1; ++z)
        for (int x = 0; x < n - 1; ++x) {
            const u32 i0 = z * n + x, i1 = i0 + 1, i2 = i0 + n, i3 = i2 + 1;
            idx.insert(idx.end(), {i0, i2, i1, i1, i2, i3});
        }
    return idx;
}

} // namespace

bool SelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { HBE_ERROR("meshcodec: FAIL - {}", what); ++fails; }
    };

    for (bool skinned : {false, true}) {
        const int n = 24;
        std::vector<Vertex> verts = MakeGrid(n, skinned);
        std::vector<u32> idx = GridIndices(n);

        BinaryWriter w;
        WriteGeometry(w, verts, idx);
        BinaryWriter w2;
        WriteGeometry(w2, verts, idx);
        check(w.Data() == w2.Data(), "encode must be deterministic");

        BinaryReader r(w.Data());
        std::vector<Vertex> outV;
        std::vector<u32> outI;
        check(ReadGeometry(r, outV, outI), "decode must succeed");
        check(outV.size() == verts.size(), "vertex count preserved");
        check(outI == idx, "indices EXACT");

        // Position within quant epsilon (relative to the AABB extent), normals close,
        // tangent.w handedness EXACT, static skinning exactly zero, skinned weights sum~1.
        glm::vec3 mn = verts[0].position, mx = verts[0].position;
        for (const Vertex& v : verts) { mn = glm::min(mn, v.position); mx = glm::max(mx, v.position); }
        const glm::vec3 ext = mx - mn;
        const float posEps = glm::max(glm::max(ext.x, ext.y), ext.z) / 60000.0f + 1e-4f;
        for (usize i = 0; i < verts.size() && i < outV.size(); ++i) {
            if (glm::length(outV[i].position - verts[i].position) > posEps)
                check(false, "position exceeded quant epsilon");
            if (glm::dot(glm::normalize(outV[i].normal), glm::normalize(verts[i].normal)) < 0.999f)
                check(false, "normal drifted too far");
            if ((outV[i].tangent.w >= 0.0f) != (verts[i].tangent.w >= 0.0f))
                check(false, "tangent handedness sign changed");
            if (!skinned) {
                const bool zero = outV[i].joints[0] == 0 && outV[i].weights[0] == 0.0f;
                if (!zero) check(false, "static vertex must decode to zero skinning");
            } else {
                const float s = outV[i].weights[0] + outV[i].weights[1] + outV[i].weights[2] +
                                outV[i].weights[3];
                if (std::fabs(s - 1.0f) > 1e-3f) check(false, "skin weights must sum to ~1");
                if (outV[i].joints[0] != verts[i].joints[0])
                    check(false, "joint indices must be EXACT");
            }
        }
    }

    // Empty geometry round-trips cleanly.
    {
        BinaryWriter w;
        WriteGeometry(w, {}, {});
        BinaryReader r(w.Data());
        std::vector<Vertex> v;
        std::vector<u32> i;
        check(ReadGeometry(r, v, i) && v.empty() && i.empty(), "empty geometry round-trips");
    }

    // Size measurement: the raw pre-v10 form (72 B/vertex + 4 B/index) vs the v10 block,
    // for the SAME geometry (apples-to-apples, no weld/LOD confounds).
    {
        const int n = 48;
        const std::vector<Vertex> verts = MakeGrid(n, /*skinned*/ false);
        const std::vector<u32> idx = GridIndices(n);
        const usize rawBytes = verts.size() * sizeof(Vertex) + idx.size() * sizeof(u32);
        BinaryWriter w;
        WriteGeometry(w, verts, idx);
        const usize v10Bytes = w.Data().size();
        HBE_INFO("meshcodec: {}v/{}i static mesh: raw(72B/vtx) {} B -> v10 {} B ({}%, {:.1f}x "
                 "smaller)",
                 verts.size(), idx.size(), rawBytes, v10Bytes,
                 rawBytes ? v10Bytes * 100 / rawBytes : 100,
                 v10Bytes ? static_cast<double>(rawBytes) / static_cast<double>(v10Bytes) : 0.0);
    }

    if (fails == 0)
        HBE_INFO("meshcodec: passed (quantize+meshopt encode/decode, static+skinned, "
                 "deterministic, handedness + joints exact).");
    return fails == 0;
}

} // namespace hbe::meshcodec

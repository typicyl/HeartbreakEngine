// Scene/TerrainSystem.cpp
#include "Scene/TerrainSystem.h"
#include "Assets/Mesh.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Renderer/Renderer.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hbe {
namespace terrain {
namespace {

f32 Hash(i32 x, i32 z, i32 seed) {
    u32 h = static_cast<u32>(x * 374761393 + z * 668265263 + seed * 362437);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0x00FFFFFFu) / static_cast<f32>(0x00FFFFFF);
}

f32 ValueNoise(f32 x, f32 z, i32 seed) {
    const f32 fx = std::floor(x), fz = std::floor(z);
    const i32 x0 = static_cast<i32>(fx), z0 = static_cast<i32>(fz);
    f32 tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const f32 n00 = Hash(x0, z0, seed), n10 = Hash(x0 + 1, z0, seed);
    const f32 n01 = Hash(x0, z0 + 1, seed), n11 = Hash(x0 + 1, z0 + 1, seed);
    return glm::mix(glm::mix(n00, n10, tx), glm::mix(n01, n11, tx), tz);
}

f32 ProceduralHeight(const TerrainComponent& t, f32 x, f32 z) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    const u32 octaves = glm::max(t.octaves, 1u);
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(x * t.frequency * freq, z * t.frequency * freq,
                                t.seed + static_cast<i32>(o) * 17);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return ((norm > 0.0f ? sum / norm : 0.0f) * 2.0f - 1.0f) * t.height;
}

// Local-XZ extent helpers (the terrain is centered on the entity origin).
f32 Step(const TerrainComponent& t) { return t.chunkSize / static_cast<f32>(t.resolution); }
f32 Total(const TerrainComponent& t) { return static_cast<f32>(t.chunks) * t.chunkSize; }

void ClampParams(TerrainComponent& t) {
    t.chunks = glm::clamp(t.chunks, 1u, 32u);
    t.resolution = glm::clamp(t.resolution, 2u, 128u);
    t.chunkSize = glm::max(t.chunkSize, 1.0f);
}

f32 HeightAt(const TerrainComponent& t, i32 gx, i32 gz) {
    const i32 g = static_cast<i32>(t.GridN());
    gx = glm::clamp(gx, 0, g - 1);
    gz = glm::clamp(gz, 0, g - 1);
    return t.heights[static_cast<usize>(gz) * g + gx];
}

MeshData BuildChunk(const TerrainComponent& t, u32 cx, u32 cz) {
    const u32 res = t.resolution;
    const i32 gridN = static_cast<i32>(t.GridN());
    const f32 step = Step(t);
    const f32 total = Total(t);
    const u32 gx0 = cx * res, gz0 = cz * res;

    MeshData m;
    m.name = "TerrainChunk";
    m.vertices.reserve(static_cast<usize>(res + 1) * (res + 1));
    for (u32 j = 0; j <= res; ++j) {
        for (u32 i = 0; i <= res; ++i) {
            const i32 gx = static_cast<i32>(gx0 + i), gz = static_cast<i32>(gz0 + j);
            const f32 wx = -total * 0.5f + gx * step;
            const f32 wz = -total * 0.5f + gz * step;
            const f32 h = HeightAt(t, gx, gz);
            // Normal from neighbouring grid heights (seamless across chunks).
            const f32 hl = HeightAt(t, gx - 1, gz), hr = HeightAt(t, gx + 1, gz);
            const f32 hd = HeightAt(t, gx, gz - 1), hu = HeightAt(t, gx, gz + 1);
            Vertex v;
            v.position = {wx, h, wz};
            v.normal = glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            // Terrain-wide UV (whole terrain -> [0,1]^2) so ONE paint canvas on the
            // terrain entity maps seamlessly across every chunk (material painting).
            v.uv = {(wx + total * 0.5f) / total, (wz + total * 0.5f) / total};
            m.vertices.push_back(v);
        }
    }
    const u32 stride = res + 1;
    for (u32 j = 0; j < res; ++j) {
        for (u32 i = 0; i < res; ++i) {
            const u32 i0 = j * stride + i, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return m;
}

} // namespace

void EnsureHeights(TerrainComponent& t) {
    ClampParams(t);
    const usize need = static_cast<usize>(t.GridN()) * t.GridN();
    if (t.heights.size() == need) return;
    // (Re)seed procedurally: sample the noise at each grid vertex's local XZ.
    t.heights.assign(need, 0.0f);
    const i32 gridN = static_cast<i32>(t.GridN());
    const f32 step = Step(t), total = Total(t);
    for (i32 gz = 0; gz < gridN; ++gz) {
        for (i32 gx = 0; gx < gridN; ++gx) {
            const f32 wx = -total * 0.5f + gx * step;
            const f32 wz = -total * 0.5f + gz * step;
            t.heights[static_cast<usize>(gz) * gridN + gx] = ProceduralHeight(t, wx, wz);
        }
    }
}

f32 SampleHeight(const TerrainComponent& t, f32 x, f32 z) {
    if (t.heights.size() != static_cast<usize>(t.GridN()) * t.GridN()) {
        return ProceduralHeight(t, x, z);
    }
    const f32 step = Step(t), total = Total(t);
    const f32 fx = (x + total * 0.5f) / step;
    const f32 fz = (z + total * 0.5f) / step;
    const i32 x0 = static_cast<i32>(std::floor(fx)), z0 = static_cast<i32>(std::floor(fz));
    const f32 tx = fx - x0, tz = fz - z0;
    const f32 h00 = HeightAt(t, x0, z0), h10 = HeightAt(t, x0 + 1, z0);
    const f32 h01 = HeightAt(t, x0, z0 + 1), h11 = HeightAt(t, x0 + 1, z0 + 1);
    return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
}

void Update(Scene& scene, Renderer& renderer) {
    if (!renderer.SupportsScene()) return;
    auto& reg = scene.Registry();

    // Hole-mask GPU uploads (independent of chunk rebuilds): expand the 1-byte mask
    // to RGBA8 (the bindless array is RGBA) and (re)upload. The forward pass samples
    // .r and clips where it's set; chunks reference this via the parent terrain.
    for (const entt::entity e : reg.view<TerrainComponent>()) {
        TerrainComponent& t = reg.get<TerrainComponent>(e);
        if (!t.holeDirty || t.holeMask.empty()) continue;
        const u32 n = t.GridN();
        std::vector<u8> rgba(static_cast<usize>(n) * n * 4, 0);
        const usize count = std::min<usize>(t.holeMask.size(), static_cast<usize>(n) * n);
        for (usize i = 0; i < count; ++i) {
            const u8 m = t.holeMask[i];
            rgba[i * 4 + 0] = m; rgba[i * 4 + 1] = m; rgba[i * 4 + 2] = m; rgba[i * 4 + 3] = m;
        }
        rhi::TextureDesc desc;
        desc.width = n;
        desc.height = n;
        desc.format = rhi::Format::R8G8B8A8_UNORM;
        desc.mipCount = 1; // no mips: keep the hole edge crisp (binary clip)
        desc.pixels = rgba.data();
        desc.debugName = "TerrainHoleMask";
        if (t.holeMaskTex.IsValid()) renderer.UpdateTexture(t.holeMaskTex, desc);
        else t.holeMaskTex = renderer.UploadTexture(desc);
        t.holeDirty = false;
    }

    // Splat weight-mask uploads (already RGBA8, one channel per layer).
    for (const entt::entity e : reg.view<TerrainComponent>()) {
        TerrainComponent& t = reg.get<TerrainComponent>(e);
        if (!t.splatDirty || t.splatWeight.empty()) continue;
        const u32 n = t.GridN();
        if (t.splatWeight.size() != static_cast<usize>(n) * n * 4) { t.splatDirty = false; continue; }
        rhi::TextureDesc desc;
        desc.width = n;
        desc.height = n;
        desc.format = rhi::Format::R8G8B8A8_UNORM;
        desc.mipCount = 1;
        desc.pixels = t.splatWeight.data();
        desc.debugName = "TerrainSplatWeight";
        if (t.splatWeightTex.IsValid()) renderer.UpdateTexture(t.splatWeightTex, desc);
        else t.splatWeightTex = renderer.UploadTexture(desc);
        t.splatDirty = false;
    }

    std::vector<entt::entity> toBuild;
    for (const entt::entity e : reg.view<TerrainComponent>()) {
        if (reg.get<TerrainComponent>(e).dirty) toBuild.push_back(e);
    }
    if (toBuild.empty()) return;

    for (const entt::entity e : toBuild) {
        EnsureHeights(reg.get<TerrainComponent>(e));
        const TerrainComponent t = reg.get<TerrainComponent>(e); // copy for building

        // Destroy this terrain's existing chunk children.
        std::vector<entt::entity> kill;
        for (const entt::entity c : reg.view<TerrainChunk, Parent>()) {
            if (reg.get<Parent>(c).entity == e) kill.push_back(c);
        }
        for (const entt::entity c : kill) reg.destroy(c);

        for (u32 cz = 0; cz < t.chunks; ++cz) {
            for (u32 cx = 0; cx < t.chunks; ++cx) {
                MeshData md = BuildChunk(t, cx, cz);
                if (md.vertices.empty()) continue;
                const entt::entity ce = scene.CreateEntity(
                    "Chunk_" + std::to_string(cx) + "_" + std::to_string(cz));
                reg.emplace<Transform>(ce, Transform{});
                MeshInstance mi;
                mi.mesh = renderer.UploadMesh(md);
                mi.baseColor = t.color;
                mi.metallic = 0.0f;
                mi.roughness = t.roughness;
                reg.emplace<MeshInstance>(ce, mi);
                glm::vec3 bmin, bmax;
                ComputeBounds(md, bmin, bmax);
                reg.emplace<AABB>(ce, AABB{bmin, bmax});
                reg.emplace<Parent>(ce, Parent{e});
                reg.emplace<TerrainChunk>(ce, TerrainChunk{cx, cz});

                // Static triangle-mesh collider so characters/physics walk on the
                // terrain. Geometry is the chunk mesh (local space, identity chunk
                // transform -> the parent terrain's world places it); rebuilt with the
                // chunk each time the heightmap is sculpted, so collision tracks shape.
                RigidBody rb;
                rb.shape = RigidBody::Shape::Mesh;
                rb.motion = RigidBody::Motion::Static;
                rb.collisionVertices.reserve(md.vertices.size());
                for (const Vertex& v : md.vertices) rb.collisionVertices.push_back(v.position);
                rb.collisionIndices = md.indices;
                reg.emplace<RigidBody>(ce, rb);
            }
        }
        reg.get<TerrainComponent>(e).dirty = false;
    }
}

void Sculpt(Scene& scene, Renderer& renderer, entt::entity terrain, f32 localX,
            f32 localZ, f32 radius, f32 amount, Brush brush, f32 flattenTarget) {
    auto& reg = scene.Registry();
    TerrainComponent* tp = reg.try_get<TerrainComponent>(terrain);
    if (!tp || tp->dirty) return; // wait until chunks exist
    TerrainComponent& t = *tp;
    EnsureHeights(t);

    const i32 gridN = static_cast<i32>(t.GridN());
    const f32 step = Step(t), total = Total(t);
    const f32 inv = step > 0.0f ? 1.0f / step : 0.0f;
    // Grid cell range covered by the brush.
    const f32 cx = (localX + total * 0.5f) * inv;
    const f32 cz = (localZ + total * 0.5f) * inv;
    const i32 r = static_cast<i32>(std::ceil(radius * inv)) + 1;
    const i32 i0 = glm::clamp(static_cast<i32>(cx) - r, 0, gridN - 1);
    const i32 i1 = glm::clamp(static_cast<i32>(cx) + r, 0, gridN - 1);
    const i32 j0 = glm::clamp(static_cast<i32>(cz) - r, 0, gridN - 1);
    const i32 j1 = glm::clamp(static_cast<i32>(cz) + r, 0, gridN - 1);
    const f32 r2 = radius * radius;

    for (i32 gz = j0; gz <= j1; ++gz) {
        for (i32 gx = i0; gx <= i1; ++gx) {
            const f32 wx = -total * 0.5f + gx * step;
            const f32 wz = -total * 0.5f + gz * step;
            const f32 d2 = (wx - localX) * (wx - localX) + (wz - localZ) * (wz - localZ);
            if (d2 > r2) continue;
            const f32 fall = 1.0f - std::sqrt(d2 / r2); // linear-ish falloff
            const f32 w = fall * fall * (3.0f - 2.0f * fall); // smoothstep
            f32& h = t.heights[static_cast<usize>(gz) * gridN + gx];
            switch (brush) {
                case Brush::Raise: h += amount * w; break;
                case Brush::Lower: h -= amount * w; break;
                case Brush::Flatten:
                    h = glm::mix(h, flattenTarget, glm::clamp(amount, 0.0f, 1.0f) * w);
                    break;
                case Brush::Smooth: {
                    const f32 avg = 0.25f * (HeightAt(t, gx - 1, gz) + HeightAt(t, gx + 1, gz) +
                                             HeightAt(t, gx, gz - 1) + HeightAt(t, gx, gz + 1));
                    h = glm::mix(h, avg, glm::clamp(amount, 0.0f, 1.0f) * w);
                    break;
                }
            }
        }
    }

    // Update the chunk meshes overlapping the brush (in place, no realloc).
    const f32 bxMin = localX - radius, bxMax = localX + radius;
    const f32 bzMin = localZ - radius, bzMax = localZ + radius;
    for (const entt::entity ce : reg.view<TerrainChunk, MeshInstance, Parent>()) {
        if (reg.get<Parent>(ce).entity != terrain) continue;
        const TerrainChunk& tc = reg.get<TerrainChunk>(ce);
        const f32 x0 = -total * 0.5f + static_cast<f32>(tc.cx) * t.chunkSize;
        const f32 z0 = -total * 0.5f + static_cast<f32>(tc.cz) * t.chunkSize;
        // Expand by one step so shared edge normals refresh on both sides.
        if (x0 + t.chunkSize + step < bxMin || x0 - step > bxMax) continue;
        if (z0 + t.chunkSize + step < bzMin || z0 - step > bzMax) continue;
        MeshData md = BuildChunk(t, tc.cx, tc.cz);
        MeshInstance& mi = reg.get<MeshInstance>(ce);
        renderer.UpdateMesh(mi.mesh, md);
        glm::vec3 bmin, bmax;
        ComputeBounds(md, bmin, bmax);
        reg.emplace_or_replace<AABB>(ce, AABB{bmin, bmax});
    }
}

void PaintHole(TerrainComponent& t, f32 localX, f32 localZ, f32 radius, bool erase) {
    EnsureHeights(t);
    const i32 gridN = static_cast<i32>(t.GridN());
    const usize need = static_cast<usize>(gridN) * gridN;
    if (t.holeMask.size() != need) t.holeMask.assign(need, 0); // 0 = solid

    const f32 step = Step(t), total = Total(t);
    const f32 inv = step > 0.0f ? 1.0f / step : 0.0f;
    const f32 cx = (localX + total * 0.5f) * inv;
    const f32 cz = (localZ + total * 0.5f) * inv;
    const i32 r = static_cast<i32>(std::ceil(radius * inv)) + 1;
    const i32 i0 = glm::clamp(static_cast<i32>(cx) - r, 0, gridN - 1);
    const i32 i1 = glm::clamp(static_cast<i32>(cx) + r, 0, gridN - 1);
    const i32 j0 = glm::clamp(static_cast<i32>(cz) - r, 0, gridN - 1);
    const i32 j1 = glm::clamp(static_cast<i32>(cz) + r, 0, gridN - 1);
    const f32 r2 = radius * radius;
    const u8 value = erase ? 0 : 255; // 255 = hole (clipped), 0 = solid

    for (i32 gz = j0; gz <= j1; ++gz) {
        for (i32 gx = i0; gx <= i1; ++gx) {
            const f32 wx = -total * 0.5f + gx * step;
            const f32 wz = -total * 0.5f + gz * step;
            const f32 d2 = (wx - localX) * (wx - localX) + (wz - localZ) * (wz - localZ);
            if (d2 > r2) continue;
            t.holeMask[static_cast<usize>(gz) * gridN + gx] = value; // hard stamp
        }
    }
    t.holeDirty = true;
}

void PaintSplat(TerrainComponent& t, f32 localX, f32 localZ, f32 radius, i32 layer) {
    EnsureHeights(t);
    layer = glm::clamp(layer, 0, 3);
    const i32 gridN = static_cast<i32>(t.GridN());
    const usize need = static_cast<usize>(gridN) * gridN * 4; // RGBA
    if (t.splatWeight.size() != need) {
        t.splatWeight.assign(need, 0);
        for (usize i = 0; i < need; i += 4) t.splatWeight[i] = 255; // default = layer 0
    }
    const f32 step = Step(t), total = Total(t);
    const f32 inv = step > 0.0f ? 1.0f / step : 0.0f;
    const f32 cx = (localX + total * 0.5f) * inv;
    const f32 cz = (localZ + total * 0.5f) * inv;
    const i32 r = static_cast<i32>(std::ceil(radius * inv)) + 1;
    const i32 i0 = glm::clamp(static_cast<i32>(cx) - r, 0, gridN - 1);
    const i32 i1 = glm::clamp(static_cast<i32>(cx) + r, 0, gridN - 1);
    const i32 j0 = glm::clamp(static_cast<i32>(cz) - r, 0, gridN - 1);
    const i32 j1 = glm::clamp(static_cast<i32>(cz) + r, 0, gridN - 1);
    const f32 r2 = radius * radius;

    for (i32 gz = j0; gz <= j1; ++gz) {
        for (i32 gx = i0; gx <= i1; ++gx) {
            const f32 wx = -total * 0.5f + gx * step;
            const f32 wz = -total * 0.5f + gz * step;
            const f32 d2 = (wx - localX) * (wx - localX) + (wz - localZ) * (wz - localZ);
            if (d2 > r2) continue;
            const f32 fall = 1.0f - std::sqrt(d2 / r2);
            const f32 w = fall * fall * (3.0f - 2.0f * fall); // smoothstep falloff
            u8* px = &t.splatWeight[(static_cast<usize>(gz) * gridN + gx) * 4];
            for (i32 c = 0; c < 4; ++c) {
                if (c == layer)
                    px[c] = static_cast<u8>(glm::max<f32>(px[c], w * 255.0f)); // grow active
                else
                    px[c] = static_cast<u8>(px[c] * (1.0f - w));               // fade others
            }
        }
    }
    t.splatDirty = true;
}

bool RaycastLocal(const TerrainComponent& t, const glm::vec3& localOrigin,
                  const glm::vec3& localDir, glm::vec3& outHit) {
    const glm::vec3 dir = glm::normalize(localDir);
    const f32 half = Total(t) * 0.5f;
    const f32 step = glm::max(Step(t) * 0.5f, 0.05f);
    f32 prevDiff = localOrigin.y - SampleHeight(t, localOrigin.x, localOrigin.z);
    const f32 maxDist = Total(t) * 3.0f + 200.0f;
    for (f32 s = step; s < maxDist; s += step) {
        const glm::vec3 p = localOrigin + dir * s;
        if (p.x < -half - 1.0f || p.x > half + 1.0f || p.z < -half - 1.0f ||
            p.z > half + 1.0f) {
            if (prevDiff < 0.0f) break; // left the terrain footprint while underground
            continue;
        }
        const f32 diff = p.y - SampleHeight(t, p.x, p.z);
        if (diff <= 0.0f && prevDiff > 0.0f) {
            // Crossed the surface between the last sample and this one: refine.
            const f32 f = prevDiff / glm::max(prevDiff - diff, 1e-4f);
            outHit = localOrigin + dir * (s - step + step * f);
            outHit.y = SampleHeight(t, outHit.x, outHit.z);
            return true;
        }
        prevDiff = diff;
    }
    return false;
}

bool IsSettled(Scene& scene) {
    auto view = scene.Registry().view<TerrainComponent>();
    for (const entt::entity e : view) {
        const TerrainComponent& t = view.get<TerrainComponent>(e);
        if (t.dirty || t.holeDirty || t.splatDirty) return false;
    }
    return true;
}

} // namespace terrain
} // namespace hbe

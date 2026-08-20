// Material/BoxBrush.cpp - see BoxBrush.h.
#include "Material/BoxBrush.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace hbe::mat {

using json = nlohmann::json;

namespace {
// Rotation may be authored/deserialized non-unit; normalize before use so ToLocal (which inverts
// the quaternion) and Bounds (which rotates corners) describe the SAME box. Guards a zero quat.
glm::quat SafeRot(const glm::quat& r) {
    const f32 n2 = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
    return n2 > 1e-12f ? r * (1.0f / std::sqrt(n2)) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}
} // namespace

glm::vec3 BoxBrush::ToLocal(const glm::vec3& worldPos) const {
    // Undo T, then R, then S: local = (R^-1 (world - T)) / scale.
    const glm::vec3 rel = worldPos - position;
    const glm::vec3 unrot = glm::inverse(SafeRot(rotation)) * rel;
    auto axis = [](f32 u, f32 s) -> f32 {
        if (s != 0.0f) return u / s;
        // A zero-scale (flat/degenerate) axis has ZERO extent: on the plane (u==0) the point is
        // inside, off it far outside - so EvaluateBrush agrees with the zero-extent Bounds() rather
        // than treating the axis as a phantom full-size slab.
        return (u == 0.0f) ? 0.0f : (u > 0.0f ? 1e30f : -1e30f);
    };
    return glm::vec3(axis(unrot.x, scale.x), axis(unrot.y, scale.y), axis(unrot.z, scale.z));
}

f32 BoxBrush::EvaluateBrush(const glm::vec3& worldPos) const {
    const glm::vec3 local = ToLocal(worldPos);
    const glm::vec3 half = 0.5f * size;
    // Per-axis inward fraction: 1 in the core, ramping to 0 at each face over the fade band.
    f32 w = 1.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 d = std::abs(local[a]);
        if (d > half[a]) return 0.0f; // outside the box
        const f32 band = std::max(half[a] * std::clamp(falloffWidth, 0.0f, 1.0f), 1e-6f);
        const f32 t = std::clamp((half[a] - d) / band, 0.0f, 1.0f); // 0 at face, 1 at core
        w = std::min(w, t);
    }
    return std::clamp(falloff.Eval(w) * strength, 0.0f, 1.0f);
}

glm::vec2 BoxBrush::ProjectUV(const glm::vec3& worldPos, const glm::vec3& normal) const {
    // Choose the source position: world (size-independent) or box-local (rides the volume).
    glm::vec3 p = (projection == BoxProjection::Local) ? ToLocal(worldPos) : worldPos;

    // Dominant-axis (or triplanar-dominant) plane selection by |normal|.
    const glm::vec3 an = glm::abs(normal);
    glm::vec2 uv;
    glm::vec2 tile;
    if (an.y >= an.x && an.y >= an.z) { // facing up/down -> XZ plane (floors)
        uv = {p.x, p.z};
        tile = {tileMeters.x, tileMeters.z};
    } else if (an.x >= an.z) { // facing X -> ZY plane
        uv = {p.z, p.y};
        tile = {tileMeters.z, tileMeters.y};
    } else { // facing Z -> XY plane (walls)
        uv = {p.x, p.y};
        tile = {tileMeters.x, tileMeters.y};
    }
    // Metres -> tiles: divide position by tileMeters (NEVER by the brush size => size-independent).
    uv.x = tile.x != 0.0f ? uv.x / tile.x : uv.x;
    uv.y = tile.y != 0.0f ? uv.y / tile.y : uv.y;
    // In-plane rotation + offset.
    if (uvRotation != 0.0f) {
        const f32 c = std::cos(uvRotation), s = std::sin(uvRotation);
        uv = {uv.x * c - uv.y * s, uv.x * s + uv.y * c};
    }
    return uv + uvOffset;
}

Aabb BoxBrush::Bounds() const {
    const glm::vec3 half = 0.5f * size;
    const glm::quat q = SafeRot(rotation); // same normalized rotation ToLocal uses
    Aabb box;
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y,
                               (i & 4) ? half.z : -half.z);
        const glm::vec3 world = position + q * (corner * scale);
        if (first) {
            box.min = box.max = world;
            first = false;
        } else {
            box.min = glm::min(box.min, world);
            box.max = glm::max(box.max, world);
        }
    }
    return box;
}

u64 BoxBrush::Hash(u64 seed) const {
    u64 h = seed;
    auto v3 = [&](const glm::vec3& v) {
        h = HashF32(v.x, h);
        h = HashF32(v.y, h);
        h = HashF32(v.z, h);
    };
    v3(position);
    h = HashF32(rotation.x, h);
    h = HashF32(rotation.y, h);
    h = HashF32(rotation.z, h);
    h = HashF32(rotation.w, h);
    v3(scale);
    v3(size);
    v3(tileMeters);
    h = HashF32(falloffWidth, h);
    h = HashF32(strength, h);
    h = HashF32(uvRotation, h);
    h = HashF32(uvOffset.x, h);
    h = HashF32(uvOffset.y, h);
    const u8 ft = static_cast<u8>(falloff.type);
    const u8 pj = static_cast<u8>(projection);
    h = HashBytes(&ft, 1, h);
    h = HashF32(falloff.gamma, h);
    h = HashBytes(&pj, 1, h);
    h = HashStr(material, h);
    h = HashBytes(&blendMode, sizeof(blendMode), h);
    return h;
}

// ---- JSON -------------------------------------------------------------------------------
namespace {
json V2(const glm::vec2& v) { return json::array({v.x, v.y}); }
json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4q(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }
glm::vec2 G2(const json& j, glm::vec2 d) {
    if (!j.is_array() || j.size() < 2) return d;
    try { return {j[0].get<f32>(), j[1].get<f32>()}; } catch (...) { return d; }
}
glm::vec3 G3(const json& j, glm::vec3 d) {
    if (!j.is_array() || j.size() < 3) return d;
    try { return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()}; } catch (...) { return d; }
}
glm::quat Gq(const json& j, glm::quat d) {
    if (!j.is_array() || j.size() < 4) return d;
    try { return glm::quat(j[3].get<f32>(), j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()); }
    catch (...) { return d; }
}
template <class T> T JG(const json& j, const char* k, const T& d) {
    const auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    try { return it->get<T>(); } catch (...) { return d; }
}
} // namespace

json BoxBrushToJson(const BoxBrush& b) {
    json j;
    j["position"] = V3(b.position);
    j["rotation"] = V4q(b.rotation);
    j["scale"] = V3(b.scale);
    j["size"] = V3(b.size);
    j["falloffType"] = static_cast<int>(b.falloff.type);
    j["falloffGamma"] = b.falloff.gamma;
    j["falloffWidth"] = b.falloffWidth;
    j["strength"] = b.strength;
    j["projection"] = static_cast<int>(b.projection);
    j["tileMeters"] = V3(b.tileMeters);
    j["uvRotation"] = b.uvRotation;
    j["uvOffset"] = V2(b.uvOffset);
    if (!b.material.empty()) j["material"] = b.material;
    j["blendMode"] = b.blendMode;
    return j;
}
void BoxBrushFromJson(const json& j, BoxBrush& b) {
    b.position = G3(j.value("position", json()), b.position);
    b.rotation = Gq(j.value("rotation", json()), b.rotation);
    b.scale = G3(j.value("scale", json()), b.scale);
    b.size = G3(j.value("size", json()), b.size);
    b.falloff.type = static_cast<FalloffType>(JG(j, "falloffType", 2));
    b.falloff.gamma = JG(j, "falloffGamma", 1.0f);
    b.falloffWidth = JG(j, "falloffWidth", 0.25f);
    b.strength = JG(j, "strength", 1.0f);
    b.projection = static_cast<BoxProjection>(JG(j, "projection", 0));
    b.tileMeters = G3(j.value("tileMeters", json()), b.tileMeters);
    b.uvRotation = JG(j, "uvRotation", 0.0f);
    b.uvOffset = G2(j.value("uvOffset", json()), b.uvOffset);
    b.material = JG(j, "material", std::string());
    b.blendMode = JG(j, "blendMode", 0);
}

std::string BoxBrushToJsonString(const BoxBrush& b) { return BoxBrushToJson(b).dump(2); }
bool BoxBrushFromJsonString(const std::string& str, BoxBrush& out) {
    try {
        BoxBrushFromJson(json::parse(str), out);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace hbe::mat

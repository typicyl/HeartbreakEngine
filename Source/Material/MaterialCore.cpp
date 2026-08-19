// Material/MaterialCore.cpp - see MaterialCore.h.
#include "Material/MaterialCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hbe::mat {

// ---- MaskTexture ------------------------------------------------------------------------
f32 MaskTexture::Sample(glm::vec2 uv) const {
    if (!Valid()) return 0.0f;
    // Wrap into [0,1). fmod can return negative for negative uv, so add 1 and wrap again.
    auto wrap01 = [](f32 v) {
        v = std::fmod(v, 1.0f);
        if (v < 0.0f) v += 1.0f;
        return v;
    };
    const f32 fx = wrap01(uv.x) * static_cast<f32>(width);
    const f32 fy = wrap01(uv.y) * static_cast<f32>(height);
    const u32 x0 = static_cast<u32>(fx) % width;
    const u32 y0 = static_cast<u32>(fy) % height;
    const u32 x1 = (x0 + 1) % width;
    const u32 y1 = (y0 + 1) % height;
    const f32 tx = fx - std::floor(fx);
    const f32 ty = fy - std::floor(fy);
    const f32 a = At(x0, y0), b = At(x1, y0), c = At(x0, y1), d = At(x1, y1);
    const f32 top = a + (b - a) * tx;
    const f32 bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

// ---- Falloff ----------------------------------------------------------------------------
f32 Falloff::Eval(f32 t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    f32 w;
    switch (type) {
        case FalloffType::Constant:    w = (t > 0.0f) ? 1.0f : 0.0f; break;
        case FalloffType::Linear:      w = t; break;
        case FalloffType::Smoothstep:  w = t * t * (3.0f - 2.0f * t); break;
        case FalloffType::Smootherstep:w = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); break;
        case FalloffType::EaseIn:      w = t * t; break;
        case FalloffType::EaseOut:     w = 1.0f - (1.0f - t) * (1.0f - t); break;
        default:                       w = t; break;
    }
    if (gamma > 0.0f && gamma != 1.0f) w = std::pow(std::clamp(w, 0.0f, 1.0f), gamma);
    return std::clamp(w, 0.0f, 1.0f);
}

// ---- ParamSet ---------------------------------------------------------------------------
const Param* ParamSet::Find(const std::string& name) const {
    for (const auto& p : params)
        if (p.name == name) return &p;
    return nullptr;
}
Param* ParamSet::Find(const std::string& name) {
    for (auto& p : params)
        if (p.name == name) return &p;
    return nullptr;
}
f32 ParamSet::Scalar(const std::string& name, f32 def) const {
    const Param* p = Find(name);
    return p ? p->value.x : def;
}
glm::vec4 ParamSet::Vector(const std::string& name, glm::vec4 def) const {
    const Param* p = Find(name);
    return p ? p->value : def;
}
std::string ParamSet::Texture(const std::string& name) const {
    const Param* p = Find(name);
    return p ? p->texture : std::string();
}

void ApplyOverrides(ParamSet& set, const std::vector<ParamOverride>& overrides) {
    for (const auto& o : overrides) {
        Param* p = set.Find(o.name);
        if (!p) continue; // never invent a parameter from a stale override
        if (p->type == ParamType::Texture) {
            p->texture = o.texture;
        } else {
            p->value = o.value;
        }
    }
}

// ---- Hashing ----------------------------------------------------------------------------
u64 HashBytes(const void* data, usize size, u64 seed) {
    const auto* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
u64 HashF32(f32 v, u64 seed) {
    // Normalise -0.0f to +0.0f so the hash is value-stable, not bit-stable.
    if (v == 0.0f) v = 0.0f;
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return HashBytes(&bits, sizeof(bits), seed);
}
u64 HashStr(const std::string& s, u64 seed) { return HashBytes(s.data(), s.size(), seed); }

} // namespace hbe::mat

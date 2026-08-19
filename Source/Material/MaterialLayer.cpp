// Material/MaterialLayer.cpp - see MaterialLayer.h.
#include "Material/MaterialLayer.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace hbe::mat {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Local deterministic value noise (HeightNoise break-up). Same style as the graph compiler's,
// kept local so this module has no cross-TU dependency on it.
namespace {
u32 Hash2(i32 x, i32 y, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= static_cast<u32>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<u32>(y) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}
f32 H01(i32 x, i32 y, u32 s) { return static_cast<f32>(Hash2(x, y, s) & 0xFFFFFFu) / 16777215.0f; }
f32 ValueNoise2D(glm::vec2 p, u32 seed) {
    const i32 x0 = static_cast<i32>(std::floor(p.x)), y0 = static_cast<i32>(std::floor(p.y));
    const f32 fx = p.x - x0, fy = p.y - y0;
    const f32 ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const f32 a = H01(x0, y0, seed), b = H01(x0 + 1, y0, seed);
    const f32 c = H01(x0, y0 + 1, seed), d = H01(x0 + 1, y0 + 1, seed);
    const f32 top = a + (b - a) * ux, bot = c + (d - c) * ux;
    return top + (bot - top) * uy;
}
glm::vec2 SpaceUV(const SampleContext& ctx, Space s) {
    switch (s) {
        case Space::UV0: return ctx.uv0;
        case Space::UV1: return ctx.uv1;
        case Space::Object: return {ctx.objectPos.x, ctx.objectPos.z};
        case Space::World:
        case Space::Triplanar: return {ctx.worldPos.x, ctx.worldPos.z};
        default: return ctx.uv0;
    }
}
} // namespace

// ---- MaskSource -------------------------------------------------------------------------
f32 MaskSource::Evaluate(const SampleContext& ctx) const {
    f32 w;
    switch (kind) {
        case MaskKind::Constant: w = constant; break;
        case MaskKind::Box: w = box.EvaluateBrush(ctx.worldPos); break;
        case MaskKind::Procedural:
        case MaskKind::Paint: w = texture.Valid() ? texture.Sample(SpaceUV(ctx, textureSpace)) : constant; break;
        default: w = constant; break;
    }
    if (invert) w = 1.0f - w;
    return std::clamp(w, 0.0f, 1.0f);
}

u64 MaskSource::Hash(u64 seed) const {
    u64 h = seed;
    const u8 k = static_cast<u8>(kind);
    h = HashBytes(&k, 1, h);
    h = HashF32(constant, h);
    if (kind == MaskKind::Box) h = box.Hash(h);
    const u8 sp = static_cast<u8>(textureSpace);
    h = HashBytes(&sp, 1, h);
    h = HashBytes(&invert, 1, h);
    h = HashBytes(&paintCanvasId, sizeof(paintCanvasId), h);
    h = HashBytes(&paintLayerId, sizeof(paintLayerId), h);
    h = HashBytes(&paintChannel, 1, h);
    // Mask texel data participates in the hash (content identity of a baked mask).
    if (texture.Valid()) h = HashBytes(texture.data.data(), texture.data.size() * sizeof(f32), h);
    return h;
}

// ---- Normal & material blending ---------------------------------------------------------
glm::vec3 BlendNormalRNM(const glm::vec3& base, const glm::vec3& detail, f32 strength) {
    // Fade the detail toward flat (0,0,1) by (1-strength).
    const glm::vec3 n2 = glm::mix(glm::vec3(0, 0, 1), detail, std::clamp(strength, 0.0f, 1.0f));
    const glm::vec3 t = base + glm::vec3(0, 0, 1);
    const glm::vec3 u = n2 * glm::vec3(-1, -1, 1);
    const glm::vec3 r = t * glm::dot(t, u) - u * t.z;
    const f32 len = glm::length(r);
    return len > 1e-6f ? r / len : base;
}

SurfaceParams LerpSurface(const SurfaceParams& a, const SurfaceParams& b, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    SurfaceParams r = a; // fields not blended keep `a`'s value (typically identical presets)
    r.base_color = glm::mix(a.base_color, b.base_color, t);
    r.base_weight = glm::mix(a.base_weight, b.base_weight, t);
    r.base_metalness = glm::mix(a.base_metalness, b.base_metalness, t);
    r.base_diffuse_roughness = glm::mix(a.base_diffuse_roughness, b.base_diffuse_roughness, t);
    r.specular_weight = glm::mix(a.specular_weight, b.specular_weight, t);
    r.specular_color = glm::mix(a.specular_color, b.specular_color, t);
    r.specular_roughness = glm::mix(a.specular_roughness, b.specular_roughness, t);
    r.specular_ior = glm::mix(a.specular_ior, b.specular_ior, t);
    r.emission_color = glm::mix(a.emission_color, b.emission_color, t);
    r.emission_luminance = glm::mix(a.emission_luminance, b.emission_luminance, t);
    r.coat_weight = glm::mix(a.coat_weight, b.coat_weight, t);
    r.coat_color = glm::mix(a.coat_color, b.coat_color, t);
    r.coat_roughness = glm::mix(a.coat_roughness, b.coat_roughness, t);
    r.subsurface_weight = glm::mix(a.subsurface_weight, b.subsurface_weight, t);
    r.subsurface_color = glm::mix(a.subsurface_color, b.subsurface_color, t);
    r.transmission_weight = glm::mix(a.transmission_weight, b.transmission_weight, t);
    r.transmission_color = glm::mix(a.transmission_color, b.transmission_color, t);
    r.fuzz_weight = glm::mix(a.fuzz_weight, b.fuzz_weight, t);
    r.fuzz_color = glm::mix(a.fuzz_color, b.fuzz_color, t);
    r.fuzz_roughness = glm::mix(a.fuzz_roughness, b.fuzz_roughness, t);
    return r;
}

// ---- Resolver ---------------------------------------------------------------------------
ResolvedSurface Resolve(const LayerStack& stack, const SampleContext& ctx) {
    ResolvedSurface acc;
    acc.surface = stack.base;
    acc.normalTS = stack.baseNormalTS;
    acc.height = stack.baseHeight;

    for (const Layer& layer : stack.layers) {
        const f32 w = std::clamp(layer.mask.Evaluate(ctx) * std::clamp(layer.opacity, 0.0f, 1.0f),
                                 0.0f, 1.0f);
        if (w <= 0.0f) continue;

        f32 wEff = w;
        if (layer.blend != BlendMode::Linear) {
            // Height priority: a taller layer wins more in the transition band. The bias term
            // w*(1-w)*4 peaks at w=0.5 and VANISHES at w=0 and w=1, so the endpoints stay exact.
            f32 heightDiff = (layer.layerHeight - acc.height) * layer.heightContribution;
            if (layer.blend == BlendMode::HeightNoise) {
                const f32 n = ValueNoise2D(ctx.uv0 * std::max(layer.noiseScale, 0.001f), 1u);
                heightDiff += (n - 0.5f) * 2.0f * layer.noiseAmount;
            }
            wEff = std::clamp(w + heightDiff * (w * (1.0f - w) * 4.0f), 0.0f, 1.0f);
        }

        acc.surface = LerpSurface(acc.surface, layer.surface, wEff);
        if (layer.contributesNormal) {
            const glm::vec3 detail = glm::mix(glm::vec3(0, 0, 1), layer.normalTS, wEff);
            acc.normalTS = BlendNormalRNM(acc.normalTS, detail, 1.0f);
        }
        if (layer.contributesHeight) acc.height = glm::mix(acc.height, layer.layerHeight, wEff);
    }
    return acc;
}

// ---- Hashes -----------------------------------------------------------------------------
namespace {
u64 HashSurface(const SurfaceParams& s, u64 h) {
    h = HashF32(s.base_color.r, h); h = HashF32(s.base_color.g, h);
    h = HashF32(s.base_color.b, h); h = HashF32(s.base_color.a, h);
    h = HashF32(s.base_metalness, h);
    h = HashF32(s.specular_roughness, h);
    h = HashF32(s.specular_ior, h);
    h = HashF32(s.emission_color.r, h); h = HashF32(s.emission_color.g, h);
    h = HashF32(s.emission_color.b, h);
    h = HashF32(s.coat_weight, h);
    return h;
}
} // namespace

u64 Layer::Hash(u64 seed) const {
    u64 h = HashStr(material, seed);
    h = HashSurface(surface, h);
    h = HashF32(normalTS.x, h); h = HashF32(normalTS.y, h); h = HashF32(normalTS.z, h);
    h = HashF32(layerHeight, h);
    h = mask.Hash(h);
    h = HashF32(opacity, h);
    const u8 b = static_cast<u8>(blend);
    h = HashBytes(&b, 1, h);
    h = HashBytes(&contributesHeight, 1, h);
    h = HashBytes(&contributesNormal, 1, h);
    h = HashF32(heightContribution, h);
    h = HashF32(noiseAmount, h);
    h = HashF32(noiseScale, h);
    return h;
}
u64 LayerStack::Hash() const {
    u64 h = HashSurface(base, 1469598103934665603ull);
    h = HashF32(baseHeight, h);
    for (const auto& l : layers) h = l.Hash(h);
    return h;
}

// ---- JSON I/O ---------------------------------------------------------------------------
namespace {
json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
glm::vec3 G3(const json& j, glm::vec3 d) {
    if (!j.is_array() || j.size() < 3) return d;
    try { return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()}; } catch (...) { return d; }
}
glm::vec4 G4(const json& j, glm::vec4 d) {
    if (!j.is_array() || j.size() < 4) return d;
    try { return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()}; } catch (...) { return d; }
}
template <class T> T JG(const json& j, const char* k, const T& d) {
    const auto it = j.find(k);
    if (it == j.end() || it->is_null()) return d;
    try { return it->get<T>(); } catch (...) { return d; }
}

// Serialize the OpenPBR VALUE subset the resolver blends (child .hbmat files hold the full set;
// this is the runtime layer-stack cache). Deterministic field order.
json SurfaceToJson(const SurfaceParams& s) {
    json j;
    j["baseColor"] = V4(s.base_color);
    j["metalness"] = s.base_metalness;
    j["roughness"] = s.specular_roughness;
    j["ior"] = s.specular_ior;
    j["emission"] = V3(s.emission_color);
    j["emissionLum"] = s.emission_luminance;
    j["coat"] = s.coat_weight;
    return j;
}
void SurfaceFromJson(const json& j, SurfaceParams& s) {
    s.base_color = G4(j.value("baseColor", json()), s.base_color);
    s.base_metalness = JG(j, "metalness", s.base_metalness);
    s.specular_roughness = JG(j, "roughness", s.specular_roughness);
    s.specular_ior = JG(j, "ior", s.specular_ior);
    s.emission_color = G3(j.value("emission", json()), s.emission_color);
    s.emission_luminance = JG(j, "emissionLum", s.emission_luminance);
    s.coat_weight = JG(j, "coat", s.coat_weight);
}

json MaskToJson(const MaskSource& m) {
    json j;
    j["kind"] = static_cast<int>(m.kind);
    j["constant"] = m.constant;
    j["space"] = static_cast<int>(m.textureSpace);
    j["invert"] = m.invert;
    j["paintCanvas"] = m.paintCanvasId;
    j["paintLayer"] = m.paintLayerId;
    j["paintChannel"] = m.paintChannel;
    if (m.kind == MaskKind::Box) j["box"] = json::parse(BoxBrushToJsonString(m.box));
    // Note: baked mask TEXELS are cooked as a separate .uaf mask asset, not inlined in JSON.
    return j;
}
void MaskFromJson(const json& j, MaskSource& m) {
    m.kind = static_cast<MaskKind>(JG(j, "kind", 0));
    m.constant = JG(j, "constant", 1.0f);
    m.textureSpace = static_cast<Space>(JG(j, "space", 0));
    m.invert = JG(j, "invert", false);
    m.paintCanvasId = JG(j, "paintCanvas", 0u);
    m.paintLayerId = JG(j, "paintLayer", 0u);
    m.paintChannel = static_cast<u8>(JG(j, "paintChannel", 0));
    if (const auto it = j.find("box"); it != j.end())
        BoxBrushFromJsonString(it->dump(), m.box);
}

json LayerToJson(const Layer& l) {
    json j;
    if (!l.material.empty()) j["material"] = l.material;
    j["surface"] = SurfaceToJson(l.surface);
    j["normal"] = V3(l.normalTS);
    j["height"] = l.layerHeight;
    j["mask"] = MaskToJson(l.mask);
    j["opacity"] = l.opacity;
    j["blend"] = static_cast<int>(l.blend);
    j["contribHeight"] = l.contributesHeight;
    j["contribNormal"] = l.contributesNormal;
    j["heightContribution"] = l.heightContribution;
    j["noiseAmount"] = l.noiseAmount;
    j["noiseScale"] = l.noiseScale;
    return j;
}
void LayerFromJson(const json& j, Layer& l) {
    l.material = JG(j, "material", std::string());
    if (const auto it = j.find("surface"); it != j.end()) SurfaceFromJson(*it, l.surface);
    l.normalTS = G3(j.value("normal", json()), glm::vec3(0, 0, 1));
    l.layerHeight = JG(j, "height", 0.5f);
    if (const auto it = j.find("mask"); it != j.end()) MaskFromJson(*it, l.mask);
    l.opacity = JG(j, "opacity", 1.0f);
    l.blend = static_cast<BlendMode>(JG(j, "blend", 0));
    l.contributesHeight = JG(j, "contribHeight", true);
    l.contributesNormal = JG(j, "contribNormal", true);
    l.heightContribution = JG(j, "heightContribution", 1.0f);
    l.noiseAmount = JG(j, "noiseAmount", 0.0f);
    l.noiseScale = JG(j, "noiseScale", 8.0f);
}

json BuildStackJson(const LayerStack& s) {
    json j;
    j["version"] = kLayerStackVersion;
    j["base"] = SurfaceToJson(s.base);
    j["baseNormal"] = V3(s.baseNormalTS);
    j["baseHeight"] = s.baseHeight;
    json arr = json::array();
    for (const auto& l : s.layers) arr.push_back(LayerToJson(l));
    j["layers"] = std::move(arr);
    return j;
}
} // namespace

std::string LayerStackToJsonString(const LayerStack& s) { return BuildStackJson(s).dump(2); }

std::optional<LayerStack> LayerStackFromJsonString(const std::string& str) {
    json j;
    try {
        j = json::parse(str);
    } catch (const std::exception& e) {
        HBE_ERROR("LayerStack: parse failed: {}", e.what());
        return std::nullopt;
    }
    LayerStack s;
    if (const auto it = j.find("base"); it != j.end()) SurfaceFromJson(*it, s.base);
    s.baseNormalTS = G3(j.value("baseNormal", json()), glm::vec3(0, 0, 1));
    s.baseHeight = JG(j, "baseHeight", 0.5f);
    if (const auto it = j.find("layers"); it != j.end() && it->is_array())
        for (const auto& jl : *it) {
            Layer l;
            LayerFromJson(jl, l);
            s.layers.push_back(std::move(l));
        }
    return s;
}

bool SaveLayerStack(const fs::path& path, const LayerStack& s) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("LayerStack: cannot write '{}'.", path.string());
        return false;
    }
    out << LayerStackToJsonString(s);
    return true;
}

std::optional<LayerStack> LoadLayerStack(const fs::path& path) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) return std::nullopt;
    return LayerStackFromJsonString(std::string(bytes->begin(), bytes->end()));
}

} // namespace hbe::mat

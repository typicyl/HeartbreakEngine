// Assets/MaterialAsset.cpp
#include "Assets/MaterialAsset.h"

#include "Assets/AssetLoader.h"
#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace hbe::assets {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json ToJson(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

glm::vec3 Vec3(const json& j, glm::vec3 def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
}
glm::vec4 Vec4(const json& j, glm::vec4 def) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
}

} // namespace

bool SaveMaterial(const fs::path& path, const MaterialAsset& mat) {
    const SurfaceParams& s = mat.surface;
    json j;
    j["version"] = 2; // 2 adds the "acoustic" block (P1 acoustic materials)
    j["name"] = mat.name;
    // Legacy top-level keys are preserved so older .hbmat readers keep working; the values
    // are now sourced from SurfaceParams via the documented legacy->OpenPBR mapping
    // (RHI/SurfaceMaterial.h). base_color is a glm::vec4 (rgb = OpenPBR base_color, a = opacity).
    j["baseColor"] = ToJson(s.base_color);
    j["metallic"] = s.base_metalness;
    j["roughness"] = s.specular_roughness;
    j["emissiveColor"] = ToJson(s.emission_color);
    j["emissiveIntensity"] = s.emission_luminance;
    j["subsurfaceColor"] = ToJson(s.subsurface_color);
    j["subsurfaceRadius"] = s.subsurface_radius;
    j["clearcoat"] = s.coat_weight;
    j["clearcoatRoughness"] = s.coat_roughness;
    // New OpenPBR Surface parameters (absent in older files -> spec defaults on load). These
    // are DATA-ONLY in P1: no shader reads them yet (OpenPBR shading lands in P2).
    j["base_weight"] = s.base_weight;
    j["base_diffuse_roughness"] = s.base_diffuse_roughness;
    j["specular_weight"] = s.specular_weight;
    j["specular_color"] = ToJson(s.specular_color);
    j["specular_ior"] = s.specular_ior;
    j["specular_roughness_anisotropy"] = s.specular_roughness_anisotropy;
    j["specular_anisotropy_rotation"] = s.specular_anisotropy_rotation;
    j["transmission_weight"] = s.transmission_weight;
    j["transmission_color"] = ToJson(s.transmission_color);
    j["transmission_depth"] = s.transmission_depth;
    j["transmission_scatter"] = ToJson(s.transmission_scatter);
    j["transmission_scatter_anisotropy"] = s.transmission_scatter_anisotropy;
    j["transmission_dispersion_scale"] = s.transmission_dispersion_scale;
    j["transmission_dispersion_abbe_number"] = s.transmission_dispersion_abbe_number;
    j["thin_walled"] = s.thin_walled;
    j["subsurface_weight"] = s.subsurface_weight;
    j["subsurface_radius_scale"] = ToJson(s.subsurface_radius_scale);
    j["subsurface_scatter_anisotropy"] = s.subsurface_scatter_anisotropy;
    j["coat_color"] = ToJson(s.coat_color);
    j["coat_roughness_anisotropy"] = s.coat_roughness_anisotropy;
    j["coat_ior"] = s.coat_ior;
    j["coat_affect_color"] = s.coat_affect_color;
    j["coat_affect_roughness"] = s.coat_affect_roughness;
    j["fuzz_weight"] = s.fuzz_weight;
    j["fuzz_color"] = ToJson(s.fuzz_color);
    j["fuzz_roughness"] = s.fuzz_roughness;
    j["thin_film_weight"] = s.thin_film_weight;
    j["thin_film_thickness"] = s.thin_film_thickness;
    j["thin_film_ior"] = s.thin_film_ior;
    j["flags"] = mat.flags;
    json& tex = j["textures"] = json::object();
    if (!mat.albedoTex.empty()) tex["albedo"] = mat.albedoTex;
    if (!mat.normalTex.empty()) tex["normal"] = mat.normalTex;
    if (!mat.mrTex.empty()) tex["metallicRoughness"] = mat.mrTex;
    if (!mat.aoTex.empty()) tex["ao"] = mat.aoTex;
    if (!mat.emissiveTex.empty()) tex["emissive"] = mat.emissiveTex;
    if (!mat.thicknessTex.empty()) tex["thickness"] = mat.thicknessTex;

    {
        json a;
        a["preset"] = mat.acousticPreset;
        a["absorption"] = mat.acoustic.absorption; // std::array<f32,9> -> JSON array
        a["scattering"] = mat.acoustic.scattering;
        a["transmission"] = mat.acoustic.transmission;
        j["acoustic"] = std::move(a);
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Material: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(2);
    return true;
}

std::optional<MaterialAsset> LoadMaterial(const fs::path& path) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) return std::nullopt;
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Material: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    MaterialAsset m;
    m.name = j.value("name", path.stem().string());
    // Legacy keys -> SurfaceParams via the documented mapping (RHI/SurfaceMaterial.h). Same
    // keys + defaults as before, so existing .hbmat files load byte-for-byte identically.
    SurfaceParams& s = m.surface;
    s.base_color = Vec4(j.value("baseColor", json()), glm::vec4(1.0f)); // rgb+a -> base_color+geometry_opacity
    s.base_metalness = j.value("metallic", 0.0f);
    s.specular_roughness = j.value("roughness", 0.5f);
    s.emission_color = Vec3(j.value("emissiveColor", json()), glm::vec3(0.0f));
    s.emission_luminance = j.value("emissiveIntensity", 1.0f);
    s.subsurface_color = Vec3(j.value("subsurfaceColor", json()), {0.85f, 0.2f, 0.16f});
    s.subsurface_radius = j.value("subsurfaceRadius", 1.0f);
    s.coat_weight = j.value("clearcoat", 0.0f);
    s.coat_roughness = j.value("clearcoatRoughness", 0.08f);
    // New OpenPBR params: a missing key falls back to the struct's spec-default initializer,
    // so old files get spec defaults and nothing changes. Data-only in P1 (no shader reads).
    s.base_weight = j.value("base_weight", s.base_weight);
    s.base_diffuse_roughness = j.value("base_diffuse_roughness", s.base_diffuse_roughness);
    s.specular_weight = j.value("specular_weight", s.specular_weight);
    s.specular_color = Vec3(j.value("specular_color", json()), s.specular_color);
    s.specular_ior = j.value("specular_ior", s.specular_ior);
    s.specular_roughness_anisotropy = j.value("specular_roughness_anisotropy", s.specular_roughness_anisotropy);
    s.specular_anisotropy_rotation = j.value("specular_anisotropy_rotation", s.specular_anisotropy_rotation);
    s.transmission_weight = j.value("transmission_weight", s.transmission_weight);
    s.transmission_color = Vec3(j.value("transmission_color", json()), s.transmission_color);
    s.transmission_depth = j.value("transmission_depth", s.transmission_depth);
    s.transmission_scatter = Vec3(j.value("transmission_scatter", json()), s.transmission_scatter);
    s.transmission_scatter_anisotropy = j.value("transmission_scatter_anisotropy", s.transmission_scatter_anisotropy);
    s.transmission_dispersion_scale = j.value("transmission_dispersion_scale", s.transmission_dispersion_scale);
    s.transmission_dispersion_abbe_number = j.value("transmission_dispersion_abbe_number", s.transmission_dispersion_abbe_number);
    s.thin_walled = j.value("thin_walled", s.thin_walled);
    s.subsurface_weight = j.value("subsurface_weight", s.subsurface_weight);
    s.subsurface_radius_scale = Vec3(j.value("subsurface_radius_scale", json()), s.subsurface_radius_scale);
    s.subsurface_scatter_anisotropy = j.value("subsurface_scatter_anisotropy", s.subsurface_scatter_anisotropy);
    s.coat_color = Vec3(j.value("coat_color", json()), s.coat_color);
    s.coat_roughness_anisotropy = j.value("coat_roughness_anisotropy", s.coat_roughness_anisotropy);
    s.coat_ior = j.value("coat_ior", s.coat_ior);
    s.coat_affect_color = j.value("coat_affect_color", s.coat_affect_color);
    s.coat_affect_roughness = j.value("coat_affect_roughness", s.coat_affect_roughness);
    s.fuzz_weight = j.value("fuzz_weight", s.fuzz_weight);
    s.fuzz_color = Vec3(j.value("fuzz_color", json()), s.fuzz_color);
    s.fuzz_roughness = j.value("fuzz_roughness", s.fuzz_roughness);
    s.thin_film_weight = j.value("thin_film_weight", s.thin_film_weight);
    s.thin_film_thickness = j.value("thin_film_thickness", s.thin_film_thickness);
    s.thin_film_ior = j.value("thin_film_ior", s.thin_film_ior);
    m.flags = j.value("flags", 0u);
    if (const auto it = j.find("textures"); it != j.end() && it->is_object()) {
        m.albedoTex = it->value("albedo", "");
        m.normalTex = it->value("normal", "");
        m.mrTex = it->value("metallicRoughness", "");
        m.aoTex = it->value("ao", "");
        m.emissiveTex = it->value("emissive", "");
        m.thicknessTex = it->value("thickness", "");
    }
    // Acoustic block (absent in v1 files -> keep the default AcousticMaterial + "Default").
    if (const auto it = j.find("acoustic"); it != j.end() && it->is_object()) {
        m.acousticPreset = it->value("preset", std::string("Default"));
        if (const auto ab = it->find("absorption"); ab != it->end() && ab->is_array()) {
            const int n = std::min<int>(kAcousticBands, static_cast<int>(ab->size()));
            for (int i = 0; i < n; ++i) m.acoustic.absorption[static_cast<usize>(i)] = (*ab)[i].get<f32>();
        }
        m.acoustic.scattering = it->value("scattering", m.acoustic.scattering);
        m.acoustic.transmission = it->value("transmission", m.acoustic.transmission);
    }
    return m;
}

void ApplyMaterial(Renderer& renderer, const fs::path& assetsDir,
                   const MaterialAsset& mat, MeshInstance& instance,
                   std::unordered_map<std::string, rhi::TextureHandle>& texCache) {
    // Full material-value copy (all OpenPBR params) + the feature-flag bitmask.
    instance.surface = mat.surface;
    instance.materialFlags = mat.flags;

    auto load = [&](const std::string& rel) -> rhi::TextureHandle {
        if (rel.empty()) return {};
        if (auto it = texCache.find(rel); it != texCache.end()) return it->second;
        const rhi::TextureHandle h = LoadTexture(renderer, assetsDir / rel);
        texCache[rel] = h;
        return h;
    };
    instance.albedoTexture = load(mat.albedoTex);
    instance.normalTexture = load(mat.normalTex);
    instance.mrTexture = load(mat.mrTex);
    instance.aoTexture = load(mat.aoTex);
    instance.emissiveTexture = load(mat.emissiveTex);
    instance.thicknessTexture = load(mat.thicknessTex);
}

} // namespace hbe::assets

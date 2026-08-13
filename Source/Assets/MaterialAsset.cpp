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
    json j;
    j["version"] = 2; // 2 adds the "acoustic" block (P1 acoustic materials)
    j["name"] = mat.name;
    j["baseColor"] = ToJson(mat.baseColor);
    j["metallic"] = mat.metallic;
    j["roughness"] = mat.roughness;
    j["emissiveColor"] = ToJson(mat.emissiveColor);
    j["emissiveIntensity"] = mat.emissiveIntensity;
    j["subsurfaceColor"] = ToJson(mat.subsurfaceColor);
    j["subsurfaceRadius"] = mat.subsurfaceRadius;
    j["clearcoat"] = mat.clearcoat;
    j["clearcoatRoughness"] = mat.clearcoatRoughness;
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
    m.baseColor = Vec4(j.value("baseColor", json()), glm::vec4(1.0f));
    m.metallic = j.value("metallic", 0.0f);
    m.roughness = j.value("roughness", 0.5f);
    m.emissiveColor = Vec3(j.value("emissiveColor", json()), glm::vec3(0.0f));
    m.emissiveIntensity = j.value("emissiveIntensity", 1.0f);
    m.subsurfaceColor = Vec3(j.value("subsurfaceColor", json()), {0.85f, 0.2f, 0.16f});
    m.subsurfaceRadius = j.value("subsurfaceRadius", 1.0f);
    m.clearcoat = j.value("clearcoat", 0.0f);
    m.clearcoatRoughness = j.value("clearcoatRoughness", 0.08f);
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
    instance.baseColor = mat.baseColor;
    instance.metallic = mat.metallic;
    instance.roughness = mat.roughness;
    instance.emissiveColor = mat.emissiveColor;
    instance.emissiveIntensity = mat.emissiveIntensity;
    instance.subsurfaceColor = mat.subsurfaceColor;
    instance.subsurfaceRadius = mat.subsurfaceRadius;
    instance.clearcoat = mat.clearcoat;
    instance.clearcoatRoughness = mat.clearcoatRoughness;
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

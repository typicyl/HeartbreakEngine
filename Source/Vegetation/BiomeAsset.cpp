// Vegetation/BiomeAsset.cpp - .hbbiome load/save.
#include "Vegetation/BiomeAsset.h"
#include "Vegetation/VegetationWorld.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::veg {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

template <class T>
T JGet(const json& j, const char* key, const T& def) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try { return it->get<T>(); } catch (...) { return def; }
}

glm::vec2 JVec2(const json& j, const char* key, glm::vec2 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 2) return def;
    try { return {(*it)[0].get<f32>(), (*it)[1].get<f32>()}; } catch (...) { return def; }
}

bool ParseBiome(const json& j, const SpeciesRegistry& reg, Biome& out) {
    out = Biome{};
    out.name = JGet<std::string>(j, "name", "biome");
    out.altitudeRange = JVec2(j, "altitudeRange", out.altitudeRange);
    out.slopeRangeDeg = JVec2(j, "slopeRangeDeg", out.slopeRangeDeg);
    out.moistureRange = JVec2(j, "moistureRange", out.moistureRange);
    out.baseDensity = JGet<f32>(j, "baseDensity", out.baseDensity);
    out.waterExclusion = JGet<f32>(j, "waterExclusion", out.waterExclusion);

    const auto sp = j.find("species");
    if (sp != j.end() && sp->is_array()) {
        for (const json& e : *sp) {
            const std::string name = JGet<std::string>(e, "species", "");
            const SpeciesId id = reg.Find(name);
            if (!id.Valid()) {
                HBE_WARN("Biome '{}': species '{}' is not loaded; rule dropped",
                         out.name, name);
                continue;
            }
            BiomeSpeciesRule r;
            r.species = id;
            r.weight = JGet<f32>(e, "weight", 1.0f);
            r.densityMul = JGet<f32>(e, "densityMul", 1.0f);
            r.requireSplatLayer = JGet<i32>(e, "requireSplatLayer", -1);
            out.species.push_back(r);
        }
    }
    return true;
}

} // namespace

bool ParseBiomeJson(const std::string& text, const SpeciesRegistry& reg, Biome& out) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("Biome: failed to parse JSON: {}", e.what());
        return false;
    }
    return ParseBiome(j, reg, out);
}

bool LoadBiome(const fs::path& path, const SpeciesRegistry& reg, Biome& out) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Biome: cannot read '{}'", path.string());
        return false;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Biome: failed to parse '{}': {}", path.string(), e.what());
        return false;
    }
    return ParseBiome(j, reg, out);
}

std::string BiomeToJson(const Biome& b, const SpeciesRegistry& reg) {
    json j;
    j["version"] = 1;
    j["name"] = b.name;
    j["altitudeRange"] = json::array({b.altitudeRange.x, b.altitudeRange.y});
    j["slopeRangeDeg"] = json::array({b.slopeRangeDeg.x, b.slopeRangeDeg.y});
    j["moistureRange"] = json::array({b.moistureRange.x, b.moistureRange.y});
    j["baseDensity"] = b.baseDensity;
    j["waterExclusion"] = b.waterExclusion;
    json arr = json::array();
    for (const BiomeSpeciesRule& r : b.species) {
        json e;
        e["species"] = reg.Valid(r.species) ? reg.Get(r.species).name : std::string();
        e["weight"] = r.weight;
        e["densityMul"] = r.densityMul;
        e["requireSplatLayer"] = r.requireSplatLayer;
        arr.push_back(std::move(e));
    }
    j["species"] = std::move(arr);
    return j.dump(2);
}

bool SaveBiome(const fs::path& path, const Biome& b, const SpeciesRegistry& reg) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        HBE_ERROR("Biome: cannot write '{}'", path.string());
        return false;
    }
    f << BiomeToJson(b, reg);
    return static_cast<bool>(f);
}

} // namespace hbe::veg

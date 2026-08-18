// Vegetation/SpeciesAsset.cpp - .hbspecies load/save.
#include "Vegetation/SpeciesAsset.h"
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
glm::vec2 JVec2(const json& j, const char* key, glm::vec2 d) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 2) return d;
    try { return {(*it)[0].get<f32>(), (*it)[1].get<f32>()}; } catch (...) { return d; }
}
glm::vec4 JVec4(const json& j, const char* key, glm::vec4 d) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 4) return d;
    try {
        return {(*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>(),
                (*it)[3].get<f32>()};
    } catch (...) { return d; }
}

const char* StrategyName(GenStrategy s) {
    switch (s) {
        case GenStrategy::SpaceColonization: return "space_colonization";
        case GenStrategy::LSystem:           return "lsystem";
        case GenStrategy::Custom:            return "custom";
        case GenStrategy::External:          return "external";
        default:                             return "space_colonization";
    }
}
GenStrategy StrategyFromName(const std::string& n) {
    if (n == "lsystem")  return GenStrategy::LSystem;
    if (n == "custom")   return GenStrategy::Custom;
    if (n == "external") return GenStrategy::External;
    return GenStrategy::SpaceColonization;
}

bool ParseSpecies(const json& j, Species& o) {
    o = Species{};
    o.name = JGet<std::string>(j, "name", "species");
    o.strategy = StrategyFromName(JGet<std::string>(j, "strategy", "space_colonization"));

    o.maxHeight = JGet<f32>(j, "maxHeight", o.maxHeight);
    o.trunkRadius = JGet<f32>(j, "trunkRadius", o.trunkRadius);
    o.taper = JGet<f32>(j, "taper", o.taper);
    o.maxBranchOrder = static_cast<u8>(JGet<i32>(j, "maxBranchOrder", o.maxBranchOrder));
    o.branchAngleDeg = JGet<f32>(j, "branchAngleDeg", o.branchAngleDeg);
    o.branchAngleJitter = JGet<f32>(j, "branchAngleJitter", o.branchAngleJitter);
    o.branchDensity = JGet<f32>(j, "branchDensity", o.branchDensity);
    o.crownWidth = JGet<f32>(j, "crownWidth", o.crownWidth);
    o.growthRateMPerYr = JGet<f32>(j, "growthRateMPerYr", o.growthRateMPerYr);
    o.lifespanYears = JGet<f32>(j, "lifespanYears", o.lifespanYears);

    o.leafSize = JGet<f32>(j, "leafSize", o.leafSize);
    o.leafDensity = JGet<f32>(j, "leafDensity", o.leafDensity);
    o.leafColor = JVec4(j, "leafColor", o.leafColor);
    o.leafColorVar = JVec4(j, "leafColorVar", o.leafColorVar);
    o.barkColor = JVec4(j, "barkColor", o.barkColor);

    o.altitudeRange = JVec2(j, "altitudeRange", o.altitudeRange);
    o.slopeToleranceDeg = JVec2(j, "slopeToleranceDeg", o.slopeToleranceDeg);
    o.moistureRange = JVec2(j, "moistureRange", o.moistureRange);
    o.temperatureRange = JVec2(j, "temperatureRange", o.temperatureRange);
    o.sunlightRequirement = JGet<f32>(j, "sunlightRequirement", o.sunlightRequirement);

    o.windResistance = JGet<f32>(j, "windResistance", o.windResistance);
    o.breakResistance = JGet<f32>(j, "breakResistance", o.breakResistance);
    o.regenRate = JGet<f32>(j, "regenRate", o.regenRate);

    o.deciduous = JGet<bool>(j, "deciduous", o.deciduous);
    o.flowers = JGet<bool>(j, "flowers", o.flowers);
    o.fruits = JGet<bool>(j, "fruits", o.fruits);

    o.barkMaterial = JGet<std::string>(j, "barkMaterial", "");
    o.leafMaterial = JGet<std::string>(j, "leafMaterial", "");
    o.authoredMesh = JGet<std::string>(j, "authoredMesh", "");
    return true;
}

} // namespace

bool ParseSpeciesJson(const std::string& text, Species& out) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("Species: failed to parse JSON: {}", e.what());
        return false;
    }
    return ParseSpecies(j, out);
}

bool LoadSpecies(const fs::path& path, Species& out) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Species: cannot read '{}'", path.string());
        return false;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Species: failed to parse '{}': {}", path.string(), e.what());
        return false;
    }
    return ParseSpecies(j, out);
}

std::string SpeciesToJson(const Species& s) {
    auto v2 = [](glm::vec2 v) { return json::array({v.x, v.y}); };
    auto v4 = [](glm::vec4 v) { return json::array({v.x, v.y, v.z, v.w}); };
    json j;
    j["version"] = 1;
    j["name"] = s.name;
    j["strategy"] = StrategyName(s.strategy);
    j["maxHeight"] = s.maxHeight;
    j["trunkRadius"] = s.trunkRadius;
    j["taper"] = s.taper;
    j["maxBranchOrder"] = static_cast<i32>(s.maxBranchOrder);
    j["branchAngleDeg"] = s.branchAngleDeg;
    j["branchAngleJitter"] = s.branchAngleJitter;
    j["branchDensity"] = s.branchDensity;
    j["crownWidth"] = s.crownWidth;
    j["growthRateMPerYr"] = s.growthRateMPerYr;
    j["lifespanYears"] = s.lifespanYears;
    j["leafSize"] = s.leafSize;
    j["leafDensity"] = s.leafDensity;
    j["leafColor"] = v4(s.leafColor);
    j["leafColorVar"] = v4(s.leafColorVar);
    j["barkColor"] = v4(s.barkColor);
    j["altitudeRange"] = v2(s.altitudeRange);
    j["slopeToleranceDeg"] = v2(s.slopeToleranceDeg);
    j["moistureRange"] = v2(s.moistureRange);
    j["temperatureRange"] = v2(s.temperatureRange);
    j["sunlightRequirement"] = s.sunlightRequirement;
    j["windResistance"] = s.windResistance;
    j["breakResistance"] = s.breakResistance;
    j["regenRate"] = s.regenRate;
    j["deciduous"] = s.deciduous;
    j["flowers"] = s.flowers;
    j["fruits"] = s.fruits;
    if (!s.barkMaterial.empty()) j["barkMaterial"] = s.barkMaterial;
    if (!s.leafMaterial.empty()) j["leafMaterial"] = s.leafMaterial;
    if (!s.authoredMesh.empty()) j["authoredMesh"] = s.authoredMesh;
    return j.dump(2);
}

bool SaveSpecies(const fs::path& path, const Species& s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        HBE_ERROR("Species: cannot write '{}'", path.string());
        return false;
    }
    f << SpeciesToJson(s);
    return static_cast<bool>(f);
}

} // namespace hbe::veg

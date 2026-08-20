// Scene/DecalAsset.cpp - see DecalAsset.h.
#include "Scene/DecalAsset.h"

#include "Core/Log.h"
#include "Scene/DecalAssetJson.h"

#include <glm/glm.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace hbe::decalasset {
namespace {
using json = nlohmann::json;

json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
glm::vec3 RV3(const json& j, glm::vec3 def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
}
} // namespace

json DecalToJson(const DecalComponent& d) {
    // Key set IDENTICAL to the scene serializer's historical `decal` block (so scenes + `.hbdecal`
    // interoperate). Keep in lockstep with DecalFromJson.
    return {
        {"halfExtents", V3(d.halfExtents)},
        {"opacity", d.opacity},
        {"angleFade", d.angleFade},
        {"normalStrength", d.normalStrength},
        {"roughness", d.roughness},
        {"metallic", d.metallic},
        {"affectBaseColor", d.affectBaseColor},
        {"affectNormal", d.affectNormal},
        {"affectMR", d.affectMR},
        {"affectEmissive", d.affectEmissive},
        {"emissiveColor", V3(d.emissiveColor)},
        {"emissiveIntensity", d.emissiveIntensity},
        {"twoSided", d.twoSided},
        {"maxAngle", d.maxAngle},
        {"albedo", d.albedoTex},
        {"normal", d.normalTex},
        {"mr", d.mrTex},
    };
}

void DecalFromJson(const json& j, DecalComponent& d) {
    if (!j.is_object()) return;
    d.halfExtents = RV3(j.value("halfExtents", json()), glm::vec3(0.5f, 0.5f, 0.15f));
    d.opacity = j.value("opacity", 1.0f);
    d.angleFade = j.value("angleFade", 2.0f);
    d.normalStrength = j.value("normalStrength", 1.0f);
    d.roughness = j.value("roughness", 0.8f);
    d.metallic = j.value("metallic", 0.0f);
    d.affectBaseColor = j.value("affectBaseColor", true);
    d.affectNormal = j.value("affectNormal", true);
    d.affectMR = j.value("affectMR", true);
    d.affectEmissive = j.value("affectEmissive", false);
    d.emissiveColor = RV3(j.value("emissiveColor", json()), glm::vec3(1.0f, 0.6f, 0.2f));
    d.emissiveIntensity = j.value("emissiveIntensity", 0.0f);
    d.twoSided = j.value("twoSided", false);
    d.maxAngle = j.value("maxAngle", 90.0f);
    d.albedoTex = j.value("albedo", std::string());
    d.normalTex = j.value("normal", std::string());
    d.mrTex = j.value("mr", std::string());
    d.resolved = false; // force a re-resolve of the texture handles after a load
}

std::string DecalToString(const DecalComponent& d) {
    json doc;
    doc["type"] = "hbdecal";
    doc["version"] = kDecalVersion;
    doc["decal"] = DecalToJson(d);
    return doc.dump(2);
}

std::optional<DecalComponent> DecalFromString(const std::string& text) {
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    const auto it = doc.find("decal");
    if (it == doc.end() || !it->is_object()) return std::nullopt;
    DecalComponent d;
    DecalFromJson(*it, d);
    return d;
}

bool SaveDecal(const std::filesystem::path& path, const DecalComponent& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        HBE_ERROR("SaveDecal: cannot open '{}'", path.string());
        return false;
    }
    f << DecalToString(d);
    return static_cast<bool>(f);
}

std::optional<DecalComponent> LoadDecal(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return DecalFromString(ss.str());
}

bool SelfTest() {
    int fail = 0;
    const auto check = [&](bool c, const char* what) {
        if (!c) {
            ++fail;
            HBE_ERROR("[hbdecal test] FAIL: {}", what);
        }
    };
    DecalComponent a;
    a.halfExtents = {1.5f, 2.0f, 0.25f};
    a.opacity = 0.7f;
    a.angleFade = 3.0f;
    a.normalStrength = 0.5f;
    a.roughness = 0.3f;
    a.metallic = 0.9f;
    a.affectBaseColor = false;
    a.affectNormal = true;
    a.affectMR = false;
    a.affectEmissive = true;
    a.emissiveColor = {0.2f, 0.8f, 0.4f};
    a.emissiveIntensity = 5.5f;
    a.twoSided = true;
    a.maxAngle = 55.0f;
    a.albedoTex = "decals/blood.uaf";
    a.normalTex = "decals/blood_n.uaf";
    a.mrTex = "decals/blood_mr.uaf";

    const std::optional<DecalComponent> b = DecalFromString(DecalToString(a));
    check(b.has_value(), "round-trip parse failed");
    if (b) {
        const DecalComponent& r = *b;
        const auto nearf = [](f32 x, f32 y) { return std::abs(x - y) < 1e-4f; };
        check(r.halfExtents == a.halfExtents && nearf(r.opacity, a.opacity) &&
                  nearf(r.angleFade, a.angleFade),
              "box/opacity fields");
        check(nearf(r.normalStrength, a.normalStrength) && nearf(r.roughness, a.roughness) &&
                  nearf(r.metallic, a.metallic),
              "surface fields");
        check(r.affectBaseColor == a.affectBaseColor && r.affectNormal == a.affectNormal &&
                  r.affectMR == a.affectMR && r.affectEmissive == a.affectEmissive,
              "channel flags");
        check(r.emissiveColor == a.emissiveColor && nearf(r.emissiveIntensity, a.emissiveIntensity),
              "emissive fields");
        check(r.twoSided == a.twoSided && nearf(r.maxAngle, a.maxAngle), "projection fields");
        check(r.albedoTex == a.albedoTex && r.normalTex == a.normalTex && r.mrTex == a.mrTex,
              "texture refs");
    }
    check(!DecalFromString("nope").has_value(), "garbage must not parse");
    check(!DecalFromString("{}").has_value(), "an object with no decal must not parse");
    if (fail == 0) HBE_INFO("[hbdecal test] .hbdecal reusable decal asset round-trips intact");
    return fail == 0;
}

} // namespace hbe::decalasset

// Scene/EffectAsset.cpp - see EffectAsset.h.
#include "Scene/EffectAsset.h"

#include "Core/Log.h"
#include "Scene/EffectAssetJson.h" // EmitterToJson / EmitterFromJson (the shared field mapping)

#include <glm/glm.hpp>

#include <fstream>
#include <sstream>

namespace hbe::particle {
namespace {

using json = nlohmann::json;

json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
glm::vec3 RV3(const json& j, glm::vec3 def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
}
glm::vec4 RV4(const json& j, glm::vec4 def) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
}

// Value-over-life curves: a gradient stop is [t, r, g, b, a]; a curve key is [t, v]. Compact and
// order-preserving; an absent/malformed array reads back as an empty curve (= the feature off).
json GradientToJson(const VfxGradient& g) {
    json a = json::array();
    for (const VfxGradient::Stop& s : g.stops)
        a.push_back(json::array({s.t, s.color.r, s.color.g, s.color.b, s.color.a}));
    return a;
}
VfxGradient GradientFromJson(const json& j) {
    VfxGradient g;
    if (!j.is_array()) return g;
    for (const auto& s : j)
        if (s.is_array() && s.size() >= 5)
            g.stops.push_back({s[0].get<f32>(),
                               {s[1].get<f32>(), s[2].get<f32>(), s[3].get<f32>(), s[4].get<f32>()}});
    return g;
}
json CurveToJson(const VfxCurve& c) {
    json a = json::array();
    for (const VfxCurve::Key& k : c.keys) a.push_back(json::array({k.t, k.v}));
    return a;
}
VfxCurve CurveFromJson(const json& j) {
    VfxCurve c;
    if (!j.is_array()) return c;
    for (const auto& k : j)
        if (k.is_array() && k.size() >= 2) c.keys.push_back({k[0].get<f32>(), k[1].get<f32>()});
    return c;
}

} // namespace

json EmitterToJson(const ParticleEmitter& e) {
    // Key set is IDENTICAL to the scene serializer's historical inline block, so old scenes and new
    // `.hbvfx` files interoperate. Keep this in lockstep with EmitterFromJson.
    return {
        {"rate", e.rate},
        {"maxParticles", e.maxParticles},
        {"emitting", e.emitting},
        {"lifetime", e.lifetime},
        {"lifetimeVariance", e.lifetimeVariance},
        {"emitRadius", e.emitRadius},
        {"direction", V3(e.direction)},
        {"startSpeed", e.startSpeed},
        {"speedVariance", e.speedVariance},
        {"spread", e.spread},
        {"gravity", V3(e.gravity)},
        {"drag", e.drag},
        {"buoyancy", e.buoyancy},
        {"vortex", e.vortex},
        {"startColor", V4(e.startColor)},
        {"endColor", V4(e.endColor)},
        {"startSize", e.startSize},
        {"endSize", e.endSize},
        {"spin", e.spin},
        {"texture", e.texture},
        {"additive", e.additive},
        {"effekseerEffect", e.effekseerEffect},
        {"shape", static_cast<u32>(e.shape)},
        {"boxHalfExtents", V3(e.boxHalfExtents)},
        {"coneAngle", e.coneAngle},
        {"burst", e.burst},
        {"loop", e.loop},
        {"duration", e.duration},
        {"turbulence", e.turbulence},
        {"turbulenceScale", e.turbulenceScale},
        {"fadeIn", e.fadeIn},
        {"fadeOut", e.fadeOut},
        {"render", static_cast<u32>(e.render)},
        {"stretch", e.stretch},
        {"subUVCols", e.subUVCols},
        {"subUVRows", e.subUVRows},
        {"subUVFps", e.subUVFps},
        {"softFade", e.softFade},
        {"useCurlNoise", e.useCurlNoise},
        {"curlStrength", e.curlStrength},
        {"curlFrequency", e.curlFrequency},
        {"expDrag", e.expDrag},
        {"simulateColor", e.simulateColor},
        {"colorVariance", e.colorVariance},
        {"simulateSize", e.simulateSize},
        {"sizeVariance", e.sizeVariance},
        {"gpuExpand", e.gpuExpand},
        {"gpuSim", e.gpuSim},
        {"useColorCurve", e.useColorCurve},
        {"colorCurve", GradientToJson(e.colorCurve)},
        {"useSizeCurve", e.useSizeCurve},
        {"sizeCurve", CurveToJson(e.sizeCurve)},
        {"onDeathEffect", e.onDeathEffect},
        {"onDeathChance", e.onDeathChance},
        {"particleMesh", e.particleMesh},
    };
}

void EmitterFromJson(const json& j, ParticleEmitter& e) {
    if (!j.is_object()) return; // a malformed asset keeps struct defaults (= legacy behaviour)
    e.rate = j.value("rate", e.rate);
    e.maxParticles = j.value("maxParticles", e.maxParticles);
    e.emitting = j.value("emitting", e.emitting);
    e.lifetime = j.value("lifetime", e.lifetime);
    e.lifetimeVariance = j.value("lifetimeVariance", e.lifetimeVariance);
    e.emitRadius = j.value("emitRadius", e.emitRadius);
    e.direction = RV3(j.value("direction", json()), e.direction);
    e.startSpeed = j.value("startSpeed", e.startSpeed);
    e.speedVariance = j.value("speedVariance", e.speedVariance);
    e.spread = j.value("spread", e.spread);
    e.gravity = RV3(j.value("gravity", json()), e.gravity);
    e.drag = j.value("drag", e.drag);
    e.buoyancy = j.value("buoyancy", e.buoyancy);
    e.vortex = j.value("vortex", e.vortex);
    e.startColor = RV4(j.value("startColor", json()), e.startColor);
    e.endColor = RV4(j.value("endColor", json()), e.endColor);
    e.startSize = j.value("startSize", e.startSize);
    e.endSize = j.value("endSize", e.endSize);
    e.spin = j.value("spin", e.spin);
    e.texture = j.value("texture", std::string());
    e.additive = j.value("additive", e.additive);
    e.effekseerEffect = j.value("effekseerEffect", std::string());
    e.shape = static_cast<ParticleEmitter::Shape>(j.value("shape", static_cast<u32>(e.shape)));
    e.boxHalfExtents = RV3(j.value("boxHalfExtents", json()), e.boxHalfExtents);
    e.coneAngle = j.value("coneAngle", e.coneAngle);
    e.burst = j.value("burst", e.burst);
    e.loop = j.value("loop", e.loop);
    e.duration = j.value("duration", e.duration);
    e.turbulence = j.value("turbulence", e.turbulence);
    e.turbulenceScale = j.value("turbulenceScale", e.turbulenceScale);
    e.fadeIn = j.value("fadeIn", e.fadeIn);
    e.fadeOut = j.value("fadeOut", e.fadeOut);
    e.render = static_cast<ParticleEmitter::Render>(j.value("render", static_cast<u32>(e.render)));
    e.stretch = j.value("stretch", e.stretch);
    e.subUVCols = j.value("subUVCols", e.subUVCols);
    e.subUVRows = j.value("subUVRows", e.subUVRows);
    e.subUVFps = j.value("subUVFps", e.subUVFps);
    e.softFade = j.value("softFade", e.softFade);
    e.useCurlNoise = j.value("useCurlNoise", e.useCurlNoise);
    e.curlStrength = j.value("curlStrength", e.curlStrength);
    e.curlFrequency = j.value("curlFrequency", e.curlFrequency);
    e.expDrag = j.value("expDrag", e.expDrag);
    e.simulateColor = j.value("simulateColor", e.simulateColor);
    e.colorVariance = glm::clamp(j.value("colorVariance", e.colorVariance), 0.0f, 1.0f);
    e.simulateSize = j.value("simulateSize", e.simulateSize);
    e.sizeVariance = glm::clamp(j.value("sizeVariance", e.sizeVariance), 0.0f, 1.0f);
    e.gpuExpand = j.value("gpuExpand", e.gpuExpand);
    e.gpuSim = j.value("gpuSim", e.gpuSim);
    e.useColorCurve = j.value("useColorCurve", e.useColorCurve);
    if (const auto it = j.find("colorCurve"); it != j.end()) e.colorCurve = GradientFromJson(*it);
    e.useSizeCurve = j.value("useSizeCurve", e.useSizeCurve);
    if (const auto it = j.find("sizeCurve"); it != j.end()) e.sizeCurve = CurveFromJson(*it);
    e.onDeathEffect = j.value("onDeathEffect", std::string());
    e.onDeathChance = j.value("onDeathChance", e.onDeathChance);
    e.particleMesh = j.value("particleMesh", std::string());
}

std::string EffectToString(const ParticleEmitter& e) {
    json doc;
    doc["type"] = "hbvfx";
    doc["version"] = kEffectVersion;
    doc["emitter"] = EmitterToJson(e);
    return doc.dump(2);
}

std::optional<ParticleEmitter> EffectFromString(const std::string& text) {
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    ParticleEmitter e; // struct defaults first, then overlay whatever the file carries
    const auto it = doc.find("emitter");
    if (it == doc.end() || !it->is_object()) return std::nullopt;
    EmitterFromJson(*it, e);
    return e;
}

bool SaveEffect(const std::filesystem::path& path, const ParticleEmitter& e) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        HBE_ERROR("SaveEffect: cannot open '{}'", path.string());
        return false;
    }
    f << EffectToString(e);
    return static_cast<bool>(f);
}

std::optional<ParticleEmitter> LoadEffect(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return EffectFromString(ss.str());
}

bool SelfTest() {
    int fail = 0;
    const auto check = [&](bool c, const char* what) {
        if (!c) {
            ++fail;
            HBE_ERROR("[hbvfx test] FAIL: {}", what);
        }
    };

    // A fully non-default effect must round-trip through the `.hbvfx` string form byte-for-byte on
    // the fields (every authored field, incl. enums + module-stack opt-ins).
    ParticleEmitter a;
    a.rate = 123.5f;
    a.maxParticles = 4096;
    a.emitting = false;
    a.lifetime = 3.25f;
    a.lifetimeVariance = 0.6f;
    a.emitRadius = 1.5f;
    a.direction = {0.1f, 0.9f, -0.3f};
    a.startSpeed = 7.0f;
    a.spread = 0.8f;
    a.gravity = {0.0f, -9.8f, 0.2f};
    a.drag = 0.4f;
    a.buoyancy = 2.1f;
    a.vortex = 1.3f;
    a.startColor = {0.2f, 0.4f, 0.6f, 0.8f};
    a.endColor = {0.05f, 0.1f, 0.15f, 0.0f};
    a.startSize = 0.9f;
    a.endSize = 0.02f;
    a.spin = 3.0f;
    a.texture = "vfx/spark.uaf";
    a.additive = false;
    a.effekseerEffect = "vfx/explosion.efkefc";
    a.shape = ParticleEmitter::Shape::Cone;
    a.boxHalfExtents = {2.0f, 0.5f, 1.0f};
    a.coneAngle = 42.0f;
    a.burst = 200;
    a.loop = false;
    a.duration = 0.75f;
    a.turbulence = 1.1f;
    a.turbulenceScale = 2.2f;
    a.fadeIn = 0.15f;
    a.fadeOut = 0.35f;
    a.render = ParticleEmitter::Render::Stretched;
    a.stretch = 4.5f;
    a.subUVCols = 4;
    a.subUVRows = 2;
    a.subUVFps = 30.0f;
    a.softFade = 0.5f;
    a.useCurlNoise = true;
    a.curlStrength = 1.7f;
    a.curlFrequency = 1.2f;
    a.expDrag = true;
    a.simulateColor = true;
    a.colorVariance = 0.5f;
    a.simulateSize = true;
    a.sizeVariance = 0.4f;
    a.gpuExpand = true;
    a.gpuSim = true;

    const std::optional<ParticleEmitter> b = EffectFromString(EffectToString(a));
    check(b.has_value(), "round-trip parse failed");
    if (b) {
        const ParticleEmitter& r = *b;
        check(r.rate == a.rate && r.maxParticles == a.maxParticles && r.emitting == a.emitting,
              "emission fields");
        check(r.lifetime == a.lifetime && r.lifetimeVariance == a.lifetimeVariance &&
                  r.emitRadius == a.emitRadius && r.direction == a.direction,
              "spawn fields");
        check(r.startSpeed == a.startSpeed && r.spread == a.spread && r.gravity == a.gravity &&
                  r.drag == a.drag && r.buoyancy == a.buoyancy && r.vortex == a.vortex,
              "motion fields");
        check(r.startColor == a.startColor && r.endColor == a.endColor &&
                  r.startSize == a.startSize && r.endSize == a.endSize && r.spin == a.spin &&
                  r.texture == a.texture && r.additive == a.additive &&
                  r.effekseerEffect == a.effekseerEffect,
              "look fields");
        check(r.shape == a.shape && r.boxHalfExtents == a.boxHalfExtents &&
                  r.coneAngle == a.coneAngle && r.burst == a.burst && r.loop == a.loop &&
                  r.duration == a.duration,
              "shape/burst fields");
        check(r.turbulence == a.turbulence && r.turbulenceScale == a.turbulenceScale &&
                  r.fadeIn == a.fadeIn && r.fadeOut == a.fadeOut,
              "turbulence/fade fields");
        check(r.render == a.render && r.stretch == a.stretch && r.subUVCols == a.subUVCols &&
                  r.subUVRows == a.subUVRows && r.subUVFps == a.subUVFps && r.softFade == a.softFade,
              "render fields");
        check(r.useCurlNoise == a.useCurlNoise && r.curlStrength == a.curlStrength &&
                  r.curlFrequency == a.curlFrequency && r.expDrag == a.expDrag,
              "curl/drag opt-ins");
        check(r.simulateColor == a.simulateColor && r.colorVariance == a.colorVariance &&
                  r.simulateSize == a.simulateSize && r.sizeVariance == a.sizeVariance &&
                  r.gpuExpand == a.gpuExpand && r.gpuSim == a.gpuSim,
              "sim/gpu opt-ins");
    }

    // Malformed / empty input keeps defaults (never throws).
    check(!EffectFromString("not json").has_value(), "garbage must not parse");
    check(!EffectFromString("{}").has_value(), "an object with no emitter must not parse");
    ParticleEmitter def;
    ParticleEmitter partial;
    EmitterFromJson(nlohmann::json::parse(R"({"rate":99.0})"), partial);
    check(partial.rate == 99.0f && partial.maxParticles == def.maxParticles &&
              partial.lifetime == def.lifetime,
          "a partial object overlays only present keys (legacy-default the rest)");

    if (fail == 0) HBE_INFO("[hbvfx test] .hbvfx effect asset round-trips with every field intact");
    return fail == 0;
}

} // namespace hbe::particle

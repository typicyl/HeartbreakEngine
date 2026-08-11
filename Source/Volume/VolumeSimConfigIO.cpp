// Volume/VolumeSimConfigIO.cpp - JSON read/write for the `.hbvolsim` authoring asset.
#include "Volume/VolumeSimConfigIO.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace hbe::volume {
namespace {
using json = nlohmann::json;

json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); } // x,y,z,w
json I3(const glm::ivec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 RdV3(const json& j, glm::vec3 d) {
    if (j.is_array() && j.size() >= 3) return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
    return d;
}
glm::ivec3 RdI3(const json& j, glm::ivec3 d) {
    if (j.is_array() && j.size() >= 3) return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    return d;
}
glm::quat RdQ(const json& j, glm::quat d) {
    if (j.is_array() && j.size() >= 4)
        return {j[3].get<f32>(), j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()}; // w,x,y,z
    return d;
}

json SaveScalarCurve(const VolumeScalarCurve& c) {
    json keys = json::array();
    for (const VolumeScalarKey& k : c.keys) keys.push_back({{"t", k.time}, {"v", k.value}});
    return json{{"constant", c.constant}, {"keys", std::move(keys)}};
}
VolumeScalarCurve RdScalarCurve(const json& j) {
    VolumeScalarCurve c;
    if (!j.is_object()) return c;
    c.constant = j.value("constant", 0.0f);
    if (const auto it = j.find("keys"); it != j.end() && it->is_array())
        for (const json& jk : *it) c.keys.push_back({jk.value("t", 0.0f), jk.value("v", 0.0f)});
    return c;
}
json SaveVec3Curve(const VolumeVec3Curve& c) {
    json keys = json::array();
    for (const VolumeVec3Key& k : c.keys) keys.push_back({{"t", k.time}, {"v", V3(k.value)}});
    return json{{"constant", V3(c.constant)}, {"keys", std::move(keys)}};
}
VolumeVec3Curve RdVec3Curve(const json& j) {
    VolumeVec3Curve c;
    if (!j.is_object()) return c;
    c.constant = RdV3(j.value("constant", json()), glm::vec3(0.0f));
    if (const auto it = j.find("keys"); it != j.end() && it->is_array())
        for (const json& jk : *it)
            c.keys.push_back({jk.value("t", 0.0f), RdV3(jk.value("v", json()), glm::vec3(0.0f))});
    return c;
}

json SaveShape(const VolumeShape& s) {
    return json{{"kind", static_cast<int>(s.kind)},
                {"center", V3(s.center)},
                {"halfExtents", V3(s.halfExtents)},
                {"rotation", V4(s.rotation)},
                {"coneHeight", s.coneHeight},
                {"edgeSoftness", s.edgeSoftness},
                {"meshId", s.meshId}};
}
VolumeShape RdShape(const json& j) {
    VolumeShape s;
    if (!j.is_object()) return s;
    s.kind = static_cast<VolumeShapeKind>(glm::clamp(j.value("kind", 0), 0, 3));
    s.center = RdV3(j.value("center", json()), s.center);
    s.halfExtents = RdV3(j.value("halfExtents", json()), s.halfExtents);
    s.rotation = RdQ(j.value("rotation", json()), s.rotation);
    s.coneHeight = j.value("coneHeight", s.coneHeight);
    s.edgeSoftness = j.value("edgeSoftness", s.edgeSoftness);
    s.meshId = j.value("meshId", s.meshId);
    return s;
}
} // namespace

nlohmann::json ConfigToJson(const VolumeSimConfig& c) {
    json j;
    j["type"] = "volumesim";
    j["version"] = 1;
    j["model"] = c.model;

    j["bounds"] = {{"worldMin", V3(c.bounds.worldMin)},
                   {"worldMax", V3(c.bounds.worldMax)},
                   {"dim", I3(c.bounds.dim)}};

    j["frameRate"] = c.frameRate;
    j["substeps"] = c.substeps;
    j["ambientTemperature"] = c.ambientTemperature;
    j["buoyancyAlpha"] = c.buoyancyAlpha;
    j["buoyancyBeta"] = c.buoyancyBeta;
    j["densityDissipation"] = c.densityDissipation;
    j["temperatureCooling"] = c.temperatureCooling;
    j["vorticityStrength"] = c.vorticityStrength;
    j["pressureIterations"] = c.pressureIterations;
    j["gravity"] = V3(c.gravity);
    j["seed"] = c.seed;
    j["keyframeInterval"] = c.keyframeInterval;

    json emitters = json::array();
    for (const VolumeEmitter& e : c.emitters) {
        emitters.push_back({{"name", e.name},
                            {"shape", SaveShape(e.shape)},
                            {"densityRate", e.densityRate},
                            {"temperatureRate", e.temperatureRate},
                            {"temperatureTarget", e.temperatureTarget},
                            {"fuelRate", e.fuelRate},
                            {"velocity", V3(e.velocity)},
                            {"worldVelocity", e.worldVelocity},
                            {"mode", static_cast<int>(e.mode)},
                            {"startTime", e.startTime},
                            {"endTime", e.endTime},
                            {"burstDuration", e.burstDuration},
                            {"translationCurve", SaveVec3Curve(e.translationCurve)},
                            {"densityRateCurve", SaveScalarCurve(e.densityRateCurve)}});
    }
    j["emitters"] = std::move(emitters);

    json obstacles = json::array();
    for (const VolumeObstacle& o : c.obstacles) {
        obstacles.push_back({{"name", o.name},
                             {"shape", SaveShape(o.shape)},
                             {"kind", static_cast<int>(o.kind)},
                             {"moving", o.moving},
                             {"translationCurve", SaveVec3Curve(o.translationCurve)}});
    }
    j["obstacles"] = std::move(obstacles);

    j["bakeFields"] = c.bakeFields;   // vector<string> -> JSON array
    j["modelParams"] = c.modelParams; // map<string,f32> -> JSON object
    return j;
}

bool SaveVolumeSimConfig(const std::filesystem::path& path, const VolumeSimConfig& config) {
    const json j = ConfigToJson(config);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("VolumeSim: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

bool ConfigFromJson(const nlohmann::json& j, VolumeSimConfig& out) {
    VolumeSimConfig c; // start from defaults so a partial/older object still loads runnable
    // The whole extraction is in one try: a present-but-wrong-typed key (or a NaN/Inf that json::dump
    // wrote out as null) makes nlohmann get<T>() throw type_error. That must degrade to "return false"
    // per the header contract - NOT escape and unwind the editor frame loop (which has no handler, so
    // an escaped throw is std::terminate mid-session).
    try {
    c.model = j.value("model", c.model);
    if (const auto b = j.find("bounds"); b != j.end() && b->is_object()) {
        c.bounds.worldMin = RdV3(b->value("worldMin", json()), c.bounds.worldMin);
        c.bounds.worldMax = RdV3(b->value("worldMax", json()), c.bounds.worldMax);
        c.bounds.dim = RdI3(b->value("dim", json()), c.bounds.dim);
    }
    c.frameRate = j.value("frameRate", c.frameRate);
    c.substeps = j.value("substeps", c.substeps);
    c.ambientTemperature = j.value("ambientTemperature", c.ambientTemperature);
    c.buoyancyAlpha = j.value("buoyancyAlpha", c.buoyancyAlpha);
    c.buoyancyBeta = j.value("buoyancyBeta", c.buoyancyBeta);
    c.densityDissipation = j.value("densityDissipation", c.densityDissipation);
    c.temperatureCooling = j.value("temperatureCooling", c.temperatureCooling);
    c.vorticityStrength = j.value("vorticityStrength", c.vorticityStrength);
    c.pressureIterations = j.value("pressureIterations", c.pressureIterations);
    c.gravity = RdV3(j.value("gravity", json()), c.gravity);
    c.seed = j.value("seed", c.seed);
    c.keyframeInterval = j.value("keyframeInterval", c.keyframeInterval);

    if (const auto it = j.find("emitters"); it != j.end() && it->is_array()) {
        c.emitters.clear();
        for (const json& je : *it) {
            VolumeEmitter e;
            e.name = je.value("name", e.name);
            e.shape = RdShape(je.value("shape", json()));
            e.densityRate = je.value("densityRate", e.densityRate);
            e.temperatureRate = je.value("temperatureRate", e.temperatureRate);
            e.temperatureTarget = je.value("temperatureTarget", e.temperatureTarget);
            e.fuelRate = je.value("fuelRate", e.fuelRate);
            e.velocity = RdV3(je.value("velocity", json()), e.velocity);
            e.worldVelocity = je.value("worldVelocity", e.worldVelocity);
            e.mode = static_cast<VolumeEmitter::Mode>(glm::clamp(je.value("mode", 0), 0, 1));
            e.startTime = je.value("startTime", e.startTime);
            e.endTime = je.value("endTime", e.endTime);
            e.burstDuration = je.value("burstDuration", e.burstDuration);
            e.translationCurve = RdVec3Curve(je.value("translationCurve", json()));
            e.densityRateCurve = RdScalarCurve(je.value("densityRateCurve", json()));
            c.emitters.push_back(std::move(e));
        }
    }
    if (const auto it = j.find("obstacles"); it != j.end() && it->is_array()) {
        c.obstacles.clear();
        for (const json& jo : *it) {
            VolumeObstacle o;
            o.name = jo.value("name", o.name);
            o.shape = RdShape(jo.value("shape", json()));
            o.kind = static_cast<VolumeObstacle::Kind>(glm::clamp(jo.value("kind", 0), 0, 1));
            o.moving = jo.value("moving", o.moving);
            o.translationCurve = RdVec3Curve(jo.value("translationCurve", json()));
            c.obstacles.push_back(std::move(o));
        }
    }
    if (const auto it = j.find("bakeFields"); it != j.end() && it->is_array()) {
        c.bakeFields.clear();
        for (const json& jf : *it)
            if (jf.is_string()) c.bakeFields.push_back(jf.get<std::string>());
    }
    if (const auto it = j.find("modelParams"); it != j.end() && it->is_object())
        for (auto& [k, v] : it->items())
            if (v.is_number()) c.modelParams[k] = v.get<f32>();
    } catch (const std::exception& e) {
        HBE_ERROR("VolumeSim: config parse error: {}", e.what());
        return false;
    }

    out = std::move(c);
    return true;
}

bool LoadVolumeSimConfig(const std::filesystem::path& path, VolumeSimConfig& out) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("VolumeSim: cannot read '{}'.", path.string());
        return false;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("VolumeSim: failed to parse '{}': {}", path.string(), e.what());
        return false;
    }
    return ConfigFromJson(j, out);
}

} // namespace hbe::volume

// Project/Project.cpp
#include "Project/Project.h"
#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Scene/PostSettingsSerialization.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe {

Project Project::s_active;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
glm::vec3 ReadVec3(const json& j, const char* key, const glm::vec3& def) {
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 3) {
        return glm::vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
    }
    return def;
}
json WriteVec3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

// Fills `s` from a parsed `.hbproj` document (shared by Open and OpenPacked).
void ParseSettings(const json& j, ProjectSettings& s) {
    s.name = j.value("name", "Untitled");
    s.startupScene = j.value("startupScene", "");
    // Legacy keys (mainMenuScene/hudScene/loadingScene) are silently ignored:
    // those screens are UIPanels inside uiScene now.
    s.studioLoadingScene = j.value("studioLoadingScene", "");
    s.uiScene = j.value("uiScene", "");
    s.musicGraph = j.value("musicGraph", "");
    s.musicStartState = j.value("musicStartState", "");
    // Reset before reading so a re-parse REPLACES rather than appends (the same
    // settings_ member is reused across in-process project switches; the glyph vectors
    // and inputActions would otherwise accumulate the previous project's entries).
    s.inputIcons = InputIcons{};
    s.inputActions.clear();
    const auto readGlyphs = [](const json& jd, DeviceGlyphs& g) {
        if (!jd.is_array()) return;
        for (const json& e : jd)
            g.icons.emplace_back(e.value("id", 0u), e.value("tex", std::string()));
    };
    if (const auto it = j.find("inputIcons"); it != j.end() && it->is_object()) {
        InputIcons& ic = s.inputIcons;
        ic.general = it->value("general", "");
        ic.logo = it->value("logo", "");
        ic.useGeneralAlways = it->value("useGeneralAlways", false);
        readGlyphs(it->value("keyboard", json::array()), ic.keyboard);
        readGlyphs(it->value("xbox", json::array()), ic.xbox);
        readGlyphs(it->value("playstation", json::array()), ic.playstation);
        readGlyphs(it->value("nintendo", json::array()), ic.nintendo);
        readGlyphs(it->value("generic", json::array()), ic.generic);
    }
    // Data-driven input actions. Seed a default "Interact" ONLY when the key is entirely
    // absent (legacy / first-time project); a present-but-empty array is an intentional
    // zero-action project and must round-trip as empty rather than resurrect "Interact".
    if (const auto it = j.find("inputActions"); it != j.end() && it->is_array()) {
        for (const json& ja : *it) {
            input::ActionDef a;
            a.name = ja.value("name", "");
            a.defaults.key = static_cast<Key>(ja.value("key", 0u));
            a.defaults.pad = ja.value("pad", 0u);
            if (!a.name.empty()) s.inputActions.push_back(std::move(a));
        }
    } else {
        s.inputActions.push_back({"Interact", {Key::E, static_cast<u32>(Gamepad_X)}});
    }
    s.audioBuses.clear();
    if (const auto it = j.find("audioBuses"); it != j.end() && it->is_array()) {
        for (const json& jb : *it) {
            AudioBusSetting bus;
            bus.name = jb.value("name", "");
            bus.parent = jb.value("parent", "Master");
            bus.volume = jb.value("volume", 1.0f);
            bus.muted = jb.value("muted", false);
            if (!bus.name.empty()) s.audioBuses.push_back(std::move(bus));
        }
    }
    s.build = BuildSettings{}; // defaults for projects without a block
    if (const auto it = j.find("build"); it != j.end() && it->is_object()) {
        BuildSettings& b = s.build;
        b.gameName = it->value("gameName", "");
        b.company = it->value("company", "");
        b.version = it->value("version", "1.0.0");
        b.backend = it->value("backend", "d3d12");
        b.width = it->value("width", 1280u);
        b.height = it->value("height", 720u);
        b.fullscreen = it->value("fullscreen", true);
        b.vsync = it->value("vsync", true);
        b.packAssets = it->value("packAssets", true);
        b.compressAssets = it->value("compressAssets", true);
        b.onlyReferenced = it->value("onlyReferenced", false);
        b.uiScaleMode = it->value("uiScaleMode", 1u);
        b.uiRefWidth = it->value("uiRefWidth", 1920u);
        b.uiRefHeight = it->value("uiRefHeight", 1080u);
        b.devMenu = it->value("devMenu", false);
        b.profiles.clear();
        if (const auto pit = it->find("profiles"); pit != it->end() && pit->is_array()) {
            for (const auto& pe : *pit) {
                if (!pe.is_object()) continue;
                BuildProfile prof;
                prof.platform = pe.value("platform", "windows");
                if (const auto bit = pe.find("backends"); bit != pe.end() && bit->is_array())
                    for (const auto& be : *bit)
                        if (be.is_string()) prof.backends.push_back(be.get<std::string>());
                b.profiles.push_back(std::move(prof));
            }
        }
    }
    s.environment = EnvironmentSettings{}; // defaults
    if (const auto it = j.find("environment"); it != j.end() && it->is_object()) {
        EnvironmentSettings& env = s.environment;
        if (const auto sit = it->find("sky"); sit != it->end() && sit->is_object()) {
            SkySettings& sky = env.sky;
            sky.horizonColor = ReadVec3(*sit, "horizon", sky.horizonColor);
            sky.zenithColor = ReadVec3(*sit, "zenith", sky.zenithColor);
            sky.groundColor = ReadVec3(*sit, "ground", sky.groundColor);
            sky.sunDirection = ReadVec3(*sit, "sunDirection", sky.sunDirection);
            sky.sunTint = ReadVec3(*sit, "sunTint", sky.sunTint);
            sky.sunIntensity = sit->value("sunIntensity", sky.sunIntensity);
            sky.skyIntensity = sit->value("skyIntensity", sky.skyIntensity);
        }
        env.sunColor = ReadVec3(*it, "sunColor", env.sunColor);
        env.sunLightIntensity = it->value("sunLightIntensity", env.sunLightIntensity);
        env.ambientIntensity = it->value("ambientIntensity", env.ambientIntensity);
        env.exposure = it->value("exposure", env.exposure);
        env.timeOfDay = it->value("timeOfDay", env.timeOfDay);
        env.dayLengthSeconds = it->value("dayLengthSeconds", env.dayLengthSeconds);
        env.dynamicSky = it->value("dynamicSky", env.dynamicSky);
        env.cloudCoverage = it->value("cloudCoverage", env.cloudCoverage);
        env.cloudDensity = it->value("cloudDensity", env.cloudDensity);
        env.overcast = it->value("overcast", env.overcast);
        env.windAngle = it->value("windAngle", env.windAngle);
        env.windSpeed = it->value("windSpeed", env.windSpeed);
        if (const auto pit = it->find("post"); pit != it->end() && pit->is_object())
            scene::PostFromJson(*pit, env.post);
    }
}
} // namespace

bool Project::Open(const fs::path& projectFile) {
    std::error_code ec;
    if (!fs::exists(projectFile, ec)) {
        HBE_ERROR("Project: '{}' does not exist.", projectFile.string());
        return false;
    }

    std::ifstream in(projectFile);
    if (!in) {
        HBE_ERROR("Project: cannot open '{}'.", projectFile.string());
        return false;
    }

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        HBE_ERROR("Project: failed to parse '{}': {}", projectFile.string(), e.what());
        return false;
    }

    ParseSettings(j, settings_);

    projectFile_ = fs::absolute(projectFile);
    root_ = projectFile_.parent_path();

    fs::create_directories(AssetsDir(), ec);
    vfs::SetSearchRoot(AssetsDir()); // filename fallback for moved assets
    HBE_INFO("Project: opened '{}' (root '{}')", settings_.name, root_.string());
    return true;
}

bool Project::OpenPacked(const fs::path& mountDir) {
    // The shipped `.hbproj` is packed under the virtual path "__project.hbproj";
    // read it through the VFS (the packs must already be mounted at mountDir).
    const fs::path assetsDir = fs::absolute(mountDir) / "Assets";
    const auto bytes = vfs::ReadFile(assetsDir / "__project.hbproj");
    if (!bytes || bytes->empty()) {
        HBE_ERROR("Project: no packed project (__project.hbproj) in the mounted packs.");
        return false;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Project: failed to parse packed project: {}", e.what());
        return false;
    }

    ParseSettings(j, settings_);
    root_ = fs::absolute(mountDir);
    projectFile_ = root_ / "__project.hbproj"; // synthetic (lives in the pack)
    vfs::SetSearchRoot(AssetsDir());
    HBE_INFO("Project: opened packed '{}' (root '{}')", settings_.name, root_.string());
    return true;
}

bool Project::Create(const fs::path& directory, const std::string& name) {
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        HBE_ERROR("Project: cannot create directory '{}': {}", directory.string(), ec.message());
        return false;
    }

    root_ = fs::absolute(directory);
    settings_.name = name;
    settings_.startupScene.clear();
    projectFile_ = root_ / (name + ".hbproj");

    fs::create_directories(AssetsDir(), ec);
    if (!Save()) return false;

    vfs::SetSearchRoot(AssetsDir());
    HBE_INFO("Project: created '{}' at '{}'", name, root_.string());
    return true;
}

bool Project::Save() const {
    json j;
    j["name"] = settings_.name;
    j["startupScene"] = settings_.startupScene;
    j["studioLoadingScene"] = settings_.studioLoadingScene;
    j["uiScene"] = settings_.uiScene;
    j["musicGraph"] = settings_.musicGraph;
    j["musicStartState"] = settings_.musicStartState;
    const auto writeGlyphs = [](const DeviceGlyphs& g) {
        json arr = json::array();
        for (const auto& e : g.icons) arr.push_back({{"id", e.first}, {"tex", e.second}});
        return arr;
    };
    {
        const InputIcons& ic = settings_.inputIcons;
        j["inputIcons"] = {{"general", ic.general},
                           {"logo", ic.logo},
                           {"useGeneralAlways", ic.useGeneralAlways},
                           {"keyboard", writeGlyphs(ic.keyboard)},
                           {"xbox", writeGlyphs(ic.xbox)},
                           {"playstation", writeGlyphs(ic.playstation)},
                           {"nintendo", writeGlyphs(ic.nintendo)},
                           {"generic", writeGlyphs(ic.generic)}};
    }
    {
        json arr = json::array();
        for (const input::ActionDef& a : settings_.inputActions)
            arr.push_back({{"name", a.name},
                           {"key", static_cast<u32>(a.defaults.key)},
                           {"pad", a.defaults.pad}});
        j["inputActions"] = std::move(arr);
    }
    j["engine"] = "HeartbreakEngine";
    j["version"] = 1;
    const BuildSettings& b = settings_.build;
    j["build"] = {{"gameName", b.gameName},
                  {"company", b.company},
                  {"version", b.version},
                  {"backend", b.backend},
                  {"width", b.width},
                  {"height", b.height},
                  {"fullscreen", b.fullscreen},
                  {"vsync", b.vsync},
                  {"packAssets", b.packAssets},
                  {"compressAssets", b.compressAssets},
                  {"onlyReferenced", b.onlyReferenced},
                  {"uiScaleMode", b.uiScaleMode},
                  {"uiRefWidth", b.uiRefWidth},
                  {"uiRefHeight", b.uiRefHeight},
                  {"devMenu", b.devMenu}};
    {
        json profs = json::array();
        for (const BuildProfile& p : b.profiles)
            profs.push_back({{"platform", p.platform}, {"backends", p.backends}});
        j["build"]["profiles"] = profs;
    }

    {
        const EnvironmentSettings& env = settings_.environment;
        const SkySettings& s = env.sky;
        j["environment"] = {
            {"sky",
             {{"horizon", WriteVec3(s.horizonColor)},
              {"zenith", WriteVec3(s.zenithColor)},
              {"ground", WriteVec3(s.groundColor)},
              {"sunDirection", WriteVec3(s.sunDirection)},
              {"sunTint", WriteVec3(s.sunTint)},
              {"sunIntensity", s.sunIntensity},
              {"skyIntensity", s.skyIntensity}}},
            {"sunColor", WriteVec3(env.sunColor)},
            {"sunLightIntensity", env.sunLightIntensity},
            {"ambientIntensity", env.ambientIntensity},
            {"exposure", env.exposure},
            {"timeOfDay", env.timeOfDay},
            {"dayLengthSeconds", env.dayLengthSeconds},
            {"dynamicSky", env.dynamicSky},
            {"cloudCoverage", env.cloudCoverage},
            {"cloudDensity", env.cloudDensity},
            {"overcast", env.overcast},
            {"windAngle", env.windAngle},
            {"windSpeed", env.windSpeed},
            {"post", scene::PostToJson(env.post)}};
    }

    if (!settings_.audioBuses.empty()) {
        json buses = json::array();
        for (const AudioBusSetting& b2 : settings_.audioBuses) {
            buses.push_back({{"name", b2.name},
                             {"parent", b2.parent},
                             {"volume", b2.volume},
                             {"muted", b2.muted}});
        }
        j["audioBuses"] = std::move(buses);
    }

    std::ofstream out(projectFile_);
    if (!out) {
        HBE_ERROR("Project: cannot write '{}'.", projectFile_.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

std::string Project::RelativeAssetPath(const fs::path& absolute) const {
    std::error_code ec;
    fs::path rel = fs::relative(absolute, AssetsDir(), ec);
    if (ec) return absolute.filename().string();
    return rel.generic_string();
}

} // namespace hbe

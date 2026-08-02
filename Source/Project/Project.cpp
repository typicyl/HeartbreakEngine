// Project/Project.cpp
#include "Project/Project.h"
#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Scene/PostSettingsSerialization.h"
#include "Scene/StreamingSalvage.h" // salvage::DefaultUnloadRadius (SALVAGE 2)
#include "Scene/TagTable.h" // tags::Normalize / Reset / SeedFromProject

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
    // those screens are UIPanels inside the UI document now.
    //
    // The UI slots take a `.hbui` DOCUMENT and fall back to the pre-document
    // `.hbscene` key. Unlike the three above, this fallback is NOT a silent
    // drop - the resolved path keeps its extension and the boot sequence
    // BRANCHES on it, running the old additive scene load for a `.hbscene` and
    // adopting the result as a document. Silently ignoring these the way
    // mainMenuScene is ignored would leave a half-migrated project with no menu
    // at all, because uiManagerMode_ is set at exactly one site.
    // Save() writes only the new keys, so the first save migrates the file.
    s.bootDocument = j.value("bootDocument", j.value("studioLoadingScene", ""));
    // 3D main menu (defaults here MUST match ProjectSettings' in-struct defaults).
    s.menuWorld = j.value("menuWorld", false);
    s.menuCamera = j.value("menuCamera", "");
    s.menuTag = j.value("menuTag", "");
    s.uiDocument = j.value("uiDocument", j.value("uiScene", ""));
    // SCREEN LIST. `uiDocuments` (one .hbui per screen) is authoritative when
    // present; otherwise it is SEEDED from the single-document key above, so a
    // project that never migrates boots byte-identically to before. The struct
    // default and the fallback here are the same value - an EMPTY vector - which
    // is the rule this codebase has been bitten by twice.
    s.uiDocuments.clear();
    if (const auto it = j.find("uiDocuments"); it != j.end() && it->is_array()) {
        for (const json& e : *it)
            if (e.is_string() && !e.get<std::string>().empty())
                s.uiDocuments.push_back(e.get<std::string>());
    }
    if (s.uiDocuments.empty()) {
        if (!s.uiDocument.empty()) s.uiDocuments.push_back(s.uiDocument);
    } else {
        // The legacy mirror always names the MENU document, so the two keys can
        // never disagree about which screen supplies `post`.
        s.uiDocument = s.uiDocuments.front();
    }
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
    // Streaming tags. Cleared first for the same reason inputActions is (the
    // reused settings_ member), and NORMALIZED after the read rather than
    // trusted: tags::Normalize is what guarantees index 0 is "Untagged", drops
    // nameless/duplicate rows, and enforces the load/unload hysteresis band on
    // every row (salvage::EnforceHysteresis - a degenerate band thrashes
    // spawn/despawn every frame, so it is corrected, not warned about and kept).
    //
    // Absent and present-but-empty are the SAME here, deliberately, unlike
    // inputActions: the list can never legitimately be empty, because
    // "Untagged" has to exist for an untagged entity to mean anything. So there
    // is nothing to seed and nothing to preserve - Normalize supplies index 0
    // either way.
    s.tags.clear();
    if (const auto it = j.find("tags"); it != j.end() && it->is_array()) {
        for (const json& jt : *it) {
            if (!jt.is_object()) continue;
            TagDef t;
            t.name = jt.value("name", "");
            t.loadRadius = jt.value("loadRadius", 120.0f);
            t.unloadRadius = jt.value("unloadRadius", salvage::DefaultUnloadRadius(t.loadRadius));
            t.priority = jt.value("priority", 0);
            t.alwaysLoaded = jt.value("alwaysLoaded", false);
            // MUST match TagDef's in-struct default (Project.h). A `.value(key, X)`
            // fallback is a SECOND default: disagree with the struct and a tag row that
            // omits the key silently behaves differently from a freshly created one.
            t.autoShard = jt.value("autoShard", false);
            t.shardCell = jt.value("shardCell", 0.0f);
            // Associated tags, by NAME. Read with find + is_array rather than
            // `.value(key, {})`: an ARRAY has no scalar fallback, and the
            // two-places-default rule is satisfied by construction here - the struct
            // default is {} and an absent key leaves the freshly constructed {}, so
            // there is no second default to disagree with. Junk entries are skipped
            // the way every other list in this parser skips them; tags::Normalize
            // below then drops self-references and duplicates. A name this project
            // does not list is deliberately KEPT: validation is a bake-time report,
            // not a parse-time deletion of authored intent (see Scene/TagShard.cpp).
            if (const auto ja = jt.find("associates"); ja != jt.end() && ja->is_array()) {
                for (const json& jn : *ja) {
                    if (!jn.is_string()) continue;
                    std::string n = jn.get<std::string>();
                    if (!n.empty()) t.associates.push_back(std::move(n));
                }
            }
            if (!t.name.empty()) s.tags.push_back(std::move(t));
        }
    }
    tags::Normalize(s.tags);
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
    s.occlusion = AudioOcclusionSettings{};
    if (const auto it = j.find("occlusion"); it != j.end() && it->is_object()) {
        AudioOcclusionSettings& o = s.occlusion;
        o.enabled = it->value("enabled", false);
        o.rays = it->value("rays", 4);
        o.attenuation = it->value("attenuation", 0.35f);
        o.cutoffHz = it->value("cutoffHz", 700.0f);
        o.spread = it->value("spread", 0.7f);
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
        b.allowMissingRefs = it->value("allowMissingRefs", false);
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
    // The tag table is process-wide, so it MUST be rebuilt from the list we just
    // read - an in-process project switch would otherwise keep the previous
    // project's ids and silently mis-map every tag in the new one. Same bug class
    // as the inputActions clear above, one level up.
    tags::SeedFromProject(settings_.tags);

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
    tags::SeedFromProject(settings_.tags); // see Open()
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
    // Create does not go through ParseSettings, so it resets the tag list here -
    // otherwise a new project would inherit the previously open one's tags (and
    // Save() below would write them into the fresh `.hbproj`).
    settings_.tags.clear();
    tags::Normalize(settings_.tags); // supplies "Untagged" at index 0
    tags::SeedFromProject(settings_.tags);
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
    // New keys only - the legacy uiScene/studioLoadingScene are read on load and
    // dropped on the first save, which is how a project migrates itself.
    j["bootDocument"] = settings_.bootDocument;
    j["menuWorld"] = settings_.menuWorld;
    j["menuCamera"] = settings_.menuCamera;
    j["menuTag"] = settings_.menuTag;
    // Both keys, always. `uiDocuments` is the truth; `uiDocument` is its first
    // entry, emitted so an older reader (or anything still keyed on the old slot)
    // gets the MENU document rather than a silently blank slot.
    j["uiDocuments"] = settings_.uiDocuments;
    j["uiDocument"] = settings_.uiDocuments.empty() ? settings_.uiDocument
                                                    : settings_.uiDocuments.front();
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
    {
        // Written UNCONDITIONALLY, including "Untagged" at index 0, so the file
        // shows the whole authored order (a tag's index IS its runtime id) and is
        // hand-editable. A re-parse folds the "Untagged" row back into index 0
        // via tags::Normalize rather than appending a second one.
        json arr = json::array();
        for (const TagDef& t : settings_.tags)
            arr.push_back({{"name", t.name},
                           {"loadRadius", t.loadRadius},
                           {"unloadRadius", t.unloadRadius},
                           {"priority", t.priority},
                           {"alwaysLoaded", t.alwaysLoaded},
                           {"autoShard", t.autoShard},
                           {"shardCell", t.shardCell},
                           {"associates", t.associates}});
        j["tags"] = std::move(arr);
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
                  {"allowMissingRefs", b.allowMissingRefs},
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
    {
        const AudioOcclusionSettings& o = settings_.occlusion;
        j["occlusion"] = {{"enabled", o.enabled},
                          {"rays", o.rays},
                          {"attenuation", o.attenuation},
                          {"cutoffHz", o.cutoffHz},
                          {"spread", o.spread}};
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

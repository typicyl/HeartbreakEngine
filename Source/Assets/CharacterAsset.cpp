// Assets/CharacterAsset.cpp
#include "Assets/CharacterAsset.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::assets {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
const char* SeamModeName(SeamMode m) { return m == SeamMode::Overlap ? "overlap" : "continuous"; }
SeamMode ParseSeamMode(const std::string& s) {
    return s == "overlap" ? SeamMode::Overlap : SeamMode::Continuous;
}
} // namespace

bool SaveCharacter(const fs::path& path, const CharacterAsset& c) {
    json j;
    j["version"] = 1;
    j["skeleton"] = c.skeleton;
    if (!c.weldCache.empty()) j["weldCache"] = c.weldCache;

    json& slots = j["slots"] = json::array();
    for (const CharacterSlot& s : c.slots) {
        json js;
        js["name"] = s.name;
        if (!s.seamNeighbors.empty()) js["seamNeighbors"] = s.seamNeighbors;
        slots.push_back(std::move(js));
    }

    json& vars = j["variants"] = json::array();
    for (const CharacterVariant& v : c.variants) {
        json jv;
        jv["id"] = v.id;
        jv["slot"] = v.slot;
        jv["mesh"] = v.mesh;
        if (!v.material.empty()) jv["material"] = v.material;
        jv["seamMode"] = SeamModeName(v.seamMode);
        if (v.isDefault) jv["default"] = true;
        vars.push_back(std::move(jv));
    }

    json& lo = j["loadouts"] = json::array();
    for (const CharacterLoadout& l : c.loadouts) {
        json jl;
        jl["name"] = l.name;
        json& sl = jl["slots"] = json::object();
        for (const auto& [slot, variant] : l.slots) sl[slot] = variant;
        lo.push_back(std::move(jl));
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Character: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(2);
    return true;
}

std::optional<CharacterAsset> LoadCharacter(const fs::path& path) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) return std::nullopt;
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Character: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    CharacterAsset c;
    c.skeleton = j.value("skeleton", "");
    c.weldCache = j.value("weldCache", "");

    if (const auto it = j.find("slots"); it != j.end() && it->is_array()) {
        for (const json& js : *it) {
            CharacterSlot s;
            s.name = js.value("name", "");
            if (const auto n = js.find("seamNeighbors"); n != js.end() && n->is_array())
                s.seamNeighbors = n->get<std::vector<std::string>>();
            if (!s.name.empty()) c.slots.push_back(std::move(s));
        }
    }

    if (const auto it = j.find("variants"); it != j.end() && it->is_array()) {
        for (const json& jv : *it) {
            CharacterVariant v;
            v.id = jv.value("id", "");
            v.slot = jv.value("slot", "");
            v.mesh = jv.value("mesh", "");
            v.material = jv.value("material", "");
            v.seamMode = ParseSeamMode(jv.value("seamMode", "continuous"));
            v.isDefault = jv.value("default", false);
            if (!v.id.empty()) c.variants.push_back(std::move(v));
        }
    }

    if (const auto it = j.find("loadouts"); it != j.end() && it->is_array()) {
        for (const json& jl : *it) {
            CharacterLoadout l;
            l.name = jl.value("name", "");
            if (const auto s = jl.find("slots"); s != jl.end() && s->is_object())
                for (const auto& [slot, variant] : s->items())
                    l.slots[slot] = variant.get<std::string>();
            if (!l.name.empty()) c.loadouts.push_back(std::move(l));
        }
    }
    return c;
}

} // namespace hbe::assets

// Assets/MusicGraph.cpp
#include "Assets/MusicGraph.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::assets {

using json = nlohmann::json;

bool SaveMusicGraph(const std::filesystem::path& path, const MusicGraph& g) {
    json j;
    j["type"] = "musicGraph";
    j["version"] = 1;
    j["defaultFade"] = g.defaultFade;
    j["initialState"] = g.initialState;

    json params = json::array();
    for (const MusicParameter& p : g.parameters) {
        params.push_back({{"name", p.name},
                          {"default", p.defaultValue},
                          {"min", p.min},
                          {"max", p.max}});
    }
    j["parameters"] = std::move(params);

    json states = json::array();
    for (const MusicState& s : g.states) {
        json layers = json::array();
        for (const MusicLayer& l : s.layers) {
            layers.push_back({{"name", l.name},
                              {"asset", l.asset},
                              {"volume", l.volume},
                              {"parameter", l.parameter},
                              {"paramLo", l.paramLo},
                              {"paramHi", l.paramHi}});
        }
        states.push_back({{"name", s.name},
                          {"bpm", s.bpm},
                          {"beatsPerBar", s.beatsPerBar},
                          {"layers", std::move(layers)}});
    }
    j["states"] = std::move(states);

    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("MusicGraph: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

std::optional<MusicGraph> LoadMusicGraph(const std::filesystem::path& path) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("MusicGraph: cannot read '{}'.", path.string());
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("MusicGraph: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    MusicGraph g;
    g.defaultFade = j.value("defaultFade", 2.0f);
    g.initialState = j.value("initialState", "");
    if (const auto it = j.find("parameters"); it != j.end() && it->is_array()) {
        for (const json& jp : *it) {
            MusicParameter p;
            p.name = jp.value("name", "intensity");
            p.defaultValue = jp.value("default", 0.0f);
            p.min = jp.value("min", 0.0f);
            p.max = jp.value("max", 1.0f);
            g.parameters.push_back(std::move(p));
        }
    }
    if (const auto it = j.find("states"); it != j.end() && it->is_array()) {
        for (const json& js : *it) {
            MusicState s;
            s.name = js.value("name", "State");
            s.bpm = js.value("bpm", 120.0f);
            s.beatsPerBar = js.value("beatsPerBar", 4);
            if (const auto lit = js.find("layers"); lit != js.end() && lit->is_array()) {
                for (const json& jl : *lit) {
                    MusicLayer l;
                    l.name = jl.value("name", "Layer");
                    l.asset = jl.value("asset", "");
                    l.volume = jl.value("volume", 1.0f);
                    l.parameter = jl.value("parameter", "");
                    l.paramLo = jl.value("paramLo", 0.0f);
                    l.paramHi = jl.value("paramHi", 1.0f);
                    s.layers.push_back(std::move(l));
                }
            }
            g.states.push_back(std::move(s));
        }
    }
    return g;
}

} // namespace hbe::assets

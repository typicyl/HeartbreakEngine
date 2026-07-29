// Assets/MusicGraph.cpp
#include "Assets/MusicGraph.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe {

const char* SyncName(MusicSync s) {
    switch (s) {
        case MusicSync::Immediate: return "Immediate";
        case MusicSync::Beat:      return "Next beat";
        case MusicSync::Bar:       return "Next bar";
        case MusicSync::TwoBars:   return "Next 2 bars";
        case MusicSync::FourBars:  return "Next 4 bars";
    }
    return "Immediate";
}

f32 SyncInterval(MusicSync s, f32 bpm, int beatsPerBar) {
    if (s == MusicSync::Immediate) return 0.0f;
    const f32 beat = 60.0f / std::max(bpm, 1.0f);
    const f32 bar = beat * static_cast<f32>(std::max(beatsPerBar, 1));
    switch (s) {
        case MusicSync::Beat:     return beat;
        case MusicSync::Bar:      return bar;
        case MusicSync::TwoBars:  return bar * 2.0f;
        case MusicSync::FourBars: return bar * 4.0f;
        case MusicSync::Immediate: break;
    }
    return 0.0f;
}

} // namespace hbe

namespace hbe::assets {

using json = nlohmann::json;

bool SaveMusicGraph(const std::filesystem::path& path, const MusicGraph& g) {
    json j;
    j["type"] = "musicGraph";
    j["version"] = 2; // v2: per-state sync + dialogue ducking
    j["defaultFade"] = g.defaultFade;
    j["initialState"] = g.initialState;
    j["duckDecibels"] = g.duckDecibels;
    j["duckAttack"] = g.duckAttack;
    j["duckRelease"] = g.duckRelease;

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
                          {"sync", static_cast<int>(s.sync)},
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
    g.duckDecibels = std::max(j.value("duckDecibels", 0.0f), 0.0f);
    g.duckAttack = std::max(j.value("duckAttack", 0.15f), 0.0f);
    g.duckRelease = std::max(j.value("duckRelease", 0.60f), 0.0f);
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
            // v1 graphs switched instantly; keep that as the default so an
            // existing score's timing does not change under the author.
            s.sync = static_cast<MusicSync>(
                std::clamp(js.value("sync", static_cast<int>(MusicSync::Immediate)), 0, 4));
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

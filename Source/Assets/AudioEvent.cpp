// Assets/AudioEvent.cpp
#include "Assets/AudioEvent.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::assets {

using json = nlohmann::json;

bool SaveAudioEvent(const std::filesystem::path& path, const AudioEvent& ev) {
    json j;
    j["type"] = "audioEvent";
    j["version"] = 1;
    j["bus"] = ev.bus;
    json sounds = json::array();
    for (const AudioEventSound& s : ev.sounds) {
        sounds.push_back({{"asset", s.asset}, {"weight", s.weight}});
    }
    j["sounds"] = std::move(sounds);
    j["volume"] = ev.volume;
    j["volumeVariance"] = ev.volumeVariance;
    j["pitch"] = ev.pitch;
    j["pitchVariance"] = ev.pitchVariance;
    j["loop"] = ev.loop;
    j["spatial"] = ev.spatial;
    j["minDistance"] = ev.minDistance;
    j["maxDistance"] = ev.maxDistance;

    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("AudioEvent: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

std::optional<AudioEvent> LoadAudioEvent(const std::filesystem::path& path) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("AudioEvent: cannot read '{}'.", path.string());
        return std::nullopt;
    }

    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("AudioEvent: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    AudioEvent ev;
    ev.bus = j.value("bus", "SFX");
    if (const auto it = j.find("sounds"); it != j.end() && it->is_array()) {
        for (const json& js : *it) {
            AudioEventSound s;
            s.asset = js.value("asset", "");
            s.weight = js.value("weight", 1.0f);
            ev.sounds.push_back(std::move(s));
        }
    }
    ev.volume = j.value("volume", 1.0f);
    ev.volumeVariance = j.value("volumeVariance", 0.0f);
    ev.pitch = j.value("pitch", 1.0f);
    ev.pitchVariance = j.value("pitchVariance", 0.0f);
    ev.loop = j.value("loop", false);
    ev.spatial = j.value("spatial", false);
    ev.minDistance = j.value("minDistance", 1.0f);
    ev.maxDistance = j.value("maxDistance", 30.0f);
    return ev;
}

} // namespace hbe::assets

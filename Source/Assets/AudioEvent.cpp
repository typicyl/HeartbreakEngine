// Assets/AudioEvent.cpp
#include "Assets/AudioEvent.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace hbe::assets {

using json = nlohmann::json;

namespace {
json SoundsToJson(const std::vector<AudioEventSound>& sounds) {
    json arr = json::array();
    for (const AudioEventSound& s : sounds) arr.push_back({{"asset", s.asset}, {"weight", s.weight}});
    return arr;
}
void SoundsFromJson(const json& arr, std::vector<AudioEventSound>& out) {
    if (!arr.is_array()) return;
    for (const json& js : arr) {
        AudioEventSound s;
        s.asset = js.value("asset", "");
        s.weight = js.value("weight", 1.0f);
        out.push_back(std::move(s));
    }
}
} // namespace

bool SaveAudioEvent(const std::filesystem::path& path, const AudioEvent& ev) {
    json j;
    j["type"] = "audioEvent";
    j["version"] = 2; // 2 adds composite "components"
    j["bus"] = ev.bus;
    j["sounds"] = SoundsToJson(ev.sounds);
    j["volume"] = ev.volume;
    j["volumeVariance"] = ev.volumeVariance;
    j["pitch"] = ev.pitch;
    j["pitchVariance"] = ev.pitchVariance;
    j["loop"] = ev.loop;
    j["spatial"] = ev.spatial;
    j["minDistance"] = ev.minDistance;
    j["maxDistance"] = ev.maxDistance;

    json comps = json::array();
    for (const AudioEventComponent& c : ev.components) {
        comps.push_back({{"name", c.name},
                         {"sounds", SoundsToJson(c.sounds)},
                         {"offset", {c.offset.x, c.offset.y, c.offset.z}},
                         {"delaySeconds", c.delaySeconds},
                         {"volume", c.volume},
                         {"volumeVariance", c.volumeVariance},
                         {"pitch", c.pitch},
                         {"pitchVariance", c.pitchVariance},
                         {"bus", c.bus},
                         {"loop", c.loop},
                         {"spatial", c.spatial},
                         {"minDistance", c.minDistance},
                         {"maxDistance", c.maxDistance},
                         {"reverbSend", c.reverbSend},
                         {"spread", c.spread},
                         {"enabled", c.enabled}});
    }
    j["components"] = std::move(comps);

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
    if (const auto it = j.find("sounds"); it != j.end()) SoundsFromJson(*it, ev.sounds);
    ev.volume = j.value("volume", 1.0f);
    ev.volumeVariance = j.value("volumeVariance", 0.0f);
    ev.pitch = j.value("pitch", 1.0f);
    ev.pitchVariance = j.value("pitchVariance", 0.0f);
    ev.loop = j.value("loop", false);
    ev.spatial = j.value("spatial", false);
    ev.minDistance = j.value("minDistance", 1.0f);
    ev.maxDistance = j.value("maxDistance", 30.0f);

    if (const auto it = j.find("components"); it != j.end() && it->is_array()) {
        for (const json& jc : *it) {
            AudioEventComponent c;
            c.name = jc.value("name", "Component");
            if (const auto sit = jc.find("sounds"); sit != jc.end()) SoundsFromJson(*sit, c.sounds);
            if (const auto oit = jc.find("offset");
                oit != jc.end() && oit->is_array() && oit->size() >= 3)
                c.offset = {(*oit)[0].get<f32>(), (*oit)[1].get<f32>(), (*oit)[2].get<f32>()};
            c.delaySeconds = jc.value("delaySeconds", 0.0f);
            c.volume = jc.value("volume", 1.0f);
            c.volumeVariance = jc.value("volumeVariance", 0.0f);
            c.pitch = jc.value("pitch", 1.0f);
            c.pitchVariance = jc.value("pitchVariance", 0.0f);
            c.bus = jc.value("bus", "");
            c.loop = jc.value("loop", false);
            c.spatial = jc.value("spatial", true);
            c.minDistance = jc.value("minDistance", 1.0f);
            c.maxDistance = jc.value("maxDistance", 30.0f);
            c.reverbSend = jc.value("reverbSend", 1.0f);
            c.spread = jc.value("spread", 0.0f);
            c.enabled = jc.value("enabled", true);
            ev.components.push_back(std::move(c));
        }
    }
    return ev;
}

bool AudioEventSelfTest() {
    AudioEvent ev;
    ev.bus = "SFX";
    AudioEventComponent muzzle;
    muzzle.name = "Muzzle Blast";
    muzzle.sounds.push_back({"sfx/muzzle.uaf", 1.0f});
    muzzle.offset = {0.1f, 0.0f, 0.5f};
    muzzle.reverbSend = 0.5f;
    muzzle.spread = 30.0f;
    AudioEventComponent tail;
    tail.name = "Distant Tail";
    tail.sounds.push_back({"sfx/tail.uaf", 1.0f});
    tail.delaySeconds = 0.25f;
    tail.reverbSend = 2.0f;
    tail.minDistance = 5.0f;
    ev.components = {muzzle, tail};

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "hbe_audioevent_selftest.hbevent";
    bool ok = SaveAudioEvent(tmp, ev);
    const std::optional<AudioEvent> loaded = LoadAudioEvent(tmp);
    ok = ok && loaded.has_value() && loaded->components.size() == 2;
    if (ok) {
        const AudioEventComponent& m = loaded->components[0];
        const AudioEventComponent& t = loaded->components[1];
        ok = ok && m.name == "Muzzle Blast" && m.sounds.size() == 1 &&
             m.sounds[0].asset == "sfx/muzzle.uaf" && std::fabs(m.offset.z - 0.5f) < 1e-4f &&
             std::fabs(m.spread - 30.0f) < 1e-3f;
        ok = ok && t.name == "Distant Tail" && std::fabs(t.delaySeconds - 0.25f) < 1e-4f &&
             std::fabs(t.reverbSend - 2.0f) < 1e-4f && std::fabs(t.minDistance - 5.0f) < 1e-4f;
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (!ok) std::printf("  [audioevent] composite round-trip FAILED\n");
    return ok;
}

} // namespace hbe::assets

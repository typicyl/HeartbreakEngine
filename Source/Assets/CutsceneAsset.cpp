// Assets/CutsceneAsset.cpp
#include "Assets/CutsceneAsset.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace hbe::assets {

using json = nlohmann::json;

namespace {
json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }
glm::vec3 RdV3(const json& j, glm::vec3 def) {
    if (j.is_array() && j.size() >= 3) return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
    return def;
}
glm::quat RdQ(const json& j, glm::quat def) {
    if (j.is_array() && j.size() >= 4)
        return {j[3].get<f32>(), j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()}; // w,x,y,z
    return def;
}
} // namespace

bool SaveCutscene(const std::filesystem::path& path, const CutsceneAsset& c) {
    json j;
    j["type"] = "cutscene";
    j["version"] = 1;
    j["duration"] = c.duration;

    json cam = json::array();
    for (const CutsceneCameraKey& k : c.camera) {
        cam.push_back({{"time", k.time},
                       {"position", V3(k.position)},
                       {"aim", V3(k.aim)},
                       {"fov", k.fov},
                       {"cut", k.cut}});
    }
    j["camera"] = std::move(cam);

    json tracks = json::array();
    for (const CutsceneAnimTrack& t : c.animTracks) {
        json keys = json::array();
        for (const CutsceneTransformKey& k : t.keys) {
            keys.push_back({{"time", k.time},
                            {"p", V3(k.position)},
                            {"r", V4(k.rotation)},
                            {"s", V3(k.scale)}});
        }
        json clips = json::array();
        for (const CutsceneClipMarker& m : t.clips)
            clips.push_back({{"time", m.time}, {"clip", m.clip}});
        tracks.push_back({{"target", t.target}, {"keys", std::move(keys)},
                          {"clips", std::move(clips)}});
    }
    j["animTracks"] = std::move(tracks);

    json dlg = json::array();
    for (const CutsceneDialogueMarker& m : c.dialogue) {
        dlg.push_back({{"time", m.time}, {"dialogue", m.dialogue},
                       {"voiceline", m.voiceline}});
    }
    j["dialogue"] = std::move(dlg);

    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Cutscene: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

std::optional<CutsceneAsset> LoadCutscene(const std::filesystem::path& path) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Cutscene: cannot read '{}'.", path.string());
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Cutscene: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    CutsceneAsset c;
    c.duration = j.value("duration", 5.0f);
    if (const auto it = j.find("camera"); it != j.end() && it->is_array()) {
        for (const json& jk : *it) {
            CutsceneCameraKey k;
            k.time = jk.value("time", 0.0f);
            k.position = RdV3(jk.value("position", json()), k.position);
            k.aim = RdV3(jk.value("aim", json()), k.aim);
            k.fov = jk.value("fov", 60.0f);
            k.cut = jk.value("cut", false);
            c.camera.push_back(k);
        }
    }
    if (const auto it = j.find("animTracks"); it != j.end() && it->is_array()) {
        for (const json& jt : *it) {
            CutsceneAnimTrack t;
            t.target = jt.value("target", "");
            if (const auto ik = jt.find("keys"); ik != jt.end() && ik->is_array()) {
                for (const json& jk : *ik) {
                    CutsceneTransformKey k;
                    k.time = jk.value("time", 0.0f);
                    k.position = RdV3(jk.value("p", json()), glm::vec3(0.0f));
                    k.rotation = RdQ(jk.value("r", json()), glm::quat(1, 0, 0, 0));
                    k.scale = RdV3(jk.value("s", json()), glm::vec3(1.0f));
                    t.keys.push_back(k);
                }
            }
            if (const auto ic = jt.find("clips"); ic != jt.end() && ic->is_array()) {
                for (const json& jm : *ic)
                    t.clips.push_back({jm.value("time", 0.0f), jm.value("clip", 0)});
            }
            c.animTracks.push_back(std::move(t));
        }
    }
    if (const auto it = j.find("dialogue"); it != j.end() && it->is_array()) {
        for (const json& jm : *it) {
            CutsceneDialogueMarker m;
            m.time = jm.value("time", 0.0f);
            m.dialogue = jm.value("dialogue", "");
            m.voiceline = jm.value("voiceline", "");
            c.dialogue.push_back(std::move(m));
        }
    }

    // The runtime interpolator assumes ascending time order (it scans forward
    // and treats the last key as the terminal pose). The editor lets a key's
    // time be dragged past its neighbours, so normalise ordering on load — for
    // both hand-authored files and anything the editor saved out of order.
    auto byTime = [](const auto& a, const auto& b) { return a.time < b.time; };
    std::stable_sort(c.camera.begin(), c.camera.end(), byTime);
    for (CutsceneAnimTrack& t : c.animTracks) {
        std::stable_sort(t.keys.begin(), t.keys.end(), byTime);
        std::stable_sort(t.clips.begin(), t.clips.end(), byTime);
    }
    std::stable_sort(c.dialogue.begin(), c.dialogue.end(), byTime);
    return c;
}

} // namespace hbe::assets

// Assets/CutsceneAsset.cpp
#include "Assets/CutsceneAsset.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace hbe {

// Declared in `hbe` (next to CutsceneEase itself), not in `hbe::assets` - the
// enum belongs to the asset's data model, which lives in the outer namespace.
const char* EaseName(CutsceneEase e) {
    switch (e) {
        case CutsceneEase::Linear:    return "Linear";
        case CutsceneEase::EaseIn:    return "Ease In";
        case CutsceneEase::EaseOut:   return "Ease Out";
        case CutsceneEase::EaseInOut: return "Ease In-Out";
        case CutsceneEase::Hold:      return "Hold";
    }
    return "Linear";
}

f32 ApplyEase(CutsceneEase e, f32 u) {
    u = std::clamp(u, 0.0f, 1.0f);
    switch (e) {
        case CutsceneEase::Linear:    return u;
        case CutsceneEase::EaseIn:    return u * u;
        case CutsceneEase::EaseOut:   return 1.0f - (1.0f - u) * (1.0f - u);
        case CutsceneEase::EaseInOut: return u * u * (3.0f - 2.0f * u); // smoothstep
        case CutsceneEase::Hold:      return 0.0f;                      // never leaves `a`
    }
    return u;
}

} // namespace hbe

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
    j["version"] = 2; // v2: per-key ease/roll/focus/aimTarget + shake/subtitle tracks
    j["duration"] = c.duration;

    json cam = json::array();
    for (const CutsceneCameraKey& k : c.camera) {
        cam.push_back({{"time", k.time},
                       {"position", V3(k.position)},
                       {"aim", V3(k.aim)},
                       {"fov", k.fov},
                       {"cut", k.cut},
                       {"ease", static_cast<int>(k.ease)},
                       {"roll", k.roll},
                       {"focusDistance", k.focusDistance},
                       {"focusRange", k.focusRange},
                       {"aperture", k.aperture},
                       {"aimTarget", k.aimTarget}});
    }
    j["camera"] = std::move(cam);

    json shakes = json::array();
    for (const CutsceneShakeMarker& m : c.shakes)
        shakes.push_back({{"time", m.time}, {"trauma", m.trauma}});
    j["shakes"] = std::move(shakes);

    json subs = json::array();
    for (const CutsceneSubtitleMarker& m : c.subtitles) {
        subs.push_back({{"time", m.time},
                        {"duration", m.duration},
                        {"speaker", m.speaker},
                        {"text", m.text}});
    }
    j["subtitles"] = std::move(subs);

    j["cinematic"] = {{"handheld", c.cinematic.handheld},
                      {"handheldPosAmount", c.cinematic.handheldPosAmount},
                      {"handheldRotAmount", c.cinematic.handheldRotAmount},
                      {"handheldFrequency", c.cinematic.handheldFrequency},
                      {"handheldRoll", c.cinematic.handheldRoll},
                      {"handheldSharpness", c.cinematic.handheldSharpness},
                      {"breathing", c.cinematic.breathing},
                      {"breathAmount", c.cinematic.breathAmount},
                      {"breathRate", c.cinematic.breathRate},
                      {"framing", c.cinematic.framing},
                      {"framingX", c.cinematic.framingX},
                      {"framingY", c.cinematic.framingY},
                      {"leadAmount", c.cinematic.leadAmount},
                      {"leadSpeed", c.cinematic.leadSpeed},
                      {"framingDamping", c.cinematic.framingDamping}};

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
            // v1 files have none of these; the defaults reproduce v1 playback
            // EXCEPT ease, which defaults to EaseInOut on a fresh key. A v1 file
            // was authored against linear interpolation, so honour that.
            k.ease = static_cast<CutsceneEase>(glm::clamp(
                jk.value("ease", static_cast<int>(CutsceneEase::Linear)), 0, 4));
            k.roll = jk.value("roll", 0.0f);
            k.focusDistance = jk.value("focusDistance", -1.0f);
            k.focusRange = jk.value("focusRange", 3.0f);
            k.aperture = jk.value("aperture", 0.0f);
            k.aimTarget = jk.value("aimTarget", "");
            c.camera.push_back(std::move(k));
        }
    }
    if (const auto it = j.find("shakes"); it != j.end() && it->is_array()) {
        for (const json& jm : *it)
            c.shakes.push_back({jm.value("time", 0.0f),
                                glm::clamp(jm.value("trauma", 0.5f), 0.0f, 1.0f)});
    }
    if (const auto it = j.find("subtitles"); it != j.end() && it->is_array()) {
        for (const json& jm : *it) {
            CutsceneSubtitleMarker m;
            m.time = jm.value("time", 0.0f);
            m.duration = jm.value("duration", 0.0f);
            m.speaker = jm.value("speaker", "");
            m.text = jm.value("text", "");
            c.subtitles.push_back(std::move(m));
        }
    }
    if (const auto it = j.find("cinematic"); it != j.end() && it->is_object()) {
        cam::CinematicSettings& cs = c.cinematic;
        cs.handheld = it->value("handheld", cs.handheld);
        cs.handheldPosAmount = it->value("handheldPosAmount", cs.handheldPosAmount);
        cs.handheldRotAmount = it->value("handheldRotAmount", cs.handheldRotAmount);
        cs.handheldFrequency = it->value("handheldFrequency", cs.handheldFrequency);
        cs.handheldRoll = it->value("handheldRoll", cs.handheldRoll);
        cs.handheldSharpness = it->value("handheldSharpness", cs.handheldSharpness);
        cs.breathing = it->value("breathing", cs.breathing);
        cs.breathAmount = it->value("breathAmount", cs.breathAmount);
        cs.breathRate = it->value("breathRate", cs.breathRate);
        cs.framing = it->value("framing", cs.framing);
        cs.framingX = it->value("framingX", cs.framingX);
        cs.framingY = it->value("framingY", cs.framingY);
        cs.leadAmount = it->value("leadAmount", cs.leadAmount);
        cs.leadSpeed = it->value("leadSpeed", cs.leadSpeed);
        cs.framingDamping = it->value("framingDamping", cs.framingDamping);
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
    std::stable_sort(c.shakes.begin(), c.shakes.end(), byTime);
    std::stable_sort(c.subtitles.begin(), c.subtitles.end(), byTime);
    return c;
}

} // namespace hbe::assets

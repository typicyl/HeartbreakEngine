// Cinematics/SequenceAsset.cpp - .hbseq JSON serialization.
#include "Cinematics/SequenceAsset.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hbe::cine {
namespace {
using json = nlohmann::json;

// ---- primitives ----
json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); } // x,y,z,w
glm::vec3 RdV3(const json& j, glm::vec3 def) {
    if (j.is_array() && j.size() >= 3) return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
    return def;
}
glm::quat RdQ(const json& j, glm::quat def) {
    if (j.is_array() && j.size() >= 4)
        return {j[3].get<f32>(), j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()}; // w,x,y,z
    return def;
}
// 64-bit guids round-trip as 16-char hex strings (JSON mangles large integers).
std::string HexU64(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}
u64 RdHexU64(const json& j) {
    if (!j.is_string()) return 0;
    const std::string s = j.get<std::string>();
    if (s.empty()) return 0;
    return std::strtoull(s.c_str(), nullptr, 16);
}

json SerParamsF(const std::vector<std::pair<std::string, f32>>& p) {
    json o = json::object();
    for (const auto& kv : p) o[kv.first] = kv.second;
    return o;
}
json SerParamsS(const std::vector<std::pair<std::string, std::string>>& p) {
    json o = json::object();
    for (const auto& kv : p) o[kv.first] = kv.second;
    return o;
}
void RdParamsF(const json& obj, std::vector<std::pair<std::string, f32>>& out) {
    if (!obj.is_object()) return;
    for (auto& el : obj.items()) out.emplace_back(el.key(), el.value().get<f32>());
}
void RdParamsS(const json& obj, std::vector<std::pair<std::string, std::string>>& out) {
    if (!obj.is_object()) return;
    for (auto& el : obj.items()) out.emplace_back(el.key(), el.value().get<std::string>());
}

// ---- curve ----
json SerCurve(const curve::Curve& c) {
    json keys = json::array();
    for (const auto& k : c.keys) {
        keys.push_back({{"t", k.time},
                        {"v", k.value},
                        {"i", static_cast<int>(k.interp)},
                        {"tm", static_cast<int>(k.tangentMode)},
                        {"wm", static_cast<int>(k.weightMode)},
                        {"at", k.arriveTangent},
                        {"lt", k.leaveTangent},
                        {"aw", k.arriveWeight},
                        {"lw", k.leaveWeight}});
    }
    return json{{"keys", std::move(keys)},
                {"pre", static_cast<int>(c.preExtrap)},
                {"post", static_cast<int>(c.postExtrap)},
                {"def", c.defaultValue}};
}
curve::Curve DeCurve(const json& j) {
    curve::Curve c;
    if (!j.is_object()) return c;
    c.preExtrap = static_cast<curve::Extrap>(std::clamp(j.value("pre", 0), 0, 4));
    c.postExtrap = static_cast<curve::Extrap>(std::clamp(j.value("post", 0), 0, 4));
    c.defaultValue = j.value("def", 0.0f);
    if (auto it = j.find("keys"); it != j.end() && it->is_array()) {
        for (const json& jk : *it) {
            curve::Keyframe k;
            k.time = jk.value("t", 0.0f);
            k.value = jk.value("v", 0.0f);
            k.interp = static_cast<curve::Interp>(std::clamp(jk.value("i", 2), 0, 2));
            k.tangentMode = static_cast<curve::Tangent>(std::clamp(jk.value("tm", 0), 0, 4));
            k.weightMode = static_cast<curve::WeightMode>(std::clamp(jk.value("wm", 0), 0, 3));
            k.arriveTangent = jk.value("at", 0.0f);
            k.leaveTangent = jk.value("lt", 0.0f);
            k.arriveWeight = jk.value("aw", 0.333f);
            k.leaveWeight = jk.value("lw", 0.333f);
            c.keys.push_back(k);
        }
    }
    return c;
}

// ---- channels / rotation keys ----
json SerChannel(const Channel& ch) { return json{{"target", ch.target}, {"curve", SerCurve(ch.curve)}}; }
Channel DeChannel(const json& j) {
    Channel ch;
    ch.target = j.value("target", "");
    if (auto it = j.find("curve"); it != j.end()) ch.curve = DeCurve(*it);
    return ch;
}
json SerQuatKey(const QuatKey& q) {
    return json{{"t", q.time}, {"r", V4(q.value)}, {"e", static_cast<int>(q.ease)}};
}
QuatKey DeQuatKey(const json& j) {
    QuatKey q;
    q.time = j.value("t", 0.0f);
    q.value = RdQ(j.value("r", json()), glm::quat(1, 0, 0, 0));
    q.ease = static_cast<ease::Curve>(std::clamp(j.value("e", 0), 0, 7));
    return q;
}

// ---- section ----
json SerSection(const Section& s) {
    json channels = json::array();
    for (const auto& ch : s.channels) channels.push_back(SerChannel(ch));
    json rot = json::array();
    for (const auto& q : s.rotationKeys) rot.push_back(SerQuatKey(q));
    return json{{"id", HexU64(s.id)},
                {"start", s.start},
                {"duration", s.duration},
                {"timeScale", s.timeScale},
                {"innerStart", s.innerStart},
                {"loop", s.loop},
                {"blendIn", s.blendIn},
                {"blendOut", s.blendOut},
                {"blendEase", static_cast<int>(s.blendEase)},
                {"row", s.row},
                {"kind", static_cast<int>(s.kind)},
                {"channels", std::move(channels)},
                {"rot", std::move(rot)},
                {"assetRef", s.assetRef},
                {"bindingRef", s.bindingRef},
                {"fparams", SerParamsF(s.floatParams)},
                {"sparams", SerParamsS(s.stringParams)}};
}
Section DeSection(const json& j) {
    Section s;
    s.id = RdHexU64(j.value("id", json()));
    s.start = j.value("start", 0.0f);
    s.duration = j.value("duration", 1.0f);
    s.timeScale = j.value("timeScale", 1.0f);
    s.innerStart = j.value("innerStart", 0.0f);
    s.loop = j.value("loop", false);
    s.blendIn = j.value("blendIn", 0.0f);
    s.blendOut = j.value("blendOut", 0.0f);
    s.blendEase = static_cast<ease::Curve>(std::clamp(j.value("blendEase", 6), 0, 7));
    s.row = j.value("row", 0);
    s.kind = static_cast<SectionKind>(std::clamp(j.value("kind", 0), 0, 4));
    if (auto it = j.find("channels"); it != j.end() && it->is_array())
        for (const json& jc : *it) s.channels.push_back(DeChannel(jc));
    if (auto it = j.find("rot"); it != j.end() && it->is_array())
        for (const json& jq : *it) s.rotationKeys.push_back(DeQuatKey(jq));
    s.assetRef = j.value("assetRef", "");
    s.bindingRef = j.value("bindingRef", -1);
    if (auto it = j.find("fparams"); it != j.end()) RdParamsF(*it, s.floatParams);
    if (auto it = j.find("sparams"); it != j.end()) RdParamsS(*it, s.stringParams);
    return s;
}

// ---- track (recursive) ----
json SerTrack(const Track& t) {
    json sections = json::array();
    for (const auto& s : t.sections) sections.push_back(SerSection(s));
    json children = json::array();
    for (const auto& c : t.children) children.push_back(SerTrack(c));
    return json{{"id", HexU64(t.id)},
                {"kind", t.kind},
                {"name", t.name},
                {"binding", t.binding},
                {"mute", t.mute},
                {"solo", t.solo},
                {"locked", t.locked},
                {"sections", std::move(sections)},
                {"children", std::move(children)},
                {"fparams", SerParamsF(t.floatParams)},
                {"sparams", SerParamsS(t.stringParams)}};
}
Track DeTrack(const json& j) {
    Track t;
    t.id = RdHexU64(j.value("id", json()));
    t.kind = j.value("kind", "");
    t.name = j.value("name", "");
    t.binding = j.value("binding", -1);
    t.mute = j.value("mute", false);
    t.solo = j.value("solo", false);
    t.locked = j.value("locked", false);
    if (auto it = j.find("sections"); it != j.end() && it->is_array())
        for (const json& js : *it) t.sections.push_back(DeSection(js));
    if (auto it = j.find("children"); it != j.end() && it->is_array())
        for (const json& jc : *it) t.children.push_back(DeTrack(jc));
    if (auto it = j.find("fparams"); it != j.end()) RdParamsF(*it, t.floatParams);
    if (auto it = j.find("sparams"); it != j.end()) RdParamsS(*it, t.stringParams);
    return t;
}

// ---- binding / marker / shot ----
json SerBinding(const Binding& b) {
    return json{{"id", b.id},
                {"label", b.label},
                {"kind", static_cast<int>(b.kind)},
                {"guid", HexU64(b.guid)},
                {"name", b.name},
                {"spawnAsset", b.spawnAsset}};
}
Binding DeBinding(const json& j) {
    Binding b;
    b.id = j.value("id", 0);
    b.label = j.value("label", "");
    b.kind = static_cast<BindingKind>(std::clamp(j.value("kind", 0), 0, 1));
    b.guid = RdHexU64(j.value("guid", json()));
    b.name = j.value("name", "");
    b.spawnAsset = j.value("spawnAsset", "");
    return b;
}
json SerMarker(const Marker& m) {
    return json{{"time", m.time},   {"name", m.name}, {"category", m.category},
                {"note", m.note},   {"color", m.color}, {"jump", m.jump}};
}
Marker DeMarker(const json& j) {
    Marker m;
    m.time = j.value("time", 0.0f);
    m.name = j.value("name", "");
    m.category = j.value("category", "");
    m.note = j.value("note", "");
    m.color = j.value("color", 0xFFFFFFFFu);
    m.jump = j.value("jump", true);
    return m;
}
json SerShot(const Shot& s) {
    return json{{"id", HexU64(s.id)}, {"shotId", s.shotId},   {"sequence", s.sequence},
                {"start", s.start},   {"duration", s.duration}, {"innerStart", s.innerStart},
                {"timeScale", s.timeScale}, {"note", s.note}, {"take", s.take},
                {"enabled", s.enabled}};
}
Shot DeShot(const json& j) {
    Shot s;
    s.id = RdHexU64(j.value("id", json()));
    s.shotId = j.value("shotId", "");
    s.sequence = j.value("sequence", "");
    s.start = j.value("start", 0.0f);
    s.duration = j.value("duration", 5.0f);
    s.innerStart = j.value("innerStart", 0.0f);
    s.timeScale = j.value("timeScale", 1.0f);
    s.note = j.value("note", "");
    s.take = j.value("take", "");
    s.enabled = j.value("enabled", true);
    return s;
}

json Build(const Sequence& seq) {
    json j;
    j["type"] = "sequence";
    j["version"] = kSequenceVersion;
    j["name"] = seq.name;
    j["guid"] = HexU64(seq.guid);
    j["frameRate"] = seq.frameRate;
    j["playStart"] = seq.playStart;
    j["duration"] = seq.duration;
    j["suppressGameplay"] = seq.suppressGameplay;
    json bindings = json::array();
    for (const auto& b : seq.bindings) bindings.push_back(SerBinding(b));
    j["bindings"] = std::move(bindings);
    json tracks = json::array();
    for (const auto& t : seq.tracks) tracks.push_back(SerTrack(t));
    j["tracks"] = std::move(tracks);
    json markers = json::array();
    for (const auto& m : seq.markers) markers.push_back(SerMarker(m));
    j["markers"] = std::move(markers);
    json shots = json::array();
    for (const auto& s : seq.shots) shots.push_back(SerShot(s));
    j["shots"] = std::move(shots);
    return j;
}

Sequence Parse(const json& j) {
    Sequence seq;
    seq.version = j.value("version", kSequenceVersion);
    seq.name = j.value("name", "");
    seq.guid = RdHexU64(j.value("guid", json()));
    seq.frameRate = j.value("frameRate", 30.0);
    seq.playStart = j.value("playStart", 0.0f);
    seq.duration = j.value("duration", 10.0f);
    seq.suppressGameplay = j.value("suppressGameplay", true);
    if (auto it = j.find("bindings"); it != j.end() && it->is_array())
        for (const json& jb : *it) seq.bindings.push_back(DeBinding(jb));
    if (auto it = j.find("tracks"); it != j.end() && it->is_array())
        for (const json& jt : *it) seq.tracks.push_back(DeTrack(jt));
    if (auto it = j.find("markers"); it != j.end() && it->is_array())
        for (const json& jm : *it) seq.markers.push_back(DeMarker(jm));
    if (auto it = j.find("shots"); it != j.end() && it->is_array())
        for (const json& js : *it) seq.shots.push_back(DeShot(js));
    return seq;
}

} // namespace

std::string ToJson(const Sequence& seq) { return Build(seq).dump(2); }

std::optional<Sequence> FromJson(const std::string& text, const std::string& sourceLabel) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("Sequence: failed to parse '{}': {}", sourceLabel, e.what());
        return std::nullopt;
    }
    return Parse(j);
}

bool SaveSequence(const std::filesystem::path& path, const Sequence& seq) {
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Sequence: cannot write '{}'.", path.string());
        return false;
    }
    out << Build(seq).dump(2);
    return true;
}

std::optional<Sequence> LoadSequence(const std::filesystem::path& path) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Sequence: cannot read '{}'.", path.string());
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Sequence: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }
    return Parse(j);
}

} // namespace hbe::cine

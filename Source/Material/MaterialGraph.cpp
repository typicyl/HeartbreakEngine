// Material/MaterialGraph.cpp - see MaterialGraph.h.
#include "Material/MaterialGraph.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace hbe::mat {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---- Node catalog (the ONE source of truth) ---------------------------------------------
const std::vector<NodeInfo>& NodeCatalog() {
    // name, inputCount, contextDependent, category. Order matches the NodeType enum so a
    // self-test can assert kNodeCatalog[i].type == NodeType(i) (lockstep).
    static const std::vector<NodeInfo> kCatalog = {
        {NodeType::Constant,       "Constant",       0, false, "Input"},
        {NodeType::Color,          "Color",          0, false, "Input"},
        {NodeType::Float,          "Float",          0, false, "Input"},
        {NodeType::Vector,         "Vector",         0, false, "Input"},
        {NodeType::Texture,        "Texture",        1, true,  "Input"},
        {NodeType::NormalMap,      "NormalMap",      1, true,  "Input"},
        {NodeType::Multiply,       "Multiply",       2, false, "Math"},
        {NodeType::Add,            "Add",            2, false, "Math"},
        {NodeType::Subtract,       "Subtract",       2, false, "Math"},
        {NodeType::Divide,         "Divide",         2, false, "Math"},
        {NodeType::Lerp,           "Lerp",           3, false, "Math"},
        {NodeType::Clamp,          "Clamp",          1, false, "Math"},
        {NodeType::Remap,          "Remap",          1, false, "Math"},
        {NodeType::Power,          "Power",          1, false, "Math"},
        {NodeType::Smoothstep,     "Smoothstep",     1, false, "Math"},
        {NodeType::OneMinus,       "OneMinus",       1, false, "Math"},
        {NodeType::Noise,          "Noise",          1, true,  "Procedural"},
        {NodeType::Voronoi,        "Voronoi",        1, true,  "Procedural"},
        {NodeType::Gradient,       "Gradient",       1, true,  "Procedural"},
        {NodeType::ColorRamp,      "ColorRamp",      1, false, "Procedural"},
        {NodeType::UV,             "UV",             0, true,  "Coordinate"},
        {NodeType::WorldPosition,  "WorldPosition",  0, true,  "Coordinate"},
        {NodeType::ObjectPosition, "ObjectPosition", 0, true,  "Coordinate"},
        {NodeType::Normal,         "Normal",         0, true,  "Coordinate"},
        {NodeType::VertexColor,    "VertexColor",    0, true,  "Coordinate"},
        {NodeType::Height,         "Height",         1, true,  "Input"},
        {NodeType::Mask,           "Mask",           1, true,  "Input"},
        {NodeType::MaterialLayer,  "MaterialLayer",  0, true,  "Layer"},
        {NodeType::Output,         "Output",         kChannelCount, false, "Output"},
        // --- Coordinate transforms ---
        {NodeType::Transform,      "Transform",      1, true,  "Transform"},
        {NodeType::Tile,           "Tile",           1, true,  "Transform"},
        {NodeType::Mirror,         "Mirror",         1, true,  "Transform"},
        {NodeType::Warp,           "Warp",           2, true,  "Transform"},
        {NodeType::Kaleidoscope,   "Kaleidoscope",   1, true,  "Transform"},
        // --- Generators ---
        {NodeType::Perlin,         "Perlin",         1, true,  "Generator"},
        {NodeType::FractalNoise,   "FractalNoise",   1, true,  "Generator"},
        {NodeType::Cellular,       "Cellular",       1, true,  "Generator"},
        {NodeType::Checker,        "Checker",        1, true,  "Generator"},
        {NodeType::Bricks,         "Bricks",         1, true,  "Generator"},
        {NodeType::Grid,           "Grid",           1, true,  "Generator"},
        {NodeType::Shape,          "Shape",          1, true,  "Generator"},
        {NodeType::Wave,           "Wave",           1, true,  "Generator"},
        {NodeType::Dots,           "Dots",           1, true,  "Generator"},
        {NodeType::RadialGradient, "RadialGradient", 1, true,  "Generator"},
        {NodeType::AngularGradient,"AngularGradient",1, true,  "Generator"},
        // --- Filters ---
        {NodeType::Blend,          "Blend",          2, false, "Filter"},
        {NodeType::HSV,            "HSV",            1, false, "Filter"},
        {NodeType::BrightnessContrast, "BrightnessContrast", 1, false, "Filter"},
        {NodeType::Levels,         "Levels",         1, false, "Filter"},
        {NodeType::Gamma,          "Gamma",          1, false, "Filter"},
        {NodeType::Posterize,      "Posterize",      1, false, "Filter"},
        {NodeType::Threshold,      "Threshold",      1, false, "Filter"},
        {NodeType::Grayscale,      "Grayscale",      1, false, "Filter"},
        {NodeType::Combine,        "Combine",        4, false, "Filter"},
        {NodeType::Swizzle,        "Swizzle",        1, false, "Filter"},
        // --- Resampling filters ---
        {NodeType::HeightToNormal, "HeightToNormal", 1, true,  "Filter"},
        {NodeType::AmbientOcclusion, "AmbientOcclusion", 1, true, "Filter"},
        {NodeType::Blur,           "Blur",           1, true,  "Filter"},
        {NodeType::Emboss,         "Emboss",         1, true,  "Filter"},
        // --- SDF ---
        {NodeType::SdfCircle,      "SdfCircle",      1, true,  "SDF"},
        {NodeType::SdfBox,         "SdfBox",         1, true,  "SDF"},
        {NodeType::SdfOp,          "SdfOp",          2, false, "SDF"},
        {NodeType::SdfShow,        "SdfShow",        1, false, "SDF"},
    };
    return kCatalog;
}

const NodeInfo& NodeInfoOf(NodeType t) {
    const auto& cat = NodeCatalog();
    const auto idx = static_cast<usize>(t);
    if (idx < cat.size() && cat[idx].type == t) return cat[idx];
    // Fallback linear search (defends against any future enum/table drift).
    for (const auto& n : cat)
        if (n.type == t) return n;
    return cat.front();
}

std::optional<NodeType> NodeTypeFromName(const std::string& name) {
    for (const auto& n : NodeCatalog())
        if (name == n.name) return n.type;
    return std::nullopt;
}

// ---- Graph authoring helpers ------------------------------------------------------------
u32 Graph::AddNode(NodeType type, glm::vec2 uiPos) {
    Node n;
    n.id = nextId++;
    n.type = type;
    n.uiPos = uiPos;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}
Node* Graph::FindNode(u32 id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Node* Graph::FindNode(u32 id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
void Graph::RemoveNode(u32 id) {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == id; }),
                nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const Link& l) { return l.fromNode == id || l.toNode == id; }),
                links.end());
}
void Graph::Connect(u32 fromNode, u32 toNode, u8 toPin, u8 fromPin) {
    if (!FindNode(fromNode) || !FindNode(toNode)) return;
    Disconnect(toNode, toPin); // an input pin holds exactly one source
    links.push_back(Link{fromNode, fromPin, toNode, toPin});
}
void Graph::Disconnect(u32 toNode, u8 toPin) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const Link& l) { return l.toNode == toNode && l.toPin == toPin; }),
                links.end());
}
const Link* Graph::LinkInto(u32 toNode, u8 toPin) const {
    for (const auto& l : links)
        if (l.toNode == toNode && l.toPin == toPin) return &l;
    return nullptr;
}
const Node* Graph::OutputNode() const {
    const Node* found = nullptr;
    for (const auto& n : nodes) {
        if (n.type == NodeType::Output) {
            if (found) return nullptr; // ambiguous
            found = &n;
        }
    }
    return found;
}

// ---- JSON helpers -----------------------------------------------------------------------
namespace {
json V4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
json V2(const glm::vec2& v) { return json::array({v.x, v.y}); }

glm::vec4 GetV4(const json& j, glm::vec4 def) {
    if (!j.is_array() || j.size() < 4) return def;
    try {
        return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
    } catch (...) {
        return def;
    }
}
glm::vec2 GetV2(const json& j, glm::vec2 def) {
    if (!j.is_array() || j.size() < 2) return def;
    try {
        return {j[0].get<f32>(), j[1].get<f32>()};
    } catch (...) {
        return def;
    }
}
template <class T>
T JGet(const json& j, const char* key, const T& def) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

json ParamsToJson(const ParamSet& set) {
    json arr = json::array();
    for (const auto& p : set.params) {
        json jp;
        jp["name"] = p.name;
        jp["type"] = static_cast<int>(p.type);
        jp["value"] = V4(p.value);
        if (!p.texture.empty()) jp["texture"] = p.texture;
        arr.push_back(std::move(jp));
    }
    return arr;
}
void ParamsFromJson(const json& arr, ParamSet& set) {
    set.params.clear();
    if (!arr.is_array()) return;
    for (const auto& jp : arr) {
        if (!jp.is_object()) continue; // skip a malformed param element rather than throw
        Param p;
        p.name = JGet(jp, "name", std::string());
        p.type = static_cast<ParamType>(JGet(jp, "type", 0));
        p.value = GetV4(jp.value("value", json()), glm::vec4(0.0f));
        p.texture = JGet(jp, "texture", std::string());
        if (!p.name.empty()) set.params.push_back(std::move(p));
    }
}

json BuildJson(const Graph& g) {
    json j;
    j["version"] = kMaterialGraphVersion;
    j["name"] = g.name;
    j["nextId"] = g.nextId;
    j["params"] = ParamsToJson(g.params);

    json jn = json::array();
    for (const auto& n : g.nodes) {
        json node;
        node["id"] = n.id;
        node["type"] = NodeInfoOf(n.type).name;
        node["constant"] = V4(n.constant);
        node["space"] = static_cast<int>(n.space);
        if (!n.paramName.empty()) node["param"] = n.paramName;
        if (!n.texture.empty()) node["texture"] = n.texture;
        node["ui"] = V2(n.uiPos);
        if (!n.ramp.empty()) {
            json jr = json::array();
            for (const auto& s : n.ramp) {
                json js;
                js["pos"] = s.pos;
                js["color"] = V4(s.color);
                jr.push_back(std::move(js));
            }
            node["ramp"] = std::move(jr);
        }
        jn.push_back(std::move(node));
    }
    j["nodes"] = std::move(jn);

    json jl = json::array();
    for (const auto& l : g.links) {
        json link;
        link["from"] = l.fromNode;
        link["fromPin"] = l.fromPin;
        link["to"] = l.toNode;
        link["toPin"] = l.toPin;
        jl.push_back(std::move(link));
    }
    j["links"] = std::move(jl);
    return j;
}

std::optional<Graph> ParseJson(const json& j) {
    Graph g;
    g.version = JGet(j, "version", 1u);
    g.name = JGet(j, "name", std::string("Material"));
    g.nextId = JGet(j, "nextId", 1u);
    if (const auto it = j.find("params"); it != j.end()) ParamsFromJson(*it, g.params);

    std::unordered_set<u32> validIds;
    if (const auto it = j.find("nodes"); it != j.end() && it->is_array()) {
        for (const auto& node : *it) {
            if (!node.is_object()) continue; // skip a malformed node element rather than throw
            const std::string typeName = JGet(node, "type", std::string());
            const auto nt = NodeTypeFromName(typeName);
            if (!nt) {
                HBE_WARN("MaterialGraph: unknown node type '{}' dropped.", typeName);
                continue; // forward-compat: skip unknown nodes, drop their links below
            }
            Node n;
            n.id = JGet(node, "id", 0u);
            n.type = *nt;
            n.constant = GetV4(node.value("constant", json()), glm::vec4(0.0f));
            n.space = static_cast<Space>(JGet(node, "space", 0));
            n.paramName = JGet(node, "param", std::string());
            n.texture = JGet(node, "texture", std::string());
            n.uiPos = GetV2(node.value("ui", json()), glm::vec2(0.0f));
            if (const auto rit = node.find("ramp"); rit != node.end() && rit->is_array()) {
                for (const auto& js : *rit) {
                    if (!js.is_object()) continue; // skip a malformed ramp stop rather than throw
                    RampStop s;
                    s.pos = JGet(js, "pos", 0.0f);
                    s.color = GetV4(js.value("color", json()), glm::vec4(1.0f));
                    n.ramp.push_back(s);
                }
            }
            if (n.id != 0) {
                validIds.insert(n.id);
                g.nodes.push_back(std::move(n));
            }
        }
    }
    if (const auto it = j.find("links"); it != j.end() && it->is_array()) {
        for (const auto& link : *it) {
            Link l;
            l.fromNode = JGet(link, "from", 0u);
            l.fromPin = static_cast<u8>(JGet(link, "fromPin", 0));
            l.toNode = JGet(link, "to", 0u);
            l.toPin = static_cast<u8>(JGet(link, "toPin", 0));
            // Drop dangling links (endpoint node was unknown/skipped).
            if (validIds.count(l.fromNode) && validIds.count(l.toNode)) g.links.push_back(l);
        }
    }
    // Keep nextId ahead of every id so freshly-added nodes never collide with loaded ones.
    for (const auto& n : g.nodes) g.nextId = std::max(g.nextId, n.id + 1);
    return g;
}
} // namespace

// ---- Public I/O -------------------------------------------------------------------------
std::string GraphToJsonString(const Graph& g) { return BuildJson(g).dump(2); }

std::optional<Graph> GraphFromJsonString(const std::string& str) {
    try {
        // ParseJson is inside the try too: a valid-JSON-but-wrong-shape document (a stray non-object
        // element the guards miss) must return nullopt, not throw out of the loader.
        return ParseJson(json::parse(str));
    } catch (const std::exception& e) {
        HBE_ERROR("MaterialGraph: parse failed: {}", e.what());
        return std::nullopt;
    }
}

bool SaveGraph(const fs::path& path, const Graph& g) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("MaterialGraph: cannot write '{}'.", path.string());
        return false;
    }
    out << GraphToJsonString(g);
    return true;
}

std::optional<Graph> LoadGraph(const fs::path& path) {
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) return std::nullopt;
    std::string str(bytes->begin(), bytes->end());
    return GraphFromJsonString(str);
}

} // namespace hbe::mat

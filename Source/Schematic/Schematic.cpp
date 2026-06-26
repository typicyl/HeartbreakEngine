// Schematic/Schematic.cpp
#include "Schematic/Schematic.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>

namespace hbe::schematic {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* PinTypeName(PinType t) {
    switch (t) {
        case PinType::Exec:   return "exec";
        case PinType::Float:  return "float";
        case PinType::Bool:   return "bool";
        case PinType::Vec3:   return "vec3";
        case PinType::String: return "string";
        case PinType::Entity: return "entity";
    }
    return "?";
}

namespace {
using P = PinType;
// Built once; indexed by NodeType. Keep aligned with the enum order.
std::array<NodeDesc, static_cast<usize>(NodeType::Count)> BuildCatalog() {
    std::array<NodeDesc, static_cast<usize>(NodeType::Count)> c;
    const auto set = [&](NodeType t, NodeDesc d) { c[static_cast<usize>(t)] = std::move(d); };
    const PinDesc EXEC{"", P::Exec};

    set(NodeType::EventStart,  {"On Start", "Events", {}, {EXEC}});
    set(NodeType::EventUpdate, {"On Update", "Events", {}, {EXEC, {"Delta", P::Float}}});
    set(NodeType::Branch,      {"Branch", "Flow", {EXEC, {"Condition", P::Bool}},
                                {{"True", P::Exec}, {"False", P::Exec}}});
    set(NodeType::Sequence,    {"Sequence", "Flow", {EXEC},
                                {{"Then 0", P::Exec}, {"Then 1", P::Exec}}});
    set(NodeType::Print,       {"Print", "Flow", {EXEC, {"Text", P::String}}, {EXEC}});
    set(NodeType::Delay,       {"Delay", "Flow", {EXEC, {"Seconds", P::Float}}, {EXEC}});
    set(NodeType::SetVar,      {"Set Variable", "Variables",
                                {EXEC, {"Name", P::String}, {"Value", P::Float}}, {EXEC}});
    set(NodeType::GetVar,      {"Get Variable", "Variables", {{"Name", P::String}},
                                {{"Value", P::Float}}});
    set(NodeType::LiteralFloat,  {"Float", "Constants", {{"Value", P::Float}}, {{"", P::Float}}});
    set(NodeType::LiteralBool,   {"Bool", "Constants", {{"Value", P::Bool}}, {{"", P::Bool}}});
    set(NodeType::LiteralVec3,   {"Vec3", "Constants",
                                  {{"X", P::Float}, {"Y", P::Float}, {"Z", P::Float}},
                                  {{"", P::Vec3}}});
    set(NodeType::LiteralString, {"String", "Constants", {{"Value", P::String}}, {{"", P::String}}});
    const NodeDesc mathDesc{"", "Math", {{"A", P::Float}, {"B", P::Float}}, {{"Result", P::Float}}};
    set(NodeType::Add,      {"Add", "Math", mathDesc.inputs, mathDesc.outputs});
    set(NodeType::Subtract, {"Subtract", "Math", mathDesc.inputs, mathDesc.outputs});
    set(NodeType::Multiply, {"Multiply", "Math", mathDesc.inputs, mathDesc.outputs});
    set(NodeType::Divide,   {"Divide", "Math", mathDesc.inputs, mathDesc.outputs});
    set(NodeType::Greater,  {"Greater (>)", "Compare", {{"A", P::Float}, {"B", P::Float}}, {{"Result", P::Bool}}});
    set(NodeType::Less,     {"Less (<)", "Compare", {{"A", P::Float}, {"B", P::Float}}, {{"Result", P::Bool}}});
    set(NodeType::EqualF,   {"Equal (=)", "Compare", {{"A", P::Float}, {"B", P::Float}}, {{"Result", P::Bool}}});
    set(NodeType::AndB,     {"And", "Logic", {{"A", P::Bool}, {"B", P::Bool}}, {{"Result", P::Bool}}});
    set(NodeType::OrB,      {"Or", "Logic", {{"A", P::Bool}, {"B", P::Bool}}, {{"Result", P::Bool}}});
    set(NodeType::NotB,     {"Not", "Logic", {{"A", P::Bool}}, {{"Result", P::Bool}}});
    set(NodeType::MakeVec3, {"Make Vec3", "Vec3", {{"X", P::Float}, {"Y", P::Float}, {"Z", P::Float}}, {{"Vec3", P::Vec3}}});
    set(NodeType::BreakVec3, {"Break Vec3", "Vec3", {{"Vec3", P::Vec3}}, {{"X", P::Float}, {"Y", P::Float}, {"Z", P::Float}}});
    set(NodeType::Self,        {"Self", "Entity", {}, {{"Entity", P::Entity}}});
    set(NodeType::GetDeltaTime, {"Delta Time", "Entity", {}, {{"Delta", P::Float}}});
    set(NodeType::KeyDown,     {"Key Down", "Input", {{"Key", P::String}}, {{"Down", P::Bool}}});
    set(NodeType::GetPosition, {"Get Position", "Transform", {{"Entity", P::Entity}}, {{"Pos", P::Vec3}}});
    set(NodeType::SetPosition, {"Set Position", "Transform",
                                {EXEC, {"Entity", P::Entity}, {"Pos", P::Vec3}}, {EXEC}});
    set(NodeType::Translate,   {"Translate", "Transform",
                                {EXEC, {"Entity", P::Entity}, {"Delta", P::Vec3}}, {EXEC}});
    set(NodeType::ReachCheckpoint, {"Reach Checkpoint", "Game",
                                    {EXEC, {"Id", P::String}}, {EXEC}});
    set(NodeType::SetObjective, {"Set Objective", "Game",
                                 {EXEC, {"Id", P::String}, {"Text", P::String}}, {EXEC}});
    set(NodeType::CompleteObjective, {"Complete Objective", "Game",
                                      {EXEC, {"Id", P::String}}, {EXEC}});
    set(NodeType::SetMusicState, {"Set Music State", "Music",
                                  {EXEC, {"State", P::String}}, {EXEC}});
    set(NodeType::SetMusicParam, {"Set Music Param", "Music",
                                  {EXEC, {"Name", P::String}, {"Value", P::Float}}, {EXEC}});
    set(NodeType::PlayStinger, {"Play Stinger", "Music",
                                {EXEC, {"Asset", P::String}}, {EXEC}});
    return c;
}
} // namespace

const NodeDesc& Describe(NodeType t) {
    static const auto catalog = BuildCatalog();
    return catalog[static_cast<usize>(t)];
}

namespace {
// A sensible literal default for a freshly added node's input pin.
Value DefaultLiteral(NodeType t, u32 inPin, PinType pt) {
    if (t == NodeType::KeyDown && inPin == 0) return Value::Str("W");
    if (t == NodeType::LiteralString && inPin == 0) return Value::Str("Hello");
    if (t == NodeType::Print && inPin == 1) return Value::Str("Print");
    if (t == NodeType::SetVar && inPin == 1) return Value::Str("MyVar");
    if (t == NodeType::GetVar && inPin == 0) return Value::Str("MyVar");
    if (t == NodeType::Delay && inPin == 1) return Value::Float(1.0f);
    if (t == NodeType::ReachCheckpoint && inPin == 1) return Value::Str("checkpoint1");
    if (t == NodeType::SetObjective && inPin == 1) return Value::Str("obj1");
    if (t == NodeType::SetObjective && inPin == 2) return Value::Str("Reach the goal");
    if (t == NodeType::CompleteObjective && inPin == 1) return Value::Str("obj1");
    if (t == NodeType::SetMusicState && inPin == 1) return Value::Str("Combat");
    if (t == NodeType::SetMusicParam && inPin == 1) return Value::Str("intensity");
    if (t == NodeType::SetMusicParam && inPin == 2) return Value::Float(1.0f);
    if (t == NodeType::PlayStinger && inPin == 1) return Value::Str("Music/stinger.uaf");
    switch (pt) {
        case PinType::Bool:   return Value::Bool(false);
        case PinType::Vec3:   return Value::Vec3({0.0f, 0.0f, 0.0f});
        case PinType::String: return Value::Str("");
        case PinType::Entity: return Value::Ent(0xFFFFFFFFu);
        default:              return Value::Float(0.0f);
    }
}
} // namespace

u32 Graph::AddNode(NodeType t, glm::vec2 pos) {
    Node n;
    n.id = nextId++;
    n.type = t;
    n.pos = pos;
    const NodeDesc& d = Describe(t);
    n.literals.resize(d.inputs.size());
    for (usize i = 0; i < d.inputs.size(); ++i)
        n.literals[i] = DefaultLiteral(t, static_cast<u32>(i), d.inputs[i].type);
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

Node* Graph::Find(u32 id) {
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Node* Graph::Find(u32 id) const {
    for (const Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

void Graph::RemoveNode(u32 id) {
    for (auto it = links.begin(); it != links.end();)
        it = (it->fromNode == id || it->toNode == id) ? links.erase(it) : std::next(it);
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
        if (it->id == id) { nodes.erase(it); return; }
}

void Graph::RemoveLink(u32 index) {
    if (index < links.size()) links.erase(links.begin() + index);
}

bool Graph::Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin) {
    if (fromNode == toNode) return false;
    const Node* a = Find(fromNode);
    const Node* b = Find(toNode);
    if (!a || !b) return false;
    const NodeDesc& da = Describe(a->type);
    const NodeDesc& db = Describe(b->type);
    if (fromPin >= da.outputs.size() || toPin >= db.inputs.size()) return false;
    const PinType ot = da.outputs[fromPin].type;
    const PinType it = db.inputs[toPin].type;
    if (ot != it) return false; // strict type match (Exec only to Exec, etc.)

    // An input pin takes one wire (data) - exec OUTPUTs also take one (one next
    // node). Replace the conflicting end.
    for (auto lit = links.begin(); lit != links.end();) {
        const bool sameIn = (lit->toNode == toNode && lit->toPin == toPin);
        const bool sameExecOut =
            (ot == PinType::Exec && lit->fromNode == fromNode && lit->fromPin == fromPin);
        lit = (sameIn || sameExecOut) ? links.erase(lit) : std::next(lit);
    }
    links.push_back({fromNode, fromPin, toNode, toPin});
    return true;
}

// --- Serialization ----------------------------------------------------------
namespace {
json ValueToJson(const Value& v) {
    json j;
    j["t"] = static_cast<int>(v.type);
    switch (v.type) {
        case PinType::Bool:   j["b"] = v.b; break;
        case PinType::Vec3:   j["v"] = {v.v3.x, v.v3.y, v.v3.z}; break;
        case PinType::String: j["s"] = v.s; break;
        case PinType::Entity: break; // runtime-only
        default:              j["f"] = v.f; break;
    }
    return j;
}
Value ValueFromJson(const json& j) {
    Value v;
    v.type = static_cast<PinType>(j.value("t", 1));
    switch (v.type) {
        case PinType::Bool:   v.b = j.value("b", false); break;
        case PinType::Vec3:
            if (auto it = j.find("v"); it != j.end() && it->is_array() && it->size() == 3)
                v.v3 = {(*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>()};
            break;
        case PinType::String: v.s = j.value("s", std::string()); break;
        case PinType::Entity: v.entity = 0xFFFFFFFFu; break;
        default:              v.f = j.value("f", 0.0f); break;
    }
    return v;
}
} // namespace

bool SaveGraph(const fs::path& path, const Graph& graph) {
    json j;
    j["version"] = 1;
    j["nextId"] = graph.nextId;
    json& jn = j["nodes"] = json::array();
    for (const Node& n : graph.nodes) {
        json lits = json::array();
        for (const Value& v : n.literals) lits.push_back(ValueToJson(v));
        jn.push_back({{"id", n.id},
                      {"type", static_cast<int>(n.type)},
                      {"pos", {n.pos.x, n.pos.y}},
                      {"literals", std::move(lits)}});
    }
    json& jl = j["links"] = json::array();
    for (const Link& l : graph.links)
        jl.push_back({{"fn", l.fromNode}, {"fp", l.fromPin}, {"tn", l.toNode}, {"tp", l.toPin}});

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Schematic: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(2);
    return true;
}

bool LoadGraph(const fs::path& path, Graph& out) {
    json j;
    if (const auto bytes = vfs::ReadFile(path); bytes && !bytes->empty()) {
        try { j = json::parse(bytes->begin(), bytes->end()); }
        catch (const std::exception& e) {
            HBE_ERROR("Schematic: parse '{}': {}", path.string(), e.what());
            return false;
        }
    } else {
        return false;
    }
    out = Graph{};
    out.nextId = j.value("nextId", 1u);
    for (const json& n : j.value("nodes", json::array())) {
        Node node;
        node.id = n.value("id", 0u);
        node.type = static_cast<NodeType>(n.value("type", 0));
        if (static_cast<u16>(node.type) >= static_cast<u16>(NodeType::Count)) continue;
        if (auto it = n.find("pos"); it != n.end() && it->is_array() && it->size() == 2)
            node.pos = {(*it)[0].get<f32>(), (*it)[1].get<f32>()};
        for (const json& lv : n.value("literals", json::array()))
            node.literals.push_back(ValueFromJson(lv));
        // Pad/trim literals to the current descriptor (catalog may have evolved).
        node.literals.resize(Describe(node.type).inputs.size());
        out.nodes.push_back(std::move(node));
        out.nextId = glm::max(out.nextId, out.nodes.back().id + 1);
    }
    for (const json& l : j.value("links", json::array()))
        out.links.push_back({l.value("fn", 0u), l.value("fp", 0u),
                             l.value("tn", 0u), l.value("tp", 0u)});
    return true;
}

} // namespace hbe::schematic

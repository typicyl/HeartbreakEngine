// Dialogue/DialogueGraph.cpp
#include "Dialogue/DialogueGraph.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::dlg {

using json = nlohmann::json;

// --- Graph methods (mirror the schematic Graph so both editors feel identical) ---

u32 Graph::AddNode(NodeType t, glm::vec2 pos) {
    Node n;
    n.id = nextId++;
    n.type = t;
    n.pos = pos;
    if (t == NodeType::Choice && n.choices.empty()) {
        n.choices.push_back({"Option A", ""});
        n.choices.push_back({"Option B", ""});
    }
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
    for (usize i = 0; i < links.size();) {
        if (links[i].fromNode == id || links[i].toNode == id)
            links.erase(links.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
    for (usize i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id) {
            nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
}

void Graph::RemoveLink(u32 index) {
    if (index < links.size()) links.erase(links.begin() + static_cast<std::ptrdiff_t>(index));
}

bool Graph::Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin) {
    if (fromNode == toNode || fromNode == 0 || toNode == 0) return false;
    const Node* fn = Find(fromNode);
    const Node* tn = Find(toNode);
    if (!fn || !tn) return false;
    if (static_cast<int>(fromPin) >= OutPinCount(*fn)) return false;
    if (tn->type == NodeType::Start) return false; // Start has no input
    // One wire per output pin, and one wire per input pin (replace existing).
    for (usize i = 0; i < links.size();) {
        const Link& l = links[i];
        if ((l.fromNode == fromNode && l.fromPin == fromPin) ||
            (l.toNode == toNode && l.toPin == toPin))
            links.erase(links.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
    links.push_back({fromNode, fromPin, toNode, toPin});
    return true;
}

int Graph::OutPinCount(const Node& n) const {
    switch (n.type) {
        case NodeType::Start: return 1;
        case NodeType::Line: return 1;
        case NodeType::Choice: return static_cast<int>(n.choices.size());
        case NodeType::Condition: return 2; // True, False
        case NodeType::SetFlag: return 1;
        case NodeType::End: return 0;
        default: return 0;
    }
}

u32 Graph::Follow(u32 nodeId, u32 outPin) const {
    for (const Link& l : links)
        if (l.fromNode == nodeId && l.fromPin == outPin) return l.toNode;
    return 0;
}

u32 Graph::StartNode() const {
    for (const Node& n : nodes)
        if (n.type == NodeType::Start) return n.id;
    return nodes.empty() ? 0u : nodes.front().id;
}

const char* NodeTypeName(NodeType t) {
    switch (t) {
        case NodeType::Start: return "Start";
        case NodeType::Line: return "Line";
        case NodeType::Choice: return "Choice";
        case NodeType::Condition: return "Condition";
        case NodeType::SetFlag: return "Set Flag";
        case NodeType::End: return "End";
        default: return "?";
    }
}

// --- Serialization ----------------------------------------------------------

bool SaveGraph(const std::filesystem::path& path, const Graph& g) {
    json j;
    j["type"] = "dialogue";
    j["version"] = 2; // 1 = legacy linear "lines"; 2 = node graph
    j["nextId"] = g.nextId;

    json nodes = json::array();
    for (const Node& n : g.nodes) {
        json jn;
        jn["id"] = n.id;
        jn["type"] = static_cast<u32>(n.type);
        jn["pos"] = {n.pos.x, n.pos.y};
        switch (n.type) {
            case NodeType::Line:
                jn["speaker"] = n.speaker;
                jn["text"] = n.text;
                jn["clip"] = n.clip;
                jn["hold"] = n.hold;
                break;
            case NodeType::Choice: {
                json ch = json::array();
                for (const ChoiceOption& c : n.choices)
                    ch.push_back({{"text", c.text}, {"showIf", c.showIf}});
                jn["choices"] = std::move(ch);
                break;
            }
            case NodeType::Condition:
                jn["flag"] = n.flag;
                jn["op"] = static_cast<u32>(n.op);
                jn["value"] = n.value;
                break;
            case NodeType::SetFlag:
                jn["setFlag"] = n.setFlag;
                jn["setValue"] = n.setValue;
                break;
            default:
                break;
        }
        nodes.push_back(std::move(jn));
    }
    j["nodes"] = std::move(nodes);

    json links = json::array();
    for (const Link& l : g.links)
        links.push_back({{"fn", l.fromNode}, {"fp", l.fromPin}, {"tn", l.toNode}, {"tp", l.toPin}});
    j["links"] = std::move(links);

    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Dialogue: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

namespace {
// Build a Start -> Line -> ... -> End chain from a legacy flat "lines" array so
// pre-graph .hbdialogue assets keep working (and open editably in the new editor).
void BuildLegacyChain(const json& linesArr, Graph& g) {
    const u32 start = g.AddNode(NodeType::Start, {40.0f, 40.0f});
    u32 prev = start;
    u32 prevPin = 0;
    f32 y = 40.0f;
    for (const json& jl : linesArr) {
        const u32 id = g.AddNode(NodeType::Line, {300.0f, y});
        Node* n = g.Find(id);
        n->speaker = jl.value("speaker", "");
        n->text = jl.value("text", "");
        n->clip = jl.value("clip", "");
        n->hold = jl.value("hold", 0.0f);
        g.Connect(prev, prevPin, id, 0);
        prev = id;
        prevPin = 0;
        y += 120.0f;
    }
    const u32 end = g.AddNode(NodeType::End, {560.0f, y});
    g.Connect(prev, prevPin, end, 0);
}
} // namespace

bool LoadGraph(const std::filesystem::path& path, Graph& out) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Dialogue: cannot read '{}'.", path.string());
        return false;
    }
    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Dialogue: failed to parse '{}': {}", path.string(), e.what());
        return false;
    }

    out = Graph{};

    // Legacy linear format (no "nodes"): build a Start->Line->End chain.
    const auto nodesIt = j.find("nodes");
    if (nodesIt == j.end() || !nodesIt->is_array()) {
        if (const auto lit = j.find("lines"); lit != j.end() && lit->is_array())
            BuildLegacyChain(*lit, out);
        else
            out.AddNode(NodeType::Start, {40.0f, 40.0f}); // empty -> a bare Start
        return true;
    }

    // Graph format.
    for (const json& jn : *nodesIt) {
        Node n;
        n.id = jn.value("id", 0u);
        n.type = static_cast<NodeType>(
            glm::clamp(jn.value("type", 0u), 0u, static_cast<u32>(NodeType::Count) - 1u));
        if (const auto pit = jn.find("pos"); pit != jn.end() && pit->is_array() && pit->size() == 2) {
            n.pos.x = (*pit)[0].get<f32>();
            n.pos.y = (*pit)[1].get<f32>();
        }
        n.speaker = jn.value("speaker", "");
        n.text = jn.value("text", "");
        n.clip = jn.value("clip", "");
        n.hold = jn.value("hold", 0.0f);
        if (const auto cit = jn.find("choices"); cit != jn.end() && cit->is_array())
            for (const json& jc : *cit)
                n.choices.push_back({jc.value("text", ""), jc.value("showIf", "")});
        n.flag = jn.value("flag", "");
        n.op = static_cast<CmpOp>(
            glm::clamp(jn.value("op", 0u), 0u, static_cast<u32>(CmpOp::LessEqual)));
        n.value = jn.value("value", 0.0f);
        n.setFlag = jn.value("setFlag", "");
        n.setValue = jn.value("setValue", 1.0f);
        out.nodes.push_back(std::move(n));
    }
    if (const auto lit = j.find("links"); lit != j.end() && lit->is_array())
        for (const json& jl : *lit)
            out.links.push_back({jl.value("fn", 0u), jl.value("fp", 0u), jl.value("tn", 0u),
                                 jl.value("tp", 0u)});

    out.nextId = j.value("nextId", 1u);
    // Guard: nextId must exceed every existing id (older/hand-edited files).
    for (const Node& n : out.nodes) out.nextId = glm::max(out.nextId, n.id + 1u);
    return true;
}

} // namespace hbe::dlg

// Dialogue/DialogueGraph.h - branching-conversation graph (.hbdialogue).
//
// A dialogue is a node graph, authored in the Dialogue Editor (its own window,
// same Blueprints-style look as the Schematic editor) and run by the engine's
// conversation player. Node types:
//   Start     - the entry point (one exec out).
//   Line      - a spoken line (speaker + caption + optional .uaf voiceline);
//               auto-advances after `hold` seconds. One exec out.
//   Choice    - presents 2..N clickable options to the player; each option is a
//               separate exec out taken when the player picks it. An option may
//               be gated by a story flag (`showIf`).
//   Condition - branches on a global story flag (True / False exec outs).
//   SetFlag   - writes a global story flag, then continues (one exec out).
//   End       - terminates the conversation.
//
// All wires are execution flow (there is only one pin type), so the graph is a
// pure control-flow diagram. Legacy linear .hbdialogue files (a flat "lines"
// array, format v1) still load - they become a Start -> Line -> ... -> End chain.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::dlg {

enum class NodeType : u8 { Start = 0, Line, Choice, Condition, SetFlag, End, Count };

// Comparison used by a Condition node against a global story flag's float value.
enum class CmpOp : u8 { NotZero = 0, Equal, NotEqual, Greater, Less, GreaterEqual, LessEqual };

// One selectable option on a Choice node (== one exec output pin, in order).
struct ChoiceOption {
    std::string text;        // button label shown to the player
    std::string showIf;      // optional flag: show this option only if flag != 0 (empty = always)
};

struct Node {
    u32 id = 0;
    NodeType type = NodeType::Line;
    glm::vec2 pos{0.0f};             // editor canvas position
    // Line
    std::string speaker;             // character name ("Speaker: text")
    std::string text;                // caption line (empty = fall back to the clip's baked caption)
    std::string clip;                // optional `.uaf` Voiceline (relative to Assets/)
    f32 hold = 0.0f;                 // seconds before auto-advance (0 = auto from text length)
    // Choice
    std::vector<ChoiceOption> choices;
    // Condition
    std::string flag;                // story flag tested
    CmpOp op = CmpOp::NotZero;
    f32 value = 0.0f;                // compared against the flag's value
    // SetFlag
    std::string setFlag;             // story flag written
    f32 setValue = 1.0f;
};

struct Link {
    u32 fromNode = 0, fromPin = 0;   // source node id + output pin index
    u32 toNode = 0, toPin = 0;       // dest node id + input pin index (always 0)
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    u32 nextId = 1;

    u32 AddNode(NodeType t, glm::vec2 pos);
    Node* Find(u32 id);
    const Node* Find(u32 id) const;
    void RemoveNode(u32 id);         // also drops the node's links
    void RemoveLink(u32 index);
    // Connect an output pin to an input pin. A given output pin drives at most one
    // wire (replaces any existing), and an input accepts at most one wire.
    bool Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin);

    // Exec output-pin count: Start 1, Line 1, Choice = choices.size(), Condition 2
    // (True/False), SetFlag 1, End 0.
    int OutPinCount(const Node& n) const;
    // Target node id reachable from (nodeId, outPin), or 0 if the pin is unconnected.
    u32 Follow(u32 nodeId, u32 outPin) const;
    // Entry point: the first Start node's id, else the first node's id, else 0.
    u32 StartNode() const;
};

// Human-readable node-type name (editor palette + titles).
const char* NodeTypeName(NodeType t);

inline constexpr const char* kDialogueExtension = ".hbdialogue";

bool SaveGraph(const std::filesystem::path& path, const Graph& g);
// Pack-aware (VFS). Accepts both the graph format (v2, "nodes"/"links") and the
// legacy linear format (v1, flat "lines" array -> built into a Start->Line chain).
bool LoadGraph(const std::filesystem::path& path, Graph& out);

} // namespace hbe::dlg

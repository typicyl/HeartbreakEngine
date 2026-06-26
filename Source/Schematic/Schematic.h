// Schematic/Schematic.h - node-graph "visual scripting" data model.
//
// A Schematic is a Blueprints-style node graph: EVENT nodes (OnStart/OnUpdate)
// fire an EXECUTION chain; pure DATA nodes feed values along data wires. Each
// node's type is described by a static NodeDesc (its pins); a graph stores node
// instances (type + canvas position + per-input literal defaults) and links.
// The interpreter (SchematicVM) runs it against an entity each frame; the editor
// (Editor::DrawSchematicEditor) edits it; both read the same catalog.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::schematic {

// Pin/value kinds. `Exec` pins are the white execution wire; the rest carry data.
enum class PinType : u8 { Exec, Float, Bool, Vec3, String, Entity };

// A tagged value flowing along a data wire (also a node's literal default).
struct Value {
    PinType type = PinType::Float;
    f32 f = 0.0f;
    bool b = false;
    glm::vec3 v3{0.0f};
    std::string s;
    u32 entity = 0xFFFFFFFFu; // entt::entity bits (invalid = none)

    static Value Float(f32 x) { Value v; v.type = PinType::Float; v.f = x; return v; }
    static Value Bool(bool x) { Value v; v.type = PinType::Bool; v.b = x; return v; }
    static Value Vec3(glm::vec3 x) { Value v; v.type = PinType::Vec3; v.v3 = x; return v; }
    static Value Str(std::string x) { Value v; v.type = PinType::String; v.s = std::move(x); return v; }
    static Value Ent(u32 e) { Value v; v.type = PinType::Entity; v.entity = e; return v; }
};

// Every node kind the engine understands. Keep in sync with Describe().
enum class NodeType : u16 {
    EventStart,    // exec out; fires once when play begins
    EventUpdate,   // exec out + Delta(float); fires every frame
    Branch,        // exec in + Condition(bool) -> True/False exec
    Sequence,      // exec in -> Then0, Then1 (run in order)
    Print,         // exec in + Text(string) -> exec (logs)
    Delay,         // exec in + Seconds(float) -> exec (per-instance timer)
    SetVar,        // exec in + Name(string) + Value(float) -> exec
    GetVar,        // Name(string) -> Value(float)   (pure)
    LiteralFloat,  // Value(float) -> float (pure)
    LiteralBool,   // Value(bool) -> bool   (pure)
    LiteralVec3,   // X,Y,Z(float) -> Vec3  (pure)
    LiteralString, // Value(string) -> string (pure)
    Add, Subtract, Multiply, Divide,        // float math (pure)
    Greater, Less, EqualF,                   // float compare -> bool (pure)
    AndB, OrB, NotB,                         // logic (pure)
    MakeVec3, BreakVec3,                     // vec3 (pure)
    Self,                                    // -> Entity (this entity) (pure)
    GetDeltaTime,                            // -> float (pure)
    KeyDown,                                 // Key(string) -> bool (pure)
    GetPosition,                             // Entity -> Vec3 (pure)
    SetPosition,                             // exec in + Entity + Pos(vec3) -> exec
    Translate,                               // exec in + Entity + Delta(vec3) -> exec
    ReachCheckpoint,                         // exec in + Id(string) -> exec (saves)
    SetObjective,                            // exec in + Id + Text(string) -> exec
    CompleteObjective,                       // exec in + Id(string) -> exec
    Count
};

struct PinDesc {
    const char* name;
    PinType type;
};

// Static description of a node kind: its display name, palette category, and pins.
// Input/output pin index 0 may be an Exec pin (flow); the interpreter + editor key
// links off these indices.
struct NodeDesc {
    const char* name;
    const char* category;
    std::vector<PinDesc> inputs;
    std::vector<PinDesc> outputs;
};

const NodeDesc& Describe(NodeType t);
const char* PinTypeName(PinType t);

// One node instance in a graph.
struct Node {
    u32 id = 0;
    NodeType type = NodeType::EventUpdate;
    glm::vec2 pos{0.0f, 0.0f};       // editor canvas position
    std::vector<Value> literals;     // per-input default (index = input pin)
};

// A wire from an output pin to an input pin (pin indices into the node's desc).
struct Link {
    u32 fromNode = 0, fromPin = 0;
    u32 toNode = 0, toPin = 0;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    u32 nextId = 1;

    u32 AddNode(NodeType t, glm::vec2 pos);  // initializes literals from the desc
    Node* Find(u32 id);
    const Node* Find(u32 id) const;
    void RemoveNode(u32 id);                  // also drops its links
    void RemoveLink(u32 index);
    // Connects out->in, replacing any existing link into the input pin (data inputs
    // take one wire) and rejecting type-incompatible / cyclic exec connections.
    bool Connect(u32 fromNode, u32 fromPin, u32 toNode, u32 toPin);
};

// .hbschem JSON load/save (pack-aware load via the VFS).
bool LoadGraph(const std::filesystem::path& path, Graph& out);
bool SaveGraph(const std::filesystem::path& path, const Graph& graph);

} // namespace hbe::schematic

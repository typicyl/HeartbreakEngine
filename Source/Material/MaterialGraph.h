// Material/MaterialGraph.h - the node-graph model for the Material Maker (.hbmatgraph).
//
// This is the SOURCE / authoring representation (Substance-Designer / Material-Maker style).
// It is NOT interpreted node-by-node at runtime: MaterialGraphCompiler turns a Graph into a
// flat, topologically-ordered op list (CompiledGraph) that constant-folds to hbe::SurfaceParams
// where possible and bakes the rest offline. See docs/Design-MaterialAuthoring.md.
//
// Design rules honoured here:
//   * Stable u32 node ids, never pointers (serialization-safe, versioned).
//   * A single NodeType catalog (name + input arity + context-dependence) is the ONE source of
//     truth the compiler, the editor add-node menu, and the self-test all read (lockstep).
//   * Deterministic JSON I/O (vector order preserved; no hash-container iteration).
#pragma once

#include "Core/Types.h"
#include "Material/MaterialCore.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe::mat {

// Every node type the Material Maker supports (the request's node list + Output). Append-only:
// new types go at the end so serialized graphs keep loading. Order is mirrored in kNodeCatalog.
enum class NodeType : u8 {
    Constant = 0, // scalar/vec literal (constant.x..w), 0 inputs
    Color,        // color literal (constant.rgba)
    Float,        // scalar literal (constant.x)
    Vector,       // vec3/vec4 literal (constant.xyzw)
    Texture,      // sample a 2D texture by uv (external)
    NormalMap,    // sample a normal map (unpack *2-1), external
    Multiply,     // a * b (component-wise)
    Add,          // a + b
    Subtract,     // a - b
    Divide,       // a / b (guarded)
    Lerp,         // mix(a, b, t)
    Clamp,        // clamp(x, constant.x, constant.y)
    Remap,        // remap x from [constant.x,constant.y] to [constant.z,constant.w]
    Power,        // pow(x, constant.x)
    Smoothstep,   // smoothstep(constant.x, constant.y, x)
    OneMinus,     // 1 - x
    Noise,        // value noise at coord*constant.x (+seed constant.y), scalar
    Voronoi,      // voronoi F1 at coord*constant.x, scalar
    Gradient,     // linear gradient along coord (t), scalar
    ColorRamp,    // sample the node's ramp stops by t
    UV,           // ctx.uv0 -> (u,v,0,0)  [context]
    WorldPosition,// ctx.worldPos          [context]
    ObjectPosition,// ctx.objectPos        [context]
    Normal,       // ctx.normal            [context]
    VertexColor,  // ctx.vertexColor       [context]
    Height,       // sample a height map (external), scalar
    Mask,         // a named mask input (external), scalar
    MaterialLayer,// reference to another material (paramName), external
    Output,       // the 8 surface channels (8 inputs)
    // === Material-Maker-class library (append-only). Coordinate-transform + resampling nodes are
    // context-dependent and re-evaluate their input at a MODIFIED uv (the resamplable-function
    // model); see MaterialGraphCompiler.cpp. constant.xyzw meanings are documented there. ===
    // --- Coordinate transforms (resample input at a modified UV) ---
    Transform,      // translate(x,y) / rotate(z turns) / scale(w) the UV before sampling input
    Tile,           // repeat: uv = frac(uv * (x,y))
    Mirror,         // mirror uv (x: 0=X 1=Y 2=both)
    Warp,           // uv += (offsetField-0.5)*2*amount, then sample source (2 inputs)
    Kaleidoscope,   // radial mirror into `count` (x) segments
    // --- Generators (procedural patterns; optional coord input, else ctx uv) ---
    Perlin,         // gradient (Perlin) noise; x=scale y=seed
    FractalNoise,   // FBM over Perlin; x=scale y=octaves z=persistence w=seed
    Cellular,       // Worley; x=scale y=seed z=mode(0 F1,1 F2,2 F2-F1,3 cell)
    Checker,        // checkerboard; x=tiles
    Bricks,         // brick pattern; x=cols y=rows z=mortar w=rowOffset
    Grid,           // grid lines; x=tiles y=lineWidth
    Shape,          // circle/polygon fill; x=sides(0=circle) y=radius z=roundness
    Wave,           // 1D wave along u; x=freq y=type(0 sine,1 tri,2 saw,3 square) z=phase
    Dots,           // dot grid; x=tiles y=radius
    RadialGradient, // radial gradient from center; x=radius
    AngularGradient,// angular (sweep) gradient
    // --- Filters (per-pixel) ---
    Blend,          // a,b + x=mode y=opacity (Substance-style blend modes)
    HSV,            // hue shift(x)/sat mul(y)/val mul(z)
    BrightnessContrast, // x=brightness y=contrast
    Levels,         // x=inLow y=inHigh z=outLow w=outHigh
    Gamma,          // x=gamma
    Posterize,      // x=levels
    Threshold,      // x=threshold
    Grayscale,      // luminance
    Combine,        // r,g,b,a (4 inputs) -> rgba
    Swizzle,        // x=channel(0 r,1 g,2 b,3 a,4 rgb->gray broadcast)
    // --- Resampling filters (sample input at neighbouring UVs) ---
    HeightToNormal, // x=strength y=epsilon -> tangent-space normal
    AmbientOcclusion, // x=strength y=radius (from height)
    Blur,           // x=radius (box blur, 9-tap)
    Emboss,         // x=strength (directional derivative)
    // --- SDF (2D signed distance fields) ---
    SdfCircle,      // x=radius -> signed distance (centered)
    SdfBox,         // x,y=half size -> signed distance
    SdfOp,          // a,b + x=mode(0 union,1 sub,2 intersect,3 smooth-union) y=smooth
    SdfShow,        // distance -> grayscale fill; x=width (antialias)
    Count
};

// The 8 authored surface channels = the input pins of the Output node.
enum class Channel : u8 {
    BaseColor = 0,
    Roughness = 1,
    Metallic = 2,
    Normal = 3,
    Height = 4,
    AO = 5,
    Emissive = 6,
    Opacity = 7,
    Count = 8
};
inline constexpr u32 kChannelCount = static_cast<u32>(Channel::Count);

// Catalog row - the single source of truth about a node type's shape.
struct NodeInfo {
    NodeType type;
    const char* name;      // stable id used in JSON + the add-node menu
    u8 inputCount;         // number of input pins
    bool contextDependent; // reads SampleContext or an external texture -> NOT constant-foldable
    const char* category;  // editor grouping
};

const std::vector<NodeInfo>& NodeCatalog();
const NodeInfo& NodeInfoOf(NodeType t);
// Parse a node-type name (JSON) back to the enum; nullopt if unknown (forward-compat guard).
std::optional<NodeType> NodeTypeFromName(const std::string& name);

// One colour stop in a ColorRamp node.
struct RampStop {
    f32 pos = 0.0f;         // [0,1]
    glm::vec4 color{1.0f};
};

// A graph node. `constant` carries literals/params whose meaning depends on `type` (documented
// per NodeType above). `uiPos` is editor-only (serialized, ignored by the compiler).
struct Node {
    u32 id = 0;
    NodeType type = NodeType::Constant;
    glm::vec4 constant{0.0f};
    Space space = Space::UV0;         // sampling space for Texture/Noise/Voronoi/Gradient
    std::string paramName;            // binds Constant/etc. to an exposed Param, or a Mask/Layer name
    std::string texture;              // texture ref for Texture/NormalMap/Height
    std::vector<RampStop> ramp;       // ColorRamp stops (sorted by pos on compile)
    glm::vec2 uiPos{0.0f};            // editor canvas position
};

// A directed connection: fromNode's output pin -> toNode's input pin. Most nodes have one
// output (pin 0); the Output node has kChannelCount input pins.
struct Link {
    u32 fromNode = 0;
    u8 fromPin = 0;
    u32 toNode = 0;
    u8 toPin = 0;
};

// The authoring graph. `nextId` mints stable ids; ids of deleted nodes are never reused within
// a session (keeps links unambiguous). `params` are the exposed material parameters.
struct Graph {
    u32 version = 1;
    std::string name = "Material";
    std::vector<Node> nodes;
    std::vector<Link> links;
    ParamSet params;
    u32 nextId = 1;

    // Authoring helpers (editor + tests).
    u32 AddNode(NodeType type, glm::vec2 uiPos = glm::vec2(0.0f));
    Node* FindNode(u32 id);
    const Node* FindNode(u32 id) const;
    void RemoveNode(u32 id);           // also drops incident links
    // Connect fromNode.pin -> toNode.pin; replaces any existing link on that input pin
    // (an input pin takes exactly one source). No-op if either node is missing.
    void Connect(u32 fromNode, u32 toNode, u8 toPin, u8 fromPin = 0);
    void Disconnect(u32 toNode, u8 toPin);
    // The node feeding an input pin, or nullptr if unconnected.
    const Link* LinkInto(u32 toNode, u8 toPin) const;
    // The single Output node (nullptr if none / more than one).
    const Node* OutputNode() const;
};

inline constexpr const char* kMaterialGraphExtension = ".hbmatgraph";
inline constexpr u32 kMaterialGraphVersion = 1;

// Deterministic JSON I/O. Save writes a stable field order; Load is forward/back tolerant
// (unknown node-type names are dropped with a warning rather than aborting the load, matching
// the .hbmat null/type-tolerant discipline). Both route through vfs on load.
bool SaveGraph(const std::filesystem::path& path, const Graph& g);
std::optional<Graph> LoadGraph(const std::filesystem::path& path);

// Serialize/parse to an in-memory JSON string (used by the editor's undo history + the
// round-trip self-test without touching disk). Deterministic.
std::string GraphToJsonString(const Graph& g);
std::optional<Graph> GraphFromJsonString(const std::string& json);

} // namespace hbe::mat

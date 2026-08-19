// Material/MaterialGraphCompiler.h - compiles a Graph into an optimized native representation.
//
// The request: "The graph must not be interpreted node-by-node at runtime. Material Graph ->
// Compiler -> Optimized Native Material Representation -> Runtime Renderer."
//
// CompiledGraph IS that optimized representation:
//   * The graph is reduced to the subgraph REACHABLE from the Output node (dead nodes dropped).
//   * That subgraph is TOPOLOGICALLY FLATTENED into a linear op list (Op[]), evaluated into a
//     register array in one forward pass - no per-sample graph traversal, no pointer chasing,
//     no virtual dispatch per node.
//   * CONSTANT SUBGRAPHS ARE FOLDED at compile time to literals (a node with no context/texture
//     dependence and all-constant inputs collapses to a value). A graph with only constant value
//     channels folds entirely to hbe::SurfaceParams - zero runtime cost, exactly today's path.
//   * Non-constant channels (texture / procedural / coordinate driven) stay as a tiny fixed op
//     set that is baked to a texture OFFLINE (BAKED mode) - never a runtime node interpreter.
//
// Compile is DETERMINISTIC: the same Graph always yields the same op list and the same Hash(),
// which is what --test-material asserts.
#pragma once

#include "Core/Types.h"
#include "Material/MaterialCore.h"
#include "Material/MaterialGraph.h"
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams (the fold target - the ONE material rep)

#include <functional>
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe::mat {

// Resolves an external texture/mask/height sample during evaluation (offline bake or preview).
// `kind` distinguishes Texture (rgba) / NormalMap (encoded 0..1) / Height / Mask (scalar in .x).
// Return the sampled value; a null provider makes samplers fall back to neutral defaults so a
// graph still evaluates headless deterministically.
using TextureProvider =
    std::function<glm::vec4(const std::string& texture, glm::vec2 uv, NodeType kind)>;

// One flattened operation. `inputReg[k]` indexes the register array (== op order); kNoReg means
// that input pin is unconnected and the op uses its neutral default. When `folded` is true the
// op's value is precomputed in `foldedValue` (constant subgraph) and inputs are ignored.
struct Op {
    static constexpr u16 kNoReg = 0xFFFF;

    NodeType type = NodeType::Constant;
    Space space = Space::UV0;
    glm::vec4 constant{0.0f};
    std::string texture;        // Texture/NormalMap/Height/Mask reference
    std::vector<RampStop> ramp; // ColorRamp stops (sorted by pos)
    u16 inputReg[3] = {kNoReg, kNoReg, kNoReg};
    bool folded = false;
    glm::vec4 foldedValue{0.0f};
};

// The compiled, optimized material. Evaluate with Eval(); fold to the runtime material with
// ToSurfaceParams(). `channelReg[c]` is the register feeding Output channel c, or -1 if the
// channel is unconnected (keeps its neutral default).
struct CompiledGraph {
    bool ok = false;
    std::string error;                 // non-empty when ok == false (cycle / no Output / ...)
    std::vector<Op> ops;               // topo-ordered
    int channelReg[kChannelCount];     // register per Channel, or -1
    bool fullyConstant = false;        // every bound channel folds to a literal
    ParamSet resolvedParams;           // params after overrides (for the editor to display)

    CompiledGraph() {
        for (u32 i = 0; i < kChannelCount; ++i) channelReg[i] = -1;
    }

    // Evaluate all 8 channels at one surface point. Runs the flat op list once.
    SurfaceSample Eval(const SampleContext& ctx, const TextureProvider& tex = {}) const;

    // Fold the constant value-channels (base color / roughness / metallic / emissive / opacity)
    // into the engine's one runtime material struct. For a fully-constant graph this is exact and
    // free at runtime; for a procedural graph it captures the neutral-context constant part (the
    // texture/procedural detail is produced by the offline bake instead).
    SurfaceParams ToSurfaceParams() const;

    // Deterministic content hash of the compiled op list + channel bindings. Equal graphs compile
    // to an equal hash; --test-material uses this for the determinism proof.
    u64 Hash() const;
};

// Compiles a graph (with optional instance parameter overrides applied first). Never throws;
// on a structural error returns a CompiledGraph with ok=false and a human error string.
CompiledGraph Compile(const Graph& g, const std::vector<ParamOverride>& overrides = {});

} // namespace hbe::mat

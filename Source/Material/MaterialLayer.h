// Material/MaterialLayer.h - material LAYERS + MASK SOURCES + the unified RESOLVER.
//
// This is the join point of the whole system (docs/Design-MaterialAuthoring.md):
//
//     base material
//         + Layer[0] (material, mask, blend)      mask <- Paint / Box / Procedural / Constant
//         + Layer[1] ...
//              => Resolve() => one hbe::SurfaceParams (+ resolved normal + height)
//
// A layer NEVER modifies geometry; it contributes a material sample and a WEIGHT. The resolver
// combines them. Blend modes: Linear, Height, Height+Noise. Normals are blended by REORIENTED
// NORMAL MAPPING (RNM), never a raw tangent-space lerp (a documented request requirement).
//
// The CPU resolver here is the REFERENCE implementation: it is what the offline BAKE mode runs to
// composite layers into the paint canvas, and it is the oracle the GPU layered path
// (Shaders/MaterialLayered.hlsli) must match. It is fully headless-testable.
#pragma once

#include "Core/Types.h"
#include "Material/BoxBrush.h"
#include "Material/MaterialCore.h"
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams (the resolver output = the ONE material rep)

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe::mat {

// Where a layer's weight/mask comes from. The three request-mandated producers (Paint, Box,
// Procedural) plus a Constant convenience. All resolve to a scalar weight in [0,1] at a point.
enum class MaskKind : u8 {
    Constant = 0,   // flat weight (`constant`)
    Box = 1,        // spatial weight field from a BoxBrush
    Procedural = 2, // a baked/graph-driven mask sampled from `texture`
    Paint = 3,      // a persistent UV/box paint mask (bound to the paint canvas at runtime)
};

struct MaskSource {
    MaskKind kind = MaskKind::Constant;
    f32 constant = 1.0f;

    BoxBrush box;                    // Box

    MaskTexture texture;             // Procedural bake (and a headless stand-in for a Paint canvas)
    Space textureSpace = Space::UV0; // how to sample `texture`
    bool invert = false;             // 1 - weight

    // Paint binding (resolved by the Scene/PaintSystem integration; inert in the headless core
    // unless a `texture` stand-in is provided).
    u32 paintCanvasId = 0;
    u32 paintLayerId = 0;
    u8 paintChannel = 0;

    // Scalar weight at a surface point. Box uses ctx.worldPos; Procedural/Paint sample `texture`.
    f32 Evaluate(const SampleContext& ctx) const;
    u64 Hash(u64 seed = 1469598103934665603ull) const;
};

enum class BlendMode : u8 { Linear = 0, Height = 1, HeightNoise = 2 };

// One material layer. Carries a resolved material (`surface`) so the reference resolver is
// self-contained; the authoring `material` ref is what the editor edits and the cooker resolves.
struct Layer {
    std::string material;             // .hbmat ref (authoring)
    SurfaceParams surface;            // resolved OpenPBR values for this layer
    glm::vec3 normalTS{0.0f, 0.0f, 1.0f}; // layer's (constant) detail normal contribution
    f32 layerHeight = 0.5f;           // this layer's height scalar (drives Height blend)

    MaskSource mask;
    f32 opacity = 1.0f;               // strength multiplier on the mask
    BlendMode blend = BlendMode::Linear;
    bool contributesHeight = true;    // does this layer's height feed the composited height?
    bool contributesNormal = true;    // does this layer's normal feed the composited normal?
    f32 heightContribution = 1.0f;    // how strongly layerHeight biases the height blend
    f32 noiseAmount = 0.0f;           // HeightNoise: amplitude of the break-up noise
    f32 noiseScale = 8.0f;            // HeightNoise: frequency

    u64 Hash(u64 seed = 1469598103934665603ull) const;
};

// The whole surface = base material + a bottom-to-top layer stack.
struct LayerStack {
    SurfaceParams base;
    glm::vec3 baseNormalTS{0.0f, 0.0f, 1.0f};
    f32 baseHeight = 0.5f;
    std::vector<Layer> layers; // bottom -> top

    u64 Hash() const;
};

// What the resolver produces at one point: the packed material + composited normal + height.
struct ResolvedSurface {
    SurfaceParams surface;
    glm::vec3 normalTS{0.0f, 0.0f, 1.0f};
    f32 height = 0.5f;
};

// Reoriented Normal Mapping blend of two tangent-space normals. `strength` in [0,1] fades the
// detail toward flat. RNM(base, flat) == base; RNM(flat, detail) == detail. Never a raw lerp.
glm::vec3 BlendNormalRNM(const glm::vec3& base, const glm::vec3& detail, f32 strength = 1.0f);

// Lerp the OpenPBR VALUE fields of two materials by t (colors, weights, roughness, metalness,
// emission, coat, transmission ...). Used by the resolver per layer.
SurfaceParams LerpSurface(const SurfaceParams& a, const SurfaceParams& b, f32 t);

// THE resolver: fold the base + layers into one resolved surface at a surface point.
ResolvedSurface Resolve(const LayerStack& stack, const SampleContext& ctx);

// ---- .hbmatlayer serialization (LIVE-mode layer stack; JSON, versioned) ------------------
inline constexpr const char* kLayerStackExtension = ".hbmatlayer";
inline constexpr u32 kLayerStackVersion = 1;

std::string LayerStackToJsonString(const LayerStack& s);
std::optional<LayerStack> LayerStackFromJsonString(const std::string& json);
bool SaveLayerStack(const std::filesystem::path& path, const LayerStack& s);
std::optional<LayerStack> LoadLayerStack(const std::filesystem::path& path);

} // namespace hbe::mat

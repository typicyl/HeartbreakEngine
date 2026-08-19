// Material/MaterialCore.h - shared foundation for the unified material-authoring system.
//
// This module is the backend-agnostic HEART of the material authoring / layering / masking
// pipeline described in docs/Design-MaterialAuthoring.md. It is deliberately free of RHI,
// Scene, and Editor dependencies (it links into BOTH the runtime `hbe` lib and `hbe_editor`)
// so the graph compiler, the layer resolver, and the box-brush weight field are all fully
// unit-testable headless (--test-material) and reusable at runtime.
//
// The resolver's OUTPUT type is the engine's one authoritative material value struct,
// hbe::SurfaceParams (RHI/SurfaceMaterial.h). That is the single seam back into the existing
// OpenPBR renderer - this system never forks the material representation.
//
// Contents here (small shared vocabulary):
//   * SampleContext  - the per-sample coordinate inputs (uv / world / object / normal / vcol)
//   * SurfaceSample  - the 8 authored channels a graph/layer produces before packing
//   * Aabb           - a world axis-aligned box (box-volume culling reuse)
//   * MaskTexture    - a portable single-channel mask (procedural bake / paint mask / tests)
//   * Falloff        - a configurable falloff curve (NOT a hardcoded smoothstep)
//   * Param / ParamSet / ParamOverride - exposed material parameters + instance overrides
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe::mat {

// -----------------------------------------------------------------------------------------
// Coordinate spaces a sampler/graph node can read from. World/Object/Triplanar keep tiling
// SIZE-INDEPENDENT (a 1m tile stays 1m regardless of surface/brush size) because the sampler
// multiplies world/object position by 1/tileMeters, never a brush dimension.
// -----------------------------------------------------------------------------------------
enum class Space : u8 { UV0 = 0, UV1 = 1, Object = 2, World = 3, Triplanar = 4 };

// The coordinate inputs available when evaluating a compiled graph or resolving a layer at
// one surface point. All fields are optional; unfilled ones keep their neutral default.
struct SampleContext {
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec3 worldPos{0.0f};
    glm::vec3 objectPos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // world-space surface normal (triplanar weighting)
    glm::vec4 vertexColor{1.0f};
};

// The 8 authored surface channels (the request's minimum output set). A compiled graph
// evaluates to this; the layer resolver blends these across layers; the packer maps them to
// hbe::SurfaceParams + the paint canvas textures.
struct SurfaceSample {
    glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
    f32       roughness = 0.5f;
    f32       metallic = 0.0f;
    glm::vec3 normalTS{0.0f, 0.0f, 1.0f}; // tangent-space normal (z-up)
    f32       height = 0.5f;              // 0.5 = neutral (matches paint canvas kNeutralHeight)
    f32       ao = 1.0f;
    glm::vec3 emissive{0.0f};
    f32       opacity = 1.0f;
};

// World axis-aligned box. Reused by box-volume culling (Renderer::Frustum::Intersects takes a
// center + half-extent) and by mask spatial buckets.
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 Center() const { return 0.5f * (min + max); }
    glm::vec3 HalfExtent() const { return 0.5f * (max - min); }
    bool Contains(const glm::vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z &&
               p.z <= max.z;
    }
    bool Intersects(const Aabb& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
};

// A portable single-channel mask (row-major, [0..1]). Used for baked procedural masks, for a
// decoupled stand-in of the paint canvas in headless tests, and as the cooked mask payload.
// Sampling is bilinear with wrap. Bytes are LE by construction (f32 vector) -> portable.
struct MaskTexture {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> data; // size == width*height

    bool Valid() const { return width > 0 && height > 0 && data.size() == usize(width) * height; }
    void Resize(u32 w, u32 h, f32 fill = 0.0f) {
        width = w;
        height = h;
        data.assign(usize(w) * h, fill);
    }
    f32 At(u32 x, u32 y) const { return data[usize(y) * width + x]; }
    void Set(u32 x, u32 y, f32 v) { data[usize(y) * width + x] = v; }
    // Bilinear sample with wrap addressing. uv in [0,1) tiles.
    f32 Sample(glm::vec2 uv) const;
};

// -----------------------------------------------------------------------------------------
// Falloff curve. The request: "Do not hardcode a single smoothstep implementation if the
// architecture can support a configurable curve." So the box-brush (and any weight fade)
// picks a curve shape + a gamma. Eval maps t in [0,1] (0 = edge, 1 = center) -> weight [0,1].
// -----------------------------------------------------------------------------------------
enum class FalloffType : u8 {
    Constant = 0,   // hard edge: 1 everywhere inside, 0 outside (t>0 -> 1)
    Linear = 1,     // t
    Smoothstep = 2, // 3t^2-2t^3 (the classic; still available, just not the ONLY option)
    Smootherstep = 3, // 6t^5-15t^4+10t^3
    EaseIn = 4,     // t^2
    EaseOut = 5,    // 1-(1-t)^2
};

struct Falloff {
    FalloffType type = FalloffType::Smoothstep;
    f32 gamma = 1.0f; // final shaping exponent (>0); 1 = no extra shaping

    f32 Eval(f32 t) const;
};

// -----------------------------------------------------------------------------------------
// Exposed material parameters + instance overrides. A material may expose Color / Scalar /
// Vector / Texture / Bool parameters; an instance overrides them WITHOUT duplicating the
// underlying material/graph.
// -----------------------------------------------------------------------------------------
enum class ParamType : u8 { Scalar = 0, Color = 1, Vector = 2, Texture = 3, Bool = 4 };

struct Param {
    std::string name;
    ParamType type = ParamType::Scalar;
    glm::vec4 value{0.0f};  // Scalar uses .x; Color/Vector use xyzw; Bool uses .x!=0
    std::string texture;    // Texture params only (a .uaf ref, relative to Assets/)

    bool AsBool() const { return value.x != 0.0f; }
    f32 AsScalar() const { return value.x; }
};

struct ParamOverride {
    std::string name;
    glm::vec4 value{0.0f};
    std::string texture;
};

struct ParamSet {
    std::vector<Param> params;

    const Param* Find(const std::string& name) const;
    Param* Find(const std::string& name);
    // Returns .x (or the whole vec) for a named scalar/color, else `def`.
    f32 Scalar(const std::string& name, f32 def = 0.0f) const;
    glm::vec4 Vector(const std::string& name, glm::vec4 def = glm::vec4(0.0f)) const;
    std::string Texture(const std::string& name) const;
};

// Applies overrides in place (matched by name; type is preserved). Unknown names are ignored
// so a stale override never invents a parameter.
void ApplyOverrides(ParamSet& set, const std::vector<ParamOverride>& overrides);

// Deterministic FNV-1a helpers reused across the module for content hashing (determinism
// self-tests, dedup keys). Portable (byte-wise over LE encodings).
u64 HashBytes(const void* data, usize size, u64 seed = 1469598103934665603ull);
u64 HashF32(f32 v, u64 seed);
u64 HashStr(const std::string& s, u64 seed);

} // namespace hbe::mat

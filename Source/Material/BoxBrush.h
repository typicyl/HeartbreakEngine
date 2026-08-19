// Material/BoxBrush.h - the Box Brush / material projection volume.
//
// A transformable box that produces a SPATIAL WEIGHT FIELD (EvaluateBrush(worldPos) -> 0..1),
// exactly as the request specifies: "Do NOT make the box directly edit the material. It should
// produce a mask/weight which feeds the material-layer system." The weight feeds a MaskSource
// (Material/MaterialLayer.h), which feeds a Layer.
//
// Tiling is first-class and SIZE-INDEPENDENT: a 1m material tile stays visually 1m whether the
// brush is 1m or 1000m, because ProjectUV multiplies WORLD (or box-local) position by 1/tileMeters
// - never the brush dimensions. World / Local / Triplanar projection are all supported with
// independent X/Y/Z tiling, rotation, and offset.
//
// Culling: Bounds() returns the world AABB so a box volume is cheaply frustum-/distance-culled
// with the renderer's existing Frustum::Intersects(center, halfExtent) and stream::DistanceToBox.
#pragma once

#include "Core/Types.h"
#include "Material/MaterialCore.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace hbe::mat {

// Projection mode for the box's tiled material sampling.
enum class BoxProjection : u8 {
    World = 0,     // tile by world position (size-independent; the environment default)
    Local = 1,     // tile by box-local position (tiling rides with the volume)
    Triplanar = 2, // 3-axis abs(normal) blend by world position (no UV stretch on any face)
};

struct BoxBrush {
    // --- Transform (Position / Rotation / Scale, per the request) ---
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity
    glm::vec3 scale{1.0f};

    // --- Dimensions (Width / Height / Depth) in local units, pre-scale ---
    glm::vec3 size{1.0f}; // full extents; local box spans [-size/2, size/2]

    // --- Weight field ---
    Falloff falloff;             // configurable falloff curve (NOT a hardcoded smoothstep)
    f32 falloffWidth = 0.25f;    // fraction (0..1) of each half-extent over which the edge fades
    f32 strength = 1.0f;         // 0..1 multiplier on the produced weight

    // --- Tiling / projection of the box's material ---
    BoxProjection projection = BoxProjection::World;
    glm::vec3 tileMeters{1.0f};  // metres per tile, independent per axis (X/Y/Z)
    f32 uvRotation = 0.0f;       // radians, applied in the projection plane
    glm::vec2 uvOffset{0.0f};

    // --- Authoring wiring ---
    std::string material;        // the .hbmat the box paints (authoring ref)
    int blendMode = 0;           // mat::BlendMode as int (kept POD-simple for serialization)

    // Weight at a world point: transform into local space, box SDF, falloff curve, strength.
    // 1 = fully inside the core, 0 = outside the box.
    f32 EvaluateBrush(const glm::vec3& worldPos) const;

    // World-space UV for the box's tiled material at a world point (with its surface normal for
    // Triplanar/World dominant-axis selection). Size-independent for World/Triplanar.
    glm::vec2 ProjectUV(const glm::vec3& worldPos, const glm::vec3& normal) const;

    // World AABB enclosing the rotated + scaled box (for spatial culling).
    Aabb Bounds() const;

    // Transform a world point into the box's local (pre-scale, pre-rotate) frame.
    glm::vec3 ToLocal(const glm::vec3& worldPos) const;

    // Content hash (determinism / dedup).
    u64 Hash(u64 seed = 1469598103934665603ull) const;
};

// JSON (de)serialization helpers for the box brush - defined in BoxBrush.cpp. Returns a JSON
// object string fragment; used by the layer-stack serializer and the scene serializer.
std::string BoxBrushToJsonString(const BoxBrush& b);
bool BoxBrushFromJsonString(const std::string& json, BoxBrush& out);

} // namespace hbe::mat

// Assets/MeshGenerator.h - procedural primitives.
#pragma once

#include "Assets/Mesh.h"

#include <string>

namespace hbe::mesh {

// Axis-aligned cube centered at the origin with per-face normals and tangents.
MeshData GenerateCube(f32 size = 1.0f);

// UV sphere centered at the origin. `segments` = longitude, `rings` = latitude.
MeshData GenerateSphere(f32 radius = 0.5f, u32 segments = 64, u32 rings = 32);

// Flat quad in the XZ plane (normal +Y), centered, spanning [-size/2, size/2]
// with `subdivisions` cells per side.
MeshData GeneratePlane(f32 size = 1.0f, u32 subdivisions = 1);

// Capped cylinder along Y, centered, total height `height`, given `radius`.
MeshData GenerateCylinder(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 24);

// Cone along Y (apex up), centered, base `radius`, total height `height`.
MeshData GenerateCone(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 24);

// Capsule along Y (cylinder + two hemispheres), centered. `height` is the TOTAL
// height (so the cylindrical mid-section is height - 2*radius, clamped >= 0).
MeshData GenerateCapsule(f32 radius = 0.5f, f32 height = 2.0f, u32 segments = 24,
                         u32 rings = 8);

// Torus in the XZ plane around +Y. `major` = ring radius, `minor` = tube radius.
MeshData GenerateTorus(f32 major = 0.5f, f32 minor = 0.2f, u32 segments = 32,
                       u32 sides = 16);

// Builds the primitive named by a "prim:<name>" mesh source's <name>
// ("cube" | "sphere" | "plane" | "cylinder" | "cone" | "capsule" | "torus").
// Returns an empty mesh for an unknown name.
MeshData GeneratePrimitive(const std::string& name);

// The primitive names the editor offers / GeneratePrimitive accepts, in order.
inline constexpr const char* kPrimitiveNames[] = {
    "cube", "sphere", "plane", "cylinder", "cone", "capsule", "torus"};

} // namespace hbe::mesh

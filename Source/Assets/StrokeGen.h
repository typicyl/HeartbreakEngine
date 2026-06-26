// Assets/StrokeGen.h - scatter brush-stroke seeds across a mesh surface for the
// 3D painterly renderer (object space, deterministic, area-weighted). Lives in
// Assets/ so the RHI backend can generate strokes when it uploads a mesh.
#pragma once

#include "Assets/Mesh.h"

#include <vector>

namespace hbe::stroke {

// Scatter ~`density` strokes per object-space square unit over the mesh surface,
// capped at `maxStrokes`. Each stroke gets an interpolated surface position,
// normal, tangent, and uv. Deterministic for a given mesh + density so the
// generated set is stable across frames/sessions. Clears and fills `out`.
void GenerateSurfaceStrokes(const MeshData& mesh, float density, u32 maxStrokes,
                            std::vector<StrokeInstance>& out);

} // namespace hbe::stroke

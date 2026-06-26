// Editor/MeshThumbnail.h - CPU-rasterized mesh previews for the asset browser.
#pragma once

#include "Assets/Mesh.h"
#include "Core/Types.h"

#include <vector>

namespace hbe::editor {

// Renders `model` into a size x size RGBA8 image (tightly packed, row-major).
// Orthographic three-quarter view, flat per-face n.l shading tinted by
// `tint` (linear 0..1), transparent background so the tile background shows
// through. The default tint is the asset browser's neutral blue-gray.
std::vector<u32> RasterizeMeshThumbnail(const Model& model, u32 size,
                                        const glm::vec3& tint = {0.69f, 0.73f, 0.81f});

} // namespace hbe::editor

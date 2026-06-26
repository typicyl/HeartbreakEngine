// Assets/AssetLoader.h - load .uaf assets into engine/GPU resources (runtime).
#pragma once

#include "Assets/Mesh.h"
#include "Assets/UAF.h"
#include "RHI/RHI.h"

#include <filesystem>
#include <optional>

namespace hbe {

class Renderer;

namespace assets {

// Generates a full box-filtered mip chain for an RGBA8 texture payload in
// place (no-op if it already has mips or isn't 4 bytes/texel). sRGB formats
// are downsampled in linear space. Kills texture shimmer at glancing angles
// together with the backends' anisotropic samplers.
void GenerateMips(uaf::Texture& tex);

// Loads a texture `.uaf` and uploads it to the bindless table (generating
// mips on the fly). Returns an invalid handle on failure.
rhi::TextureHandle LoadTexture(Renderer& renderer, const std::filesystem::path& uaf);

// Loads a mesh `.uaf` as a CPU model (caller uploads / spawns).
std::optional<Model> LoadMesh(const std::filesystem::path& uaf);

} // namespace assets
} // namespace hbe

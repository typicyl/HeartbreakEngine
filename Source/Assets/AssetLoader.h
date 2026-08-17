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

// --- BC texture variant resolution (P6) ------------------------------------
// A BC-compressed texture is baked at import ALONGSIDE the uncompressed source: `Foo.uaf` gets a
// sibling `Foo.bc.uaf`. This returns that sibling name for a `.uaf` ref ("Foo.uaf" -> "Foo.bc.uaf";
// leaves a non-.uaf ref unchanged). The runtime prefers the sibling when texture compression is
// enabled AND the backend supports BC (see below) - transparently, keyed by the ORIGINAL ref so
// the cache and every consumer are unaffected. A non-mult-4 texture has no BC sibling and falls
// back to the uncompressed `.uaf` automatically (the read simply fails and the caller retries).
std::string BcVariantName(const std::string& uafRef);

// Whether the active backend can sample BC (set once at renderer init; read from worker threads
// during streaming, hence atomic). False -> never load a BC variant (the desc would be rejected).
void SetBlockCompressionAvailable(bool v);
bool BlockCompressionAvailable();

} // namespace assets
} // namespace hbe

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

// --- Missing-asset tally (safe-mode banner) --------------------------------
// A runtime asset that cannot be loaded is non-fatal: the loader logs + skips it
// and the game keeps running. But the player should be told their files are
// incomplete, so StageAssets NOTES each miss here and the engine reads the totals
// each frame to pick a safe-mode tier. IMPORTANT = structural (mesh/material) ->
// the game won't play correctly; COSMETIC = texture/paint -> it just looks wrong.
// Incremented from streaming worker threads, so the counters are atomic.
struct MissCounts { u32 important = 0; u32 cosmetic = 0; };
void NoteMissingAsset(bool important);
// Note a miss AT MOST ONCE per distinct `key` (until the next ResetMissTally). For load
// paths that are retried every frame or hit repeatedly (audio, UI images), so a single
// missing asset counts once, not thousands of times. Thread-safe.
void NoteMissingAssetOnce(const std::string& key, bool important);
MissCounts MissTally();
void ResetMissTally();

// Runtime audio load that FEEDS the miss tally. Wraps uaf::ReadAudio and, on a miss,
// notes ONE cosmetic miss per DISTINCT path (some callers retry a missing clip every
// frame, so it dedupes). Audio is cosmetic - the game still plays, it just may not sound
// as intended. For shipped, referenced audio only - NOT editor/import reads (those also
// touch non-shipped files). The dedupe set is cleared by ResetMissTally on each world load.
std::optional<uaf::Audio> ReadAudioTracked(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe

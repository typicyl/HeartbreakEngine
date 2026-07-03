// Assets/UAF.h - Unified Asset Format (.uaf).
//
// Every imported asset (textures, models, audio, ...) is stored as a `.uaf`
// binary container, similar in spirit to Unreal's .uasset. A small header
// (magic + version + asset type + GUID) precedes a type-specific payload. The
// runtime loads `.uaf` files; raw source files (.png/.gltf/.wav) are only read
// by the editor's importers (see Assets/Importer.*).
#pragma once

#include "Assets/Animation.h"
#include "Assets/Mesh.h"
#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe::uaf {

inline constexpr char kMagic[4] = {'U', 'A', 'F', '1'};
// v2: material texture refs; v3: emissive; v4: per-submesh .hbmat asset ref;
// v5: skinned vertices (72B) + optional rig (skeleton + animation clips)
inline constexpr u32 kVersion = 7; // v7: Audio gains a caption speaker name

enum class AssetType : u32 {
    Unknown = 0,
    Texture = 1,
    Mesh    = 2,
    Audio   = 3,
    Font    = 4, // raw TTF/OTF bytes (baked to an atlas at load time)
};

const char* ToString(AssetType t);

// --- Payloads --------------------------------------------------------------
struct Texture {
    u32 width = 1;
    u32 height = 1;
    u32 format = 1;    // rhi::Format value
    u32 mipCount = 1;
    std::vector<u8> pixels; // tightly packed, all mips
};

// Audio category, for mixing/organisation + accessibility (voicelines caption).
enum class AudioKind : u32 { Sfx = 0, Music = 1, Ambience = 2, Voiceline = 3 };
const char* ToString(AudioKind k);

struct Audio {
    u32 channels = 1;
    u32 sampleRate = 44100;
    u32 bitsPerSample = 16;
    AudioKind kind = AudioKind::Sfx; // SFX / Music / Ambience / Voiceline
    std::string caption;             // accessibility caption (voicelines)
    std::string speaker;             // v7: character name; shown as "Speaker: caption"
    std::vector<u8> pcm; // interleaved PCM samples
};

// A mesh asset stores a hbe::Model (one or more submeshes + materials).

// --- API -------------------------------------------------------------------
// Returns the asset type recorded in a `.uaf` file (Unknown on error).
AssetType PeekType(const std::filesystem::path& path);

bool WriteTexture(const std::filesystem::path& path, const Texture& tex, u64 guid = 0);
std::optional<Texture> ReadTexture(const std::filesystem::path& path);
// Reads ONLY a texture's width/height (skips the pixel payload). VFS-aware
// (works inside packed assets), so 9-slice borders resolve in shipped builds.
bool PeekTextureSize(const std::filesystem::path& path, u32& width, u32& height);

// `rig` (skeleton + clips) is optional; pass nullptr (or an invalid rig) for
// static meshes.
bool WriteMesh(const std::filesystem::path& path, const Model& model, u64 guid = 0,
               const Rig* rig = nullptr);
std::optional<Model> ReadMesh(const std::filesystem::path& path);
// Reads only the rig of a mesh asset (nullopt when the asset has none).
std::optional<Rig> ReadRig(const std::filesystem::path& path);

bool WriteAudio(const std::filesystem::path& path, const Audio& audio, u64 guid = 0);
std::optional<Audio> ReadAudio(const std::filesystem::path& path);

// Fonts: the payload is the verbatim TTF/OTF file.
bool WriteFont(const std::filesystem::path& path, const std::vector<u8>& ttf,
               u64 guid = 0);
std::optional<std::vector<u8>> ReadFont(const std::filesystem::path& path);

} // namespace hbe::uaf

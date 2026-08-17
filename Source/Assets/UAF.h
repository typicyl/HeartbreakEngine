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
// v8: Mesh gains per-submesh MORPH TARGETS (blendshapes). Before v8 the importer
//     read them out of the source model and then dropped them on the floor here,
//     so no `.uaf` on disk could ever resolve a blendshape atlas - facial
//     animation was dead on the FIRST spawn, not just on a respawn. Reads are
//     version-gated, so every existing v1-v7 asset still loads (with no morphs,
//     exactly as it behaves today); re-import a character to get its blendshapes.
// v9: Mesh gains per-submesh distance LODs (a u32 count + that many reduced
//     {vertices,indices} sets, written after the morph block). Generated at import
//     by mesh::BuildLodChain and shipped ALONGSIDE the full-detail geometry - LOD0 is
//     the submesh itself. Version-gated: every v1-v8 asset still loads (no LODs);
//     re-import (or --generate-mesh-lods) to add them.
inline constexpr u32 kVersion = 9; // v9: Mesh carries distance LODs

// A `.uaf` header optionally carries the asset's PACK SLOT (see Assets/SlotIds.h)
// as a u32 right after the guid. Its presence is a FLAG BIT on the version word,
// NOT a version bump, and that distinction is load-bearing: an existing v5 asset
// has to be stampable without claiming to be a v8 asset, or its reader would go
// looking for the v8 blendshape block that file never had. So the payload version
// and "does the header reserve a slot field" version independently.
inline constexpr u32 kSlotFlag = 0x80000000u;
inline constexpr usize kHeaderSize = 4 + 4 + 4 + 8;          // magic|version|type|guid
inline constexpr usize kHeaderSizeWithSlot = kHeaderSize + 4; // ...|packSlot

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

// Reads a `.uaf`'s header (type + PAYLOAD version + guid) without paging the payload (assets
// can be hundreds of MB). Returns false on a missing/corrupt/short file. The asset auto-upgrade
// uses this to find files below `kVersion` and re-write them in place, preserving their guid.
bool PeekHeader(const std::filesystem::path& path, AssetType& type, u32& version, u64& guid);

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

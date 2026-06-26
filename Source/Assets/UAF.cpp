// Assets/UAF.cpp
#include "Assets/UAF.h"
#include "Assets/VFS.h"
#include "Core/BinaryStream.h"
#include "Core/Log.h"

#include <cstring>

namespace {
// All .uaf reads go through the VFS so shipped builds can serve them from
// mounted .uap packs instead of loose files.
std::vector<hbe::u8> LoadAssetFile(const std::filesystem::path& path) {
    auto bytes = hbe::vfs::ReadFile(path);
    return bytes ? std::move(*bytes) : std::vector<hbe::u8>{};
}
} // namespace

namespace hbe::uaf {
namespace {

void WriteHeader(BinaryWriter& w, AssetType type, u64 guid) {
    w.Bytes(kMagic, 4);
    w.Pod(kVersion);
    w.Pod(static_cast<u32>(type));
    w.Pod(guid);
}

// Validates the header and returns the recorded type (Unknown on mismatch).
AssetType ReadHeader(BinaryReader& r, u32* outVersion = nullptr, u64* outGuid = nullptr) {
    char magic[4] = {};
    r.Bytes(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return AssetType::Unknown;
    u32 version = 0, type = 0;
    u64 guid = 0;
    r.Pod(version);
    r.Pod(type);
    r.Pod(guid);
    if (outVersion) *outVersion = version;
    if (outGuid) *outGuid = guid;
    if (!r.Ok() || version > kVersion) return AssetType::Unknown;
    return static_cast<AssetType>(type);
}

void WriteMaterial(BinaryWriter& w, const Material& m) {
    w.Pod(m.baseColor);
    w.Pod(m.metallic);
    w.Pod(m.roughness);
    w.Str(m.name);
    // v2 texture references.
    w.Str(m.baseColorTex);
    w.Str(m.normalTex);
    w.Str(m.mrTex);
    w.Str(m.aoTex);
    // v3 emissive.
    w.Pod(m.emissive);
    w.Str(m.emissiveTex);
    // v4 material asset reference.
    w.Str(m.materialAsset);
}
bool ReadMaterial(BinaryReader& r, Material& m, u32 version) {
    r.Pod(m.baseColor);
    r.Pod(m.metallic);
    r.Pod(m.roughness);
    r.Str(m.name);
    if (version >= 2) {
        r.Str(m.baseColorTex);
        r.Str(m.normalTex);
        r.Str(m.mrTex);
        r.Str(m.aoTex);
    }
    if (version >= 3) {
        r.Pod(m.emissive);
        r.Str(m.emissiveTex);
    }
    if (version >= 4) {
        r.Str(m.materialAsset);
    }
    return r.Ok();
}

} // namespace

const char* ToString(AssetType t) {
    switch (t) {
        case AssetType::Texture: return "Texture";
        case AssetType::Mesh:    return "Mesh";
        case AssetType::Audio:   return "Audio";
        case AssetType::Font:    return "Font";
        default:                 return "Unknown";
    }
}

AssetType PeekType(const std::filesystem::path& path) {
    // Read only the fixed-size header (magic+version+type+guid = 20 bytes) rather
    // than loading the whole file - asset assets can be hundreds of MB.
    constexpr usize kHeaderSize = 4 + 4 + 4 + 8;
    std::ifstream in(path, std::ios::binary);
    if (!in) return AssetType::Unknown;
    u8 hdr[kHeaderSize] = {};
    in.read(reinterpret_cast<char*>(hdr), kHeaderSize);
    if (static_cast<usize>(in.gcount()) < kHeaderSize) return AssetType::Unknown;
    BinaryReader r(hdr, kHeaderSize);
    return ReadHeader(r);
}

// --- Texture ---------------------------------------------------------------
bool WriteTexture(const std::filesystem::path& path, const Texture& tex, u64 guid) {
    BinaryWriter w;
    WriteHeader(w, AssetType::Texture, guid);
    w.Pod(tex.width);
    w.Pod(tex.height);
    w.Pod(tex.format);
    w.Pod(tex.mipCount);
    w.Vec(tex.pixels);
    if (!w.SaveToFile(path)) {
        HBE_ERROR("UAF: failed to write texture '{}'.", path.string());
        return false;
    }
    return true;
}

std::optional<Texture> ReadTexture(const std::filesystem::path& path) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return std::nullopt;
    BinaryReader r(bytes);
    if (ReadHeader(r) != AssetType::Texture) return std::nullopt;
    Texture t;
    r.Pod(t.width);
    r.Pod(t.height);
    r.Pod(t.format);
    r.Pod(t.mipCount);
    r.Vec(t.pixels);
    if (!r.Ok()) return std::nullopt;
    return t;
}

// --- Mesh ------------------------------------------------------------------
namespace {

// The 48-byte vertex written by v1-v4 assets (no skinning fields).
struct LegacyVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 uv;
};
static_assert(sizeof(LegacyVertex) == 48, "v4 vertex layout");

void WriteRigPayload(BinaryWriter& w, const Rig& rig) {
    w.Pod(static_cast<u32>(rig.skeleton.joints.size()));
    for (const Joint& j : rig.skeleton.joints) {
        w.Str(j.name);
        w.Pod(j.parent);
        w.Pod(j.inverseBind);
        w.Pod(j.bindPosition);
        w.Pod(j.bindRotation);
        w.Pod(j.bindScale);
    }
    w.Pod(static_cast<u32>(rig.clips.size()));
    for (const AnimationClip& clip : rig.clips) {
        w.Str(clip.name);
        w.Pod(clip.duration);
        w.Pod(static_cast<u32>(clip.channels.size()));
        for (const AnimChannel& ch : clip.channels) {
            w.Str(ch.jointName);
            w.Vec(ch.positions);
            w.Vec(ch.rotations);
            w.Vec(ch.scales);
        }
    }
}

bool ReadRigPayload(BinaryReader& r, Rig& rig) {
    u32 jointCount = 0;
    r.Pod(jointCount);
    rig.skeleton.joints.resize(jointCount);
    for (Joint& j : rig.skeleton.joints) {
        r.Str(j.name);
        r.Pod(j.parent);
        r.Pod(j.inverseBind);
        r.Pod(j.bindPosition);
        r.Pod(j.bindRotation);
        r.Pod(j.bindScale);
    }
    u32 clipCount = 0;
    r.Pod(clipCount);
    rig.clips.resize(clipCount);
    for (AnimationClip& clip : rig.clips) {
        r.Str(clip.name);
        r.Pod(clip.duration);
        u32 channelCount = 0;
        r.Pod(channelCount);
        clip.channels.resize(channelCount);
        for (AnimChannel& ch : clip.channels) {
            r.Str(ch.jointName);
            r.Vec(ch.positions);
            r.Vec(ch.rotations);
            r.Vec(ch.scales);
        }
    }
    return r.Ok();
}

// Reads the mesh payload; either output may be null to skip it. The rig flag
// was introduced in v5 (older assets simply have no trailing rig).
bool ReadMeshPayload(BinaryReader& r, u32 version, Model* outModel, Rig* outRig) {
    u32 count = 0;
    r.Pod(count);
    for (u32 i = 0; i < count; ++i) {
        MeshData md;
        if (version >= 5) {
            r.Vec(md.vertices);
        } else {
            // v1-v4: convert packed legacy vertices (skinning fields default).
            std::vector<LegacyVertex> legacy;
            r.Vec(legacy);
            md.vertices.resize(legacy.size());
            for (usize v = 0; v < legacy.size(); ++v) {
                md.vertices[v].position = legacy[v].position;
                md.vertices[v].normal = legacy[v].normal;
                md.vertices[v].tangent = legacy[v].tangent;
                md.vertices[v].uv = legacy[v].uv;
            }
        }
        r.Vec(md.indices);
        if (!ReadMaterial(r, md.material, version)) return false;
        r.Str(md.name);
        if (!r.Ok()) return false;
        if (outModel) outModel->push_back(std::move(md));
    }
    if (version >= 5 && outRig) {
        u32 hasRig = 0;
        r.Pod(hasRig);
        if (hasRig != 0 && !ReadRigPayload(r, *outRig)) return false;
    }
    return r.Ok();
}

} // namespace

bool WriteMesh(const std::filesystem::path& path, const Model& model, u64 guid,
               const Rig* rig) {
    BinaryWriter w;
    WriteHeader(w, AssetType::Mesh, guid);
    w.Pod(static_cast<u32>(model.size()));
    for (const MeshData& md : model) {
        w.Vec(md.vertices);
        w.Vec(md.indices);
        WriteMaterial(w, md.material);
        w.Str(md.name);
    }
    const u32 hasRig = (rig && rig->Valid()) ? 1u : 0u;
    w.Pod(hasRig);
    if (hasRig) WriteRigPayload(w, *rig);
    if (!w.SaveToFile(path)) {
        HBE_ERROR("UAF: failed to write mesh '{}'.", path.string());
        return false;
    }
    return true;
}

std::optional<Model> ReadMesh(const std::filesystem::path& path) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return std::nullopt;
    BinaryReader r(bytes);
    u32 version = 0;
    if (ReadHeader(r, &version) != AssetType::Mesh) return std::nullopt;
    Model model;
    if (!ReadMeshPayload(r, version, &model, nullptr)) return std::nullopt;
    return model;
}

std::optional<Rig> ReadRig(const std::filesystem::path& path) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return std::nullopt;
    BinaryReader r(bytes);
    u32 version = 0;
    if (ReadHeader(r, &version) != AssetType::Mesh) return std::nullopt;
    if (version < 5) return std::nullopt; // pre-rig asset
    Rig rig;
    if (!ReadMeshPayload(r, version, nullptr, &rig)) return std::nullopt;
    if (!rig.Valid()) return std::nullopt;
    return rig;
}

// --- Audio -----------------------------------------------------------------
const char* ToString(AudioKind k) {
    switch (k) {
        case AudioKind::Sfx:       return "SFX";
        case AudioKind::Music:     return "Music";
        case AudioKind::Ambience:  return "Ambience";
        case AudioKind::Voiceline: return "Voiceline";
    }
    return "SFX";
}

bool WriteAudio(const std::filesystem::path& path, const Audio& audio, u64 guid) {
    BinaryWriter w;
    WriteHeader(w, AssetType::Audio, guid);
    w.Pod(audio.channels);
    w.Pod(audio.sampleRate);
    w.Pod(audio.bitsPerSample);
    w.Pod(static_cast<u32>(audio.kind)); // v6
    w.Str(audio.caption);                // v6
    w.Vec(audio.pcm);
    if (!w.SaveToFile(path)) {
        HBE_ERROR("UAF: failed to write audio '{}'.", path.string());
        return false;
    }
    return true;
}

// --- Font ------------------------------------------------------------------
bool WriteFont(const std::filesystem::path& path, const std::vector<u8>& ttf, u64 guid) {
    BinaryWriter w;
    WriteHeader(w, AssetType::Font, guid);
    w.Vec(ttf);
    if (!w.SaveToFile(path)) {
        HBE_ERROR("UAF: failed to write font '{}'.", path.string());
        return false;
    }
    return true;
}

std::optional<std::vector<u8>> ReadFont(const std::filesystem::path& path) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return std::nullopt;
    BinaryReader r(bytes);
    if (ReadHeader(r) != AssetType::Font) return std::nullopt;
    std::vector<u8> ttf;
    r.Vec(ttf);
    if (!r.Ok() || ttf.empty()) return std::nullopt;
    return ttf;
}

std::optional<Audio> ReadAudio(const std::filesystem::path& path) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return std::nullopt;
    BinaryReader r(bytes);
    u32 version = 0;
    if (ReadHeader(r, &version) != AssetType::Audio) return std::nullopt;
    Audio a;
    r.Pod(a.channels);
    r.Pod(a.sampleRate);
    r.Pod(a.bitsPerSample);
    if (version >= 6) { // kind + accessibility caption
        u32 kind = 0;
        r.Pod(kind);
        a.kind = static_cast<AudioKind>(kind > 3 ? 0 : kind);
        r.Str(a.caption);
    }
    r.Vec(a.pcm);
    if (!r.Ok()) return std::nullopt;
    return a;
}

} // namespace hbe::uaf

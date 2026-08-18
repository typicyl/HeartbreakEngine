// Assets/UAF.cpp
#include "Assets/UAF.h"
#include "Assets/MeshCodec.h" // v10 quantized + meshopt-encoded geometry blocks
#include "Assets/VFS.h"
#include "Core/BinaryStream.h"
#include "Core/Log.h"

// miniaudio decoder - declarations only; MINIAUDIO_IMPLEMENTATION (the WAV/MP3/FLAC
// decoders) lives in Audio/AudioSystem.cpp, in the same static library. Used to decode a
// v11 asset's stored compressed source back into PCM at load time.
#include <miniaudio.h>

#include <cmath>
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

// Every header written from here RESERVES the pack-slot field (kSlotFlag), even
// though the writer does not know the number: an importer produces the bytes,
// and the id is assigned afterwards by Assets/SlotIds.cpp. Reserving it makes
// that a 4-byte patch at a fixed offset instead of a rewrite of a file that may
// be hundreds of megabytes. kUnassignedSlot is what an unstamped asset holds.
constexpr u32 kUnassignedSlot = 0xFFFFFFFFu;

void WriteHeader(BinaryWriter& w, AssetType type, u64 guid) {
    w.Bytes(kMagic, 4);
    w.Pod(static_cast<u32>(kVersion | kSlotFlag));
    w.Pod(static_cast<u32>(type));
    w.Pod(guid);
    w.Pod(kUnassignedSlot);
}

// Validates the header and returns the recorded type (Unknown on mismatch).
// `outVersion` is the PAYLOAD version with the slot flag masked off - every
// payload read downstream gates on it, so leaking the flag bit through would
// make every stamped asset look like a future version and fail to load.
AssetType ReadHeader(BinaryReader& r, u32* outVersion = nullptr, u64* outGuid = nullptr,
                     u32* outSlot = nullptr) {
    char magic[4] = {};
    r.Bytes(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return AssetType::Unknown;
    u32 raw = 0, type = 0;
    u64 guid = 0;
    r.Pod(raw);
    r.Pod(type);
    r.Pod(guid);
    const bool hasSlot = (raw & kSlotFlag) != 0;
    const u32 version = raw & ~kSlotFlag;
    u32 slot = kUnassignedSlot;
    if (hasSlot) r.Pod(slot);
    if (outVersion) *outVersion = version;
    if (outGuid) *outGuid = guid;
    if (outSlot) *outSlot = slot;
    if (!r.Ok() || version == 0 || version > kVersion) return AssetType::Unknown;
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
    // Read only the header (20 bytes, or 24 when it reserves a pack slot) rather
    // than loading the whole file - assets can be hundreds of MB. A short read is
    // fine: whether the extra 4 bytes are needed is decided by the flag bit, and
    // BinaryReader reports a shortfall through Ok().
    std::ifstream in(path, std::ios::binary);
    if (!in) return AssetType::Unknown;
    u8 hdr[kHeaderSizeWithSlot] = {};
    in.read(reinterpret_cast<char*>(hdr), kHeaderSizeWithSlot);
    const usize got = static_cast<usize>(in.gcount());
    if (got < kHeaderSize) return AssetType::Unknown;
    BinaryReader r(hdr, got);
    return ReadHeader(r);
}

bool PeekHeader(const std::filesystem::path& path, AssetType& type, u32& version, u64& guid) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    u8 hdr[kHeaderSizeWithSlot] = {};
    in.read(reinterpret_cast<char*>(hdr), kHeaderSizeWithSlot);
    const usize got = static_cast<usize>(in.gcount());
    if (got < kHeaderSize) return false;
    BinaryReader r(hdr, got);
    u32 v = 0;
    u64 g = 0;
    const AssetType t = ReadHeader(r, &v, &g);
    if (t == AssetType::Unknown) return false;
    type = t;
    version = v;
    guid = g;
    return true;
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

bool PeekTextureSize(const std::filesystem::path& path, u32& width, u32& height) {
    std::vector<u8> bytes = LoadAssetFile(path);
    if (bytes.empty()) return false;
    BinaryReader r(bytes);
    if (ReadHeader(r) != AssetType::Texture) return false;
    r.Pod(width);
    r.Pod(height); // width/height are the first fields after the header
    return r.Ok();
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
        if (version >= 10) {
            // v10: quantized + meshopt-encoded geometry, dequantized back to the exact
            // 72-byte Vertex here (static meshes get zeroed skinning).
            if (!meshcodec::ReadGeometry(r, md.vertices, md.indices)) return false;
        } else if (version >= 5) {
            r.Vec(md.vertices);
            r.Vec(md.indices);
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
            r.Vec(md.indices);
        }
        if (!ReadMaterial(r, md.material, version)) return false;
        r.Str(md.name);
        // v8: blendshape / morph targets. Read unconditionally (not only when
        // outModel is wanted) - the payload is one sequential stream, so skipping
        // the bytes would desynchronise the trailing rig for ReadRig.
        if (version >= 8) {
            u32 morphCount = 0;
            r.Pod(morphCount);
            if (!r.Ok()) return false;
            for (u32 m = 0; m < morphCount; ++m) {
                MorphTarget mt;
                r.Str(mt.name);
                r.Vec(mt.posDelta);
                r.Vec(mt.nrmDelta);
                if (!r.Ok()) return false;
                md.morphTargets.push_back(std::move(mt));
            }
        }
        // v9: distance LODs. Read unconditionally (like morphs) so the trailing rig
        // stays aligned in the sequential stream even when outModel is null (ReadRig).
        if (version >= 9) {
            u32 lodCount = 0;
            r.Pod(lodCount);
            if (!r.Ok()) return false;
            for (u32 l = 0; l < lodCount; ++l) {
                MeshLod lod;
                if (version >= 10) {
                    if (!meshcodec::ReadGeometry(r, lod.vertices, lod.indices)) return false;
                } else {
                    r.Vec(lod.vertices);
                    r.Vec(lod.indices);
                }
                if (!r.Ok()) return false;
                md.lods.push_back(std::move(lod));
            }
        }
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
        // v10: quantized + meshopt-encoded geometry (was a raw 72-byte Vertex dump).
        // Vertices are NOT reordered by the codec, so the morph deltas below stay
        // index-parallel.
        meshcodec::WriteGeometry(w, md.vertices, md.indices);
        WriteMaterial(w, md.material);
        w.Str(md.name);
        // v8: blendshapes. Kept RAW - deltas are not unit vectors (oct-encoding is wrong
        // for them), they are index-parallel to the vertices, and they appear on few
        // meshes. The importer has always produced these (ModelLoader converts aiAnimMesh
        // absolute positions to deltas).
        w.Pod(static_cast<u32>(md.morphTargets.size()));
        for (const MorphTarget& mt : md.morphTargets) {
            w.Str(mt.name);
            w.Vec(mt.posDelta);
            w.Vec(mt.nrmDelta);
        }
        // v9/v10: distance LODs. Each LOD is a compact geometry block too (LODs are
        // always static - skinned meshes are excluded from LOD generation - so this is a
        // pure win). Written after morphs so the trailing rig stays aligned.
        w.Pod(static_cast<u32>(md.lods.size()));
        for (const MeshLod& lod : md.lods) meshcodec::WriteGeometry(w, lod.vertices, lod.indices);
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

namespace {
// v11 storage-mode selector, written after speaker (0 = raw PCM, 1 = compressed source).
constexpr u32 kAudioStorePcm = 0;
constexpr u32 kAudioStoreSource = 1;

// Decodes compressed source bytes (wav/mp3/flac) to interleaved 16-bit PCM via miniaudio,
// filling channels/sampleRate from the stream. Mirrors the importer's decode, but from a
// memory buffer at load time. Returns false on an undecodable/empty source.
bool DecodeAudioSource(const std::vector<u8>& src, u32& channels, u32& sampleRate,
                       std::vector<u8>& pcm) {
    if (src.empty()) return false;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(src.data(), src.size(), &cfg, &dec) != MA_SUCCESS) return false;
    channels = dec.outputChannels;
    sampleRate = dec.outputSampleRate;
    const ma_uint64 kChunkFrames = 8192;
    std::vector<i16> chunk(static_cast<usize>(kChunkFrames) * (channels ? channels : 1));
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, chunk.data(), kChunkFrames, &got) != MA_SUCCESS ||
            got == 0)
            break;
        const u8* b = reinterpret_cast<const u8*>(chunk.data());
        pcm.insert(pcm.end(), b, b + static_cast<usize>(got) * channels * sizeof(i16));
        if (got < kChunkFrames) break;
    }
    ma_decoder_uninit(&dec);
    return !pcm.empty();
}
} // namespace

bool WriteAudio(const std::filesystem::path& path, const Audio& audio, u64 guid) {
    BinaryWriter w;
    WriteHeader(w, AssetType::Audio, guid);
    w.Pod(audio.channels);
    w.Pod(audio.sampleRate);
    w.Pod(audio.bitsPerSample);
    w.Pod(static_cast<u32>(audio.kind)); // v6
    w.Str(audio.caption);                // v6
    w.Str(audio.speaker);                // v7
    // v11: store the compressed SOURCE when we have it (much smaller than decoded PCM);
    // otherwise fall back to raw PCM exactly as v7 did (procedurally-generated clips, and
    // the reference test fixtures, have no source file).
    if (!audio.encoded.empty()) {
        w.Pod<u32>(kAudioStoreSource);
        w.Pod<u32>(audio.encodedFormat);
        w.Vec(audio.encoded);
    } else {
        w.Pod<u32>(kAudioStorePcm);
        w.Vec(audio.pcm);
    }
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
    if (version >= 7) r.Str(a.speaker); // caption speaker name
    if (version >= 11) {
        u32 mode = kAudioStorePcm;
        r.Pod(mode);
        if (mode == kAudioStoreSource) {
            r.Pod(a.encodedFormat);
            r.Vec(a.encoded);
            if (!r.Ok()) return std::nullopt;
            u32 ch = a.channels, sr = a.sampleRate;
            if (!DecodeAudioSource(a.encoded, ch, sr, a.pcm)) {
                HBE_ERROR("UAF: failed to decode stored audio source in '{}'.", path.string());
                return std::nullopt;
            }
            a.channels = ch;
            a.sampleRate = sr;
            a.bitsPerSample = 16; // miniaudio decodes to signed-16 PCM
        } else {
            r.Vec(a.pcm);
        }
    } else {
        r.Vec(a.pcm); // v1-v10: raw PCM
    }
    if (!r.Ok()) return std::nullopt;
    return a;
}

// ---------------------------------------------------------------------------
// Audio self-test (--test-audiocodec)
// ---------------------------------------------------------------------------
namespace {
void PutLE(std::vector<u8>& b, u32 v, int bytes) {
    for (int i = 0; i < bytes; ++i) b.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
}
// A minimal PCM WAV (RIFF) around interleaved 16-bit samples - a valid source miniaudio
// decodes, so the test needs no real audio file on disk.
std::vector<u8> MakeWav(u32 channels, u32 sampleRate, const std::vector<i16>& samples) {
    const u32 dataBytes = static_cast<u32>(samples.size() * sizeof(i16));
    std::vector<u8> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    PutLE(b, 36 + dataBytes, 4);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    PutLE(b, 16, 4);                                   // fmt chunk size
    PutLE(b, 1, 2);                                    // PCM
    PutLE(b, channels, 2);
    PutLE(b, sampleRate, 4);
    PutLE(b, sampleRate * channels * 2, 4);            // byte rate
    PutLE(b, channels * 2, 2);                         // block align
    PutLE(b, 16, 2);                                   // bits per sample
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    PutLE(b, dataBytes, 4);
    const u8* s = reinterpret_cast<const u8*>(samples.data());
    b.insert(b.end(), s, s + dataBytes);
    return b;
}
} // namespace

bool AudioSelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { HBE_ERROR("audiocodec: FAIL - {}", what); ++fails; }
    };
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hbe_audiocodec_test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // 0.25s stereo sine as the "source" WAV.
    const u32 ch = 2, sr = 22050;
    std::vector<i16> samples(sr / 4 * ch);
    for (u32 i = 0; i < sr / 4; ++i) {
        const i16 s = static_cast<i16>(std::sin(i * 0.1f) * 12000.0f);
        samples[i * 2] = s;
        samples[i * 2 + 1] = static_cast<i16>(-s);
    }
    const std::vector<u8> wav = MakeWav(ch, sr, samples);

    // v11 encoded-source path: WriteAudio must store the ENCODED bytes, NOT the (here
    // deliberately huge) PCM, and ReadAudio must decode the source back to PCM.
    {
        Audio a;
        a.encoded = wav;
        a.encodedFormat = 1; // wav
        a.kind = AudioKind::Voiceline;
        a.caption = "hello";
        a.speaker = "Nyx";
        a.pcm.assign(1u << 20, 0); // 1 MB dummy that must NOT be written to disk
        const std::filesystem::path p = dir / "clip.uaf";
        check(WriteAudio(p, a, 0x55), "WriteAudio (encoded) must succeed");
        const u64 fileSize = std::filesystem::file_size(p, ec);
        check(fileSize < wav.size() + 4096, "the .uaf must store the SOURCE, not the 1 MB PCM");

        const std::optional<Audio> back = ReadAudio(p);
        check(back.has_value(), "ReadAudio must succeed");
        if (back) {
            check(back->channels == ch && back->sampleRate == sr, "metadata survives");
            check(back->kind == AudioKind::Voiceline && back->caption == "hello" &&
                      back->speaker == "Nyx",
                  "kind/caption/speaker survive");
            check(!back->pcm.empty(), "source decoded back to PCM on load");
            check(back->encoded == wav, "the source bytes are preserved for re-save");
            // read-modify-write (the editor's caption edit) must keep the compact form.
            Audio edit = *back;
            edit.caption = "changed";
            const std::filesystem::path p2 = dir / "clip2.uaf";
            check(WriteAudio(p2, edit, 0x55), "re-save after metadata edit");
            check(std::filesystem::file_size(p2, ec) < wav.size() + 4096,
                  "re-save must stay compact (source preserved, not re-stored as PCM)");
            const std::optional<Audio> back2 = ReadAudio(p2);
            check(back2 && back2->caption == "changed" && !back2->pcm.empty(),
                  "edited metadata + decodable audio after re-save");
        }
    }

    // Legacy raw-PCM path (no source): still round-trips.
    {
        Audio a;
        a.channels = ch;
        a.sampleRate = sr;
        a.bitsPerSample = 16;
        const u8* s = reinterpret_cast<const u8*>(samples.data());
        a.pcm.assign(s, s + samples.size() * sizeof(i16));
        const std::filesystem::path p = dir / "raw.uaf";
        check(WriteAudio(p, a, 0x66), "WriteAudio (raw PCM) must succeed");
        const std::optional<Audio> back = ReadAudio(p);
        check(back && back->pcm == a.pcm && back->encoded.empty(),
              "legacy raw-PCM asset round-trips unchanged");
    }

    std::filesystem::remove_all(dir, ec);
    if (fails == 0)
        HBE_INFO("audiocodec: passed (stores compressed source, decodes on load, metadata "
                 "read-modify-write, raw-PCM back-compat).");
    return fails == 0;
}

} // namespace hbe::uaf

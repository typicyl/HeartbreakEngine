// Editor/Importer.cpp
#include "Editor/Importer.h"

#include "Assets/AssetFormats.h" // the single source of truth for source formats
#include "Assets/MaterialAsset.h"
#include "Assets/AssetLoader.h"   // assets::GenerateMips (mip-then-compress for BC bake)
#include "Assets/MeshOptimize.h" // import-time GPU geometry optimization (meshoptimizer)
#include "Assets/MeshSimplify.h"  // import-time distance-LOD generation (quadric decimation)
#include "Assets/ModelLoader.h"
#include "Editor/TextureCompress.h" // import-time BC texture compression (opt-in)
#include "Assets/SlotIds.h" // the pack slot is assigned HERE, at import
#include "Assets/UAF.h"
#include "Core/BinaryStream.h" // --test-upgrade writes a genuine legacy v9 .uaf fixture
#include "Core/Log.h"
#include "Project/Project.h"
#include "RHI/RHI.h"

#include <stb_image.h>

// miniaudio decoder — declarations only; MINIAUDIO_IMPLEMENTATION (with the
// built-in WAV/MP3/FLAC decoders) lives in Audio/AudioSystem.cpp. Used to decode
// source audio to PCM on import.
#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>

namespace hbe::importer {
namespace fs = std::filesystem;
namespace {

u64 GenGuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

// All four category tests defer to the registry (Assets/AssetFormats.cpp), which
// is also what the editor's Import dialog builds its filter from - so the dialog
// can no longer offer a different set of formats than the importer accepts.
std::string LowerExt(const fs::path& p) { return assets::NormalizeExtension(p); }

bool IsImage(const std::string& e) { return assets::IsSourceKind(e, assets::SourceKind::Image); }
bool IsModel(const std::string& e) { return assets::IsSourceKind(e, assets::SourceKind::Model); }
bool IsAudio(const std::string& e) { return assets::IsSourceKind(e, assets::SourceKind::Audio); }
bool IsFont(const std::string& e)  { return assets::IsSourceKind(e, assets::SourceKind::Font); }

// Fonts are stored verbatim (the engine bakes atlases at load time).
bool ImportFont(const fs::path& src, const fs::path& out) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    return uaf::WriteFont(out, bytes, GenGuid());
}

// Non-destructive BC bake (opt-in): mip-then-compress a COPY of the imported RGBA8 texture and
// write it beside the untouched `out` as `<stem>.bc.uaf`. Skipped when compression is off, on a
// non-RGBA8 (HDR) input, or a non-mult-4 texture (CompressToBC refuses those -> stays uncompressed).
void MaybeBakeBC(const uaf::Texture& rgba, const fs::path& out, tex::BCKind kind, bool srgb) {
    const fs::path bcOut = out.parent_path() / (out.stem().string() + ".bc.uaf");
    std::error_code ec;
    // EVERY path that does not (re)write a BC variant must delete a pre-existing one, or a stale
    // sibling from an earlier import (different content/size, or baked when compression was on)
    // outlives its source and gets packed + loaded in place of the current texture.
    if (!Project::HasActive() || !Project::Active().Settings().textureCompression ||
        kind == tex::BCKind::None) {
        fs::remove(bcOut, ec);
        return;
    }
    uaf::Texture mipped = rgba;        // BC ships pre-baked mips (can't mip a block format on GPU)
    assets::GenerateMips(mipped);
    std::optional<uaf::Texture> bc = tex::CompressToBC(mipped, kind, srgb);
    if (!bc) { fs::remove(bcOut, ec); return; } // non-mult-4 / refused: drop any stale sibling
    if (uaf::WriteTexture(bcOut, *bc, GenGuid()))
        HBE_INFO("Importer: baked BC variant '{}' ({} mips, format {}).",
                 bcOut.filename().string(), bc->mipCount, bc->format);
}

bool ImportTexture(const fs::path& src, const fs::path& out, bool srgb,
                   tex::BCKind bcKind = tex::BCKind::None) { // standalone: role unknown, keep RGBA8
    int w = 0, h = 0, channels = 0;

    // HDR sources (.hdr/.exr-ish) carry values outside [0,1]; keep them as
    // 32-bit float (linear) instead of crushing them to 8-bit. The IBL path and
    // any emissive/skybox use benefit from the preserved range.
    if (stbi_is_hdr(src.string().c_str())) {
        float* pixels = stbi_loadf(src.string().c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            HBE_ERROR("Importer: stb_image (HDR) failed for '{}': {}", src.string(),
                      stbi_failure_reason());
            return false;
        }
        uaf::Texture tex;
        tex.width = static_cast<u32>(w);
        tex.height = static_cast<u32>(h);
        tex.format = static_cast<u32>(rhi::Format::R32G32B32A32_FLOAT); // always linear
        tex.mipCount = 1;
        const usize bytes = static_cast<usize>(w) * h * 4 * sizeof(float);
        tex.pixels.resize(bytes);
        std::memcpy(tex.pixels.data(), pixels, bytes);
        stbi_image_free(pixels);
        return uaf::WriteTexture(out, tex, GenGuid());
    }

    stbi_uc* pixels = stbi_load(src.string().c_str(), &w, &h, &channels, 4); // force RGBA
    if (!pixels) {
        HBE_ERROR("Importer: stb_image failed for '{}': {}", src.string(), stbi_failure_reason());
        return false;
    }
    uaf::Texture tex;
    tex.width = static_cast<u32>(w);
    tex.height = static_cast<u32>(h);
    // Base color is sRGB-encoded (decoded to linear by the sampler); data maps
    // (normal/metallic-roughness/AO) are linear UNORM.
    tex.format = static_cast<u32>(srgb ? rhi::Format::R8G8B8A8_SRGB : rhi::Format::R8G8B8A8_UNORM);
    tex.mipCount = 1;
    tex.pixels.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
    stbi_image_free(pixels);

    MaybeBakeBC(tex, out, bcKind, srgb); // non-destructive BC sibling (opt-in)
    return uaf::WriteTexture(out, tex, GenGuid());
}

// Writes a texture embedded in a model file (glb / embedded FBX) to `.uaf`.
// Compressed payloads (PNG/JPG bytes) are decoded from memory; raw payloads are
// already RGBA8.
bool ImportEmbeddedTexture(const EmbeddedTexture& emb, const fs::path& out, bool srgb,
                           tex::BCKind bcKind = tex::BCKind::None) {
    uaf::Texture tex;
    if (emb.compressed) {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            emb.data.data(), static_cast<int>(emb.data.size()), &w, &h, &channels, 4);
        if (!pixels) {
            HBE_ERROR("Importer: failed to decode embedded texture '{}': {}", emb.name,
                      stbi_failure_reason());
            return false;
        }
        tex.width = static_cast<u32>(w);
        tex.height = static_cast<u32>(h);
        tex.pixels.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
        stbi_image_free(pixels);
    } else {
        if (emb.width == 0 || emb.height == 0 ||
            emb.data.size() < static_cast<usize>(emb.width) * emb.height * 4) {
            return false;
        }
        tex.width = emb.width;
        tex.height = emb.height;
        tex.pixels = emb.data; // already RGBA8
    }
    tex.format =
        static_cast<u32>(srgb ? rhi::Format::R8G8B8A8_SRGB : rhi::Format::R8G8B8A8_UNORM);
    tex.mipCount = 1;
    MaybeBakeBC(tex, out, bcKind, srgb); // non-destructive BC sibling (opt-in)
    return uaf::WriteTexture(out, tex, GenGuid());
}

// Decodes percent-encoded URI characters (glTF texture URIs encode spaces
// and friends as %20 etc., which never match on-disk file names verbatim).
std::string PercentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (usize i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) &&
            std::isxdigit((unsigned char)in[i + 2])) {
            out.push_back(static_cast<char>(
                std::stoi(in.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Replaces characters Windows forbids in file names.
std::string SanitizeFileName(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        if (std::strchr("\\/:*?\"<>|", c)) c = '_';
    }
    return out.empty() ? "Material" : out;
}

// Decodes a material texture ref (external file, or embedded "*N") to RGBA8.
bool DecodeRef(const std::string& ref, const fs::path& modelDir,
               const std::vector<EmbeddedTexture>& embedded, std::vector<u8>& rgba, u32& w,
               u32& h) {
    if (ref.empty()) return false;
    if (ref[0] == '*') {
        for (const EmbeddedTexture& e : embedded) {
            if (e.name != ref) continue;
            if (e.compressed) {
                int iw = 0, ih = 0, ch = 0;
                stbi_uc* px = stbi_load_from_memory(
                    e.data.data(), static_cast<int>(e.data.size()), &iw, &ih, &ch, 4);
                if (!px) return false;
                w = static_cast<u32>(iw);
                h = static_cast<u32>(ih);
                rgba.assign(px, px + static_cast<usize>(iw) * ih * 4);
                stbi_image_free(px);
                return true;
            }
            if (e.width == 0 || e.height == 0) return false;
            w = e.width;
            h = e.height;
            rgba = e.data;
            return true;
        }
        return false;
    }
    const std::string decoded = PercentDecode(ref);
    std::error_code ec;
    for (const fs::path& cand :
         {modelDir / fs::path(ref), modelDir / fs::path(decoded),
          modelDir / fs::path(decoded).filename(),
          modelDir / "textures" / fs::path(decoded).filename()}) {
        if (!fs::exists(cand, ec)) continue;
        int iw = 0, ih = 0, ch = 0;
        stbi_uc* px = stbi_load(cand.string().c_str(), &iw, &ih, &ch, 4);
        if (!px) return false;
        w = static_cast<u32>(iw);
        h = static_cast<u32>(ih);
        rgba.assign(px, px + static_cast<usize>(iw) * ih * 4);
        stbi_image_free(px);
        return true;
    }
    return false;
}

// Packs separate metallic/roughness sources into one map (B=metal, G=rough,
// matching the shader), filling a missing source from its scalar factor.
// Nearest-samples each source to the larger of the two sizes.
bool PackMetalRough(const std::string& metalRef, const std::string& roughRef, f32 metalFactor,
                    f32 roughFactor, const fs::path& modelDir,
                    const std::vector<EmbeddedTexture>& embedded, const fs::path& out) {
    std::vector<u8> mPix, rPix;
    u32 mw = 0, mh = 0, rw = 0, rh = 0;
    const bool haveM = DecodeRef(metalRef, modelDir, embedded, mPix, mw, mh);
    const bool haveR = DecodeRef(roughRef, modelDir, embedded, rPix, rw, rh);
    if (!haveM && !haveR) return false;
    const u32 w = std::max(haveM ? mw : 1u, haveR ? rw : 1u);
    const u32 h = std::max(haveM ? mh : 1u, haveR ? rh : 1u);

    uaf::Texture tex;
    tex.width = w;
    tex.height = h;
    tex.format = static_cast<u32>(rhi::Format::R8G8B8A8_UNORM);
    tex.mipCount = 1;
    tex.pixels.resize(static_cast<usize>(w) * h * 4);
    const auto toByte = [](f32 v) {
        return static_cast<u8>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
    };
    const u8 mConst = toByte(metalFactor);
    const u8 rConst = toByte(roughFactor);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const usize o = (static_cast<usize>(y) * w + x) * 4;
            u8 metal = mConst, rough = rConst;
            if (haveM) {
                const usize sx = static_cast<usize>(x) * mw / w;
                const usize sy = static_cast<usize>(y) * mh / h;
                metal = mPix[(sy * mw + sx) * 4]; // R channel of the greyscale map
            }
            if (haveR) {
                const usize sx = static_cast<usize>(x) * rw / w;
                const usize sy = static_cast<usize>(y) * rh / h;
                rough = rPix[(sy * rw + sx) * 4];
            }
            tex.pixels[o + 0] = 255;   // R unused (AO is a separate map)
            tex.pixels[o + 1] = rough; // G = roughness
            tex.pixels[o + 2] = metal; // B = metallic
            tex.pixels[o + 3] = 255;
        }
    }
    return uaf::WriteTexture(out, tex, GenGuid());
}

bool ImportModel(const fs::path& src, const fs::path& out) {
    Rig rig;
    std::vector<EmbeddedTexture> embedded;
    std::optional<Model> model = LoadModel(src.string(), &rig, &embedded);
    if (!model) return false;

    const fs::path modelDir = src.parent_path();
    const fs::path importDir = out.parent_path(); // folder being browsed
    // Refs are stored relative to the project's Assets ROOT (not bare names)
    // so they keep resolving after assets are organized into subfolders.
    const fs::path assetsRoot =
        Project::HasActive() ? Project::Active().AssetsDir() : importDir;
    const auto toRel = [&](const fs::path& p) {
        std::error_code ec;
        const fs::path rel = fs::relative(p, assetsRoot, ec);
        return (ec || rel.empty() || rel.native().starts_with(L".."))
                   ? p.filename().generic_string()
                   : rel.generic_string();
    };

    std::unordered_map<std::string, std::string> done; // source ref -> Assets-rel (or "")

    // Imports a referenced texture (external file) and rewrites the ref to the
    // resulting `.uaf`'s Assets-relative path. Dedupes across submeshes.
    auto importTex = [&](std::string& ref, bool srgb, tex::BCKind bcKind = tex::BCKind::ColorRGBA) {
        if (ref.empty()) return;
        const std::string original = ref;
        if (auto it = done.find(original); it != done.end()) { ref = it->second; return; }
        std::error_code ec;

        // Embedded texture (glb / embedded FBX) referenced as "*N".
        if (original[0] == '*') {
            const EmbeddedTexture* emb = nullptr;
            for (const EmbeddedTexture& e : embedded)
                if (e.name == original) { emb = &e; break; }
            if (!emb) {
                HBE_WARN("Importer: embedded texture '{}' missing from '{}'.", original,
                         src.filename().string());
                done[original] = "";
                ref.clear();
                return;
            }
            const fs::path uafPath =
                importDir / (out.stem().string() + "_tex" + original.substr(1) + ".uaf");
            if (!fs::exists(uafPath, ec) && !ImportEmbeddedTexture(*emb, uafPath, srgb, bcKind)) {
                done[original] = "";
                ref.clear();
                return;
            }
            done[original] = toRel(uafPath);
            ref = done[original];
            return;
        }

        // External file: try the URI as-authored, percent-decoded, by bare
        // filename, and in a conventional textures/ subfolder next to the model.
        const std::string decoded = PercentDecode(original);
        fs::path resolved;
        for (const fs::path& candidate :
             {modelDir / fs::path(original), modelDir / fs::path(decoded),
              modelDir / fs::path(decoded).filename(),
              modelDir / "textures" / fs::path(decoded).filename()}) {
            if (fs::exists(candidate, ec)) {
                resolved = candidate;
                break;
            }
        }
        if (resolved.empty()) {
            // Not on disk - it may be embedded under this filename (FBX media).
            const std::string base = fs::path(decoded).filename().generic_string();
            for (const EmbeddedTexture& e : embedded) {
                if (e.filename.empty() ||
                    fs::path(e.filename).filename().generic_string() != base) {
                    continue;
                }
                const fs::path uafPath = importDir / (fs::path(base).stem().string() + ".uaf");
                if (fs::exists(uafPath, ec) || ImportEmbeddedTexture(e, uafPath, srgb, bcKind)) {
                    done[original] = toRel(uafPath);
                    ref = done[original];
                    return;
                }
            }
            HBE_WARN("Importer: texture '{}' not found near '{}'.", decoded,
                     modelDir.string());
            done[original] = "";
            ref.clear();
            return;
        }
        const std::string uafName = resolved.stem().string() + ".uaf";
        const fs::path uafPath = importDir / uafName;
        if (!fs::exists(uafPath, ec) && !ImportTexture(resolved, uafPath, srgb, bcKind)) {
            done[original] = "";
            ref.clear();
            return;
        }
        done[original] = toRel(uafPath);
        ref = done[original];
    };

    u32 packIndex = 0;
    for (MeshData& md : *model) {
        Material& m = md.material;
        // Separate metallic/roughness greyscale maps -> pack into one mrTex.
        if (!m.metallicTex.empty() || !m.roughnessTex.empty()) {
            const std::string key = "mr|" + m.metallicTex + "|" + m.roughnessTex;
            if (auto it = done.find(key); it != done.end()) {
                m.mrTex = it->second;
            } else {
                const fs::path packed =
                    importDir / (out.stem().string() + "_mr" + std::to_string(packIndex++) +
                                 ".uaf");
                if (PackMetalRough(m.metallicTex, m.roughnessTex, m.metallic, m.roughness,
                                   modelDir, embedded, packed)) {
                    m.mrTex = toRel(packed);
                    done[m.mrTex] = m.mrTex; // Assets-rel already; importTex leaves it
                    m.metallic = 1.0f;        // the packed texture now carries the values
                    m.roughness = 1.0f;
                } else {
                    HBE_WARN("Importer: could not pack metallic/roughness for material '{}'.",
                             m.name);
                    m.mrTex.clear();
                }
                done[key] = m.mrTex;
            }
            m.metallicTex.clear();
            m.roughnessTex.clear();
        }
        importTex(m.baseColorTex, true, tex::BCKind::ColorRGBA); // sRGB color -> BC3
        importTex(m.normalTex, false, tex::BCKind::NormalRG);    // tangent normals -> BC5 (+shader Z)
        // Metal-roughness (G=rough, B=metal are INDEPENDENT) and occlusion are left uncompressed:
        // BC3 forces both MR channels onto one endpoint line (banding + cross-channel bleed), and a
        // clean BC5 remap would need a lockstep shader/packing migration. Quality > the small saving.
        importTex(m.mrTex, false, tex::BCKind::None);
        importTex(m.aoTex, false, tex::BCKind::None);
        importTex(m.emissiveTex, true, tex::BCKind::ColorRGBA);  // emissive sRGB -> BC3
    }

    // Generate a `.hbmat` asset per unique material so the model's materials
    // are editable, reusable assets; submeshes remember theirs by ref.
    {
        const fs::path matDir = importDir / (out.stem().string() + "_Materials");
        std::unordered_map<std::string, std::string> matDone; // mat name -> rel
        u32 unnamed = 0;
        for (MeshData& md : *model) {
            std::string name = md.material.name.empty()
                                   ? "Material_" + std::to_string(unnamed++)
                                   : md.material.name;
            name = SanitizeFileName(name);
            if (auto it = matDone.find(name); it != matDone.end()) {
                md.material.materialAsset = it->second;
                continue;
            }
            MaterialAsset mat;
            mat.name = name;
            mat.surface.base_color = md.material.baseColor;
            mat.surface.base_metalness = md.material.metallic;
            mat.surface.specular_roughness = md.material.roughness;
            mat.surface.emission_color = md.material.emissive;
            // KHR glTF extensions -> OpenPBR. Absent extensions carry OpenPBR-default factors
            // (ior 1.5 / specular 1 / transmission 0 / anisotropy 0), so this is a no-op for plain
            // metallic-roughness and non-glTF models.
            mat.surface.specular_ior = md.material.ior;
            mat.surface.specular_weight = md.material.specularFactor;
            mat.surface.transmission_weight = md.material.transmission;
            mat.surface.specular_roughness_anisotropy = md.material.anisotropy;
            if (md.material.clearcoat > 0.0f) {
                mat.surface.coat_weight = md.material.clearcoat;
                mat.surface.coat_roughness = md.material.clearcoatRoughness;
            }
            // KHR_materials_sheen -> OpenPBR fuzz (presence gives it full weight).
            if (md.material.sheenColor != glm::vec3(0.0f) || md.material.sheenRoughness > 0.0f) {
                mat.surface.fuzz_weight = 1.0f;
                mat.surface.fuzz_color = md.material.sheenColor;
                mat.surface.fuzz_roughness = md.material.sheenRoughness;
            }
            // KHR_materials_volume -> absorptive (non-thin-walled) glass with attenuation.
            if (md.material.volumeThickness > 0.0f) {
                mat.surface.thin_walled = false;
                mat.surface.transmission_color = md.material.attenuationColor;
                // Finite guard: a +Infinity attenuation distance (glTF "no absorption") must NOT reach
                // transmission_depth - a non-finite float serialises to JSON null and throws on reload.
                if (std::isfinite(md.material.attenuationDistance) && md.material.attenuationDistance > 0.0f)
                    mat.surface.transmission_depth = md.material.attenuationDistance;
            }
            // A transmissive material must render in the alpha-blended pass (where the OpenPBR
            // transmission/refraction path runs); flag it so it is not drawn as an opaque.
            if (md.material.transmission > 0.0f)
                mat.flags |= rhi::MaterialFlag_Transparent;
            mat.albedoTex = md.material.baseColorTex;
            mat.normalTex = md.material.normalTex;
            mat.mrTex = md.material.mrTex;
            mat.aoTex = md.material.aoTex;
            mat.emissiveTex = md.material.emissiveTex;
            const fs::path matPath = matDir / (name + assets::kMaterialExtension);
            if (assets::SaveMaterial(matPath, mat)) {
                matDone[name] = toRel(matPath);
                md.material.materialAsset = matDone[name];
            }
        }
        if (!matDone.empty()) {
            HBE_INFO("Importer: generated {} material asset(s) in '{}'.",
                     matDone.size(), matDir.string());
        }
    }

    // Reorder each submesh's geometry for the GPU (vertex cache / overdraw / fetch) before
    // it is written. Deterministic; morph-target deltas ride along through the same remap.
    // Materials above only touch refs, never geometry, so this is safe to run last.
    for (MeshData& md : *model)
        mesh::OptimizeForGpu(md);

    // Distance LODs (v9), generated NON-DESTRUCTIVELY alongside LOD0 (the reordered source).
    // Opt-in project setting (default on). Skinned models are excluded whole (hero quality +
    // Simplify welds UV seams); morph submeshes are excluded per-submesh inside BuildLodChain.
    // Deterministic, so re-import reproduces byte-identical LODs.
    const bool genLods =
        Project::HasActive() && Project::Active().Settings().meshLodEnabled && !rig.Valid();
    if (genLods) {
        u32 lodded = 0;
        for (MeshData& md : *model) {
            mesh::BuildLodChain(md); // no-op for morph submeshes / already-tiny meshes
            if (!md.lods.empty()) ++lodded;
        }
        if (lodded > 0)
            HBE_INFO("Importer: generated distance LODs for {}/{} submesh(es) of '{}'.",
                     lodded, model->size(), out.filename().string());
    }

    return uaf::WriteMesh(out, *model, GenGuid(), rig.Valid() ? &rig : nullptr);
}

// Stores the COMPRESSED SOURCE bytes (WAV / MP3 / FLAC) in the `.uaf`, not decoded PCM:
// an mp3/flac source is ~8-10x smaller on disk than its decoded PCM, and the runtime
// decodes back to PCM on load (uaf::ReadAudio). The source is validated (and its channel
// count / sample rate read) by opening a decoder over the bytes - no full decode needed
// at import. WAV sources are already-uncompressed PCM, so they don't shrink at this layer,
// but the pack's zstd handles those; the big win is the compressed formats.
bool ImportAudio(const fs::path& src, const fs::path& out) {
    std::ifstream in(src, std::ios::binary | std::ios::ate);
    if (!in) {
        HBE_ERROR("Importer: cannot open audio '{}'.", src.string());
        return false;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<u8> bytes(static_cast<usize>(size));
    if (size) in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (bytes.empty()) {
        HBE_ERROR("Importer: audio '{}' is empty.", src.string());
        return false;
    }

    // Validate + read metadata by opening a decoder over the source bytes.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) {
        HBE_ERROR("Importer: cannot decode audio '{}' (unsupported/corrupt).", src.string());
        return false;
    }
    uaf::Audio audio;
    audio.channels = dec.outputChannels;
    audio.sampleRate = dec.outputSampleRate;
    audio.bitsPerSample = 16;
    ma_decoder_uninit(&dec);
    if (audio.channels == 0 || audio.sampleRate == 0) {
        HBE_ERROR("Importer: '{}' has no decodable audio.", src.string());
        return false;
    }

    const std::string e = LowerExt(src);
    audio.encodedFormat = (e == ".wav") ? 1u : (e == ".mp3") ? 2u : (e == ".flac") ? 3u : 0u;
    audio.encoded = std::move(bytes); // WriteAudio stores this (v11), decoded on load
    return uaf::WriteAudio(out, audio, GenGuid());
}

} // namespace

bool IsSupportedSource(const fs::path& path) {
    const std::string e = LowerExt(path);
    return IsImage(e) || IsModel(e) || IsAudio(e) || IsFont(e);
}

std::optional<fs::path> Import(const fs::path& src, const fs::path& assetsDir) {
    const std::string e = LowerExt(src);
    // THE moment a pack slot is minted. Everything this call writes gets an id
    // before it returns - and that is deliberately measured by "written at or
    // after this mark" rather than "the file we return", because one model import
    // also writes a `.uaf` per texture and a `.hbmat` per material that the
    // caller never sees. Files older than the mark are never touched.
    const auto importMark = slots::MarkNow();
    fs::path out = assetsDir / (src.stem().string() + ".uaf");
    // The output name is the source STEM, so different sources sharing a base name
    // (e.g. track.mp3 + track.wav, or model.glb + model.png) map to one .uaf and the
    // later import overwrites the earlier. Warn so a folder-drop doesn't lose an asset
    // silently (a same-source re-import to update the asset is the intended overwrite).
    std::error_code existEc;
    if (fs::exists(out, existEc))
        HBE_WARN("Importer: '{}' overwrites existing '{}' (same asset name).", src.filename().string(),
                 out.filename().string());

    bool ok = false;
    if (IsImage(e))      ok = ImportTexture(src, out, true); // assume a color texture
    else if (IsModel(e)) ok = ImportModel(src, out);
    else if (IsAudio(e)) ok = ImportAudio(src, out);
    else if (IsFont(e))  ok = ImportFont(src, out);
    else {
        HBE_WARN("Importer: unsupported source '{}'.", src.string());
        return std::nullopt;
    }

    if (!ok) return std::nullopt;
    // Assign the pack slot(s). A RE-import keeps the number the manifest
    // remembers for that path (overwriting a `.uaf` clears its header field), so
    // re-importing a texture to fix its gamma does not reshuffle the packs.
    if (Project::HasActive()) {
        // `assetsDir` is the DESTINATION folder, which is routinely a subfolder of
        // the project's Assets/ (the asset browser imports into wherever you are).
        // Pack keys are relative to the ROOT, always, so stamp from there.
        const fs::path assetsRoot = Project::Active().AssetsDir();
        std::error_code rec;
        const std::string rel = fs::relative(assetsDir, assetsRoot, rec).generic_string();
        if (!rec && !rel.empty() && rel.rfind("..", 0) != 0) {
            const u32 n = slots::StampNewAssets(assetsRoot,
                                                Project::Active().SlotManifestPath(), importMark);
            if (n > 1) HBE_INFO("Importer: assigned pack slots to {} new asset(s).", n);
        }
    }
    HBE_INFO("Importer: '{}' -> '{}'", src.filename().string(), out.filename().string());
    return out;
}

UpgradeReport UpgradeAssets(const fs::path& assetsDir, const fs::path& manifestPath) {
    UpgradeReport rep;
    std::error_code ec;
    if (!fs::exists(assetsDir, ec)) return rep;

    const bool genLods = Project::HasActive() && Project::Active().Settings().meshLodEnabled;
    const bool bcTex = Project::HasActive() && Project::Active().Settings().textureCompression;

    // Material texture roles gathered from EVERY mesh (a loose texture `.uaf` carries no role, so
    // the BC bake must learn baseColor-vs-normal from the materials that reference it). First role
    // wins; MR/AO are deliberately never noted (kept uncompressed).
    std::unordered_map<std::string, tex::BCKind> texRoles;
    const auto noteRole = [&](const std::string& ref, tex::BCKind kind) {
        if (!ref.empty()) texRoles.emplace(ref, kind);
    };

    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const fs::path& p = it->path();
        if (p.extension() != ".uaf") continue;
        if (p.stem().extension() == ".bc") continue; // a generated `foo.bc.uaf` - its source drives it
        uaf::AssetType type = uaf::AssetType::Unknown;
        u32 version = 0;
        u64 guid = 0;
        if (!uaf::PeekHeader(p, type, version, guid)) continue;
        ++rep.scanned;
        if (type != uaf::AssetType::Mesh) continue;

        // A current mesh only needs reading when the texture pass wants its material roles;
        // otherwise skip the (potentially large) payload load entirely.
        const bool outdated = version < uaf::kVersion;
        if (!outdated && !bcTex) continue;

        std::optional<Model> model = uaf::ReadMesh(p);
        if (!model) continue;
        if (bcTex) {
            for (const MeshData& md : *model) {
                noteRole(md.material.baseColorTex, tex::BCKind::ColorRGBA);
                noteRole(md.material.normalTex, tex::BCKind::NormalRG);
                noteRole(md.material.emissiveTex, tex::BCKind::ColorRGBA);
            }
        }
        if (!outdated) continue;

        // Re-write at the current version, PRESERVING identity: guid + rig round-trip through
        // WriteMesh. The pack slot embedded in the file is the TIER-1 authority (it lives in the
        // asset precisely so the manifest can be absent/stale on a fresh clone), and WriteMesh
        // resets it to unassigned - so capture it FIRST and restore it verbatim afterwards, rather
        // than re-deriving from the manifest (which would renumber the asset and reshuffle packs
        // whenever the manifest is missing). Fall back to StampAsset only if it truly had no slot.
        const u32 oldSlot = slots::ReadSlot(p).value_or(slots::kUnassigned);
        std::optional<Rig> rig = uaf::ReadRig(p);
        const bool skinned = rig && rig->Valid();
        if (genLods && !skinned)
            for (MeshData& md : *model)
                mesh::BuildLodChain(md); // no-op for morph submeshes
        if (uaf::WriteMesh(p, *model, guid, skinned ? &*rig : nullptr)) {
            if (oldSlot != slots::kUnassigned)
                slots::WriteSlot(p, oldSlot); // restore the asset's own id, manifest-independent
            else
                slots::StampAsset(assetsDir, manifestPath, p); // never had one -> assign fresh
            ++rep.meshesUpgraded;
        }
    }

    // Bake a role-correct BC sibling for any material texture that lacks one (opt-in).
    if (bcTex) {
        for (const auto& [ref, kind] : texRoles) {
            const fs::path texPath = assetsDir / fs::path(ref);
            const fs::path bcPath = texPath.parent_path() / (texPath.stem().string() + ".bc.uaf");
            if (fs::exists(bcPath, ec)) continue; // already baked
            std::optional<uaf::Texture> t = uaf::ReadTexture(texPath);
            if (!t) continue;
            const bool srgb = static_cast<rhi::Format>(t->format) == rhi::Format::R8G8B8A8_SRGB;
            MaybeBakeBC(*t, texPath, kind, srgb); // writes .bc.uaf when enabled + RGBA8 + mult-4
            if (fs::exists(bcPath, ec)) {
                slots::StampAsset(assetsDir, manifestPath, bcPath);
                ++rep.texturesBaked;
            }
        }
    }

    if (rep.changedAnything())
        HBE_INFO("Assets: upgraded {} mesh(es) to v{} + baked {} BC texture(s) ({} .uaf scanned).",
                 rep.meshesUpgraded, uaf::kVersion, rep.texturesBaked, rep.scanned);
    return rep;
}

bool UpgradeSelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "upgrade FAIL: %s\n", what); ++fails; }
    };
    const fs::path dir = fs::temp_directory_path() / "hbe_upgrade_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path manifest = dir / "slots.uapmanifest";
    const fs::path meshPath = dir / "mesh.uaf";

    // A 16x16 subdivided quad - enough triangles that BuildLodChain can actually reduce.
    MeshData md;
    constexpr int N = 16;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            Vertex v;
            v.position = {static_cast<f32>(x) / N, 0.0f, static_cast<f32>(y) / N};
            v.normal = {0.0f, 1.0f, 0.0f};
            md.vertices.push_back(v);
        }
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            const u32 i0 = y * (N + 1) + x, i1 = i0 + 1, i2 = i0 + (N + 1), i3 = i2 + 1;
            md.indices.insert(md.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    const u64 guid = 0x1234ABCDu;
    // Write a GENUINE v9 mesh (raw 72-byte-Vertex geometry - the pre-v10 layout). Since
    // v10 changed the on-disk GEOMETRY encoding (quantized + meshopt), a "stale" fixture
    // can no longer be faked by patching the version byte of a current file: the reader
    // would use the v9 raw path on v10-encoded bytes. Emitting real v9 bytes exercises the
    // actual in-place v9 -> v10 migration users hit on project open. This mirrors the v9
    // WriteMesh/ReadMaterial layout exactly (single static submesh, no morphs/LODs/rig).
    const auto writeLegacyV9 = [](const fs::path& path, const MeshData& sm, u64 g) {
        BinaryWriter w;
        w.Bytes("UAF1", 4);
        w.Pod<u32>(9u | 0x80000000u);   // v9 payload, slot-field reserved (uaf::kSlotFlag)
        w.Pod<u32>(2u);                 // uaf::AssetType::Mesh
        w.Pod<u64>(g);
        w.Pod<u32>(0xFFFFFFFFu);        // slots::kUnassigned (StampAsset fills it in)
        w.Pod<u32>(1u);                 // one submesh
        w.Vec(sm.vertices);             // v5+ raw vertices
        w.Vec(sm.indices);
        w.Pod(sm.material.baseColor);   // v4 material field set
        w.Pod(sm.material.metallic);
        w.Pod(sm.material.roughness);
        w.Str(sm.material.name);
        w.Str(sm.material.baseColorTex);
        w.Str(sm.material.normalTex);
        w.Str(sm.material.mrTex);
        w.Str(sm.material.aoTex);
        w.Pod(sm.material.emissive);
        w.Str(sm.material.emissiveTex);
        w.Str(sm.material.materialAsset);
        w.Str(sm.name);
        w.Pod<u32>(0u);                 // morph targets
        w.Pod<u32>(0u);                 // distance LODs
        w.Pod<u32>(0u);                 // hasRig
        return w.SaveToFile(path);
    };
    check(writeLegacyV9(meshPath, md, guid), "write a genuine v9 mesh fixture");
    const u32 slot = slots::StampAsset(dir, manifest, meshPath); // assign + embed a pack slot
    check(slot != slots::kUnassigned, "the fixture mesh must get a pack slot");
    // DELETE the manifest so the test proves the EMBEDDED slot (tier-1) is preserved on its own -
    // the exact fresh-clone / missing-manifest condition the wipe-then-restamp bug renumbered under.
    fs::remove(manifest, ec);

    uaf::AssetType ty = uaf::AssetType::Unknown;
    u32 ver = 0;
    u64 g = 0;
    check(uaf::PeekHeader(meshPath, ty, ver, g) && ver == 9 && g == guid,
          "the fixture must peek as v9 with its guid intact");

    const UpgradeReport rep = UpgradeAssets(dir, manifest);
    check(rep.meshesUpgraded == 1, "exactly one mesh must be upgraded");

    // Now current version, guid + slot preserved, and (LODs enabled) LODs generated.
    u32 ver2 = 0;
    u64 g2 = 0;
    check(uaf::PeekHeader(meshPath, ty, ver2, g2), "upgraded mesh must still read");
    check(ver2 == uaf::kVersion, "mesh must now be at the current version");
    check(g2 == guid, "guid must be preserved across the upgrade");
    check(slots::ReadSlot(meshPath).value_or(slots::kUnassigned) == slot,
          "the embedded pack slot must survive the upgrade even with the manifest deleted");
    if (const std::optional<Model> back = uaf::ReadMesh(meshPath);
        back && back->size() == 1 && Project::HasActive() &&
        Project::Active().Settings().meshLodEnabled) {
        check(!(*back)[0].lods.empty(), "an upgraded eligible mesh must gain LODs");
    }

    const UpgradeReport rep2 = UpgradeAssets(dir, manifest);
    check(rep2.meshesUpgraded == 0, "a second upgrade of a current project must be a no-op");

    fs::remove_all(dir, ec);
    if (fails == 0)
        std::printf("upgrade: an out-of-date mesh migrates to v%u in place, keeps its guid + pack "
                    "slot, still reads back, and re-running is a no-op\n", uaf::kVersion);
    return fails == 0;
}

} // namespace hbe::importer

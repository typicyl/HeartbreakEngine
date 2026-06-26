// Editor/Importer.cpp
#include "Editor/Importer.h"

#include "Assets/MaterialAsset.h"
#include "Assets/ModelLoader.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Project/Project.h"
#include "RHI/RHI.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
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

std::string LowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

bool IsImage(const std::string& e) {
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" ||
           e == ".psd" || e == ".gif" || e == ".hdr";
}
bool IsModel(const std::string& e) {
    return e == ".gltf" || e == ".glb" || e == ".obj" || e == ".fbx" || e == ".dae" || e == ".ply";
}
bool IsAudio(const std::string& e) { return e == ".wav"; }
bool IsFont(const std::string& e) { return e == ".ttf" || e == ".otf"; }

// Fonts are stored verbatim (the engine bakes atlases at load time).
bool ImportFont(const fs::path& src, const fs::path& out) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    return uaf::WriteFont(out, bytes, GenGuid());
}

bool ImportTexture(const fs::path& src, const fs::path& out, bool srgb) {
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

    return uaf::WriteTexture(out, tex, GenGuid());
}

// Writes a texture embedded in a model file (glb / embedded FBX) to `.uaf`.
// Compressed payloads (PNG/JPG bytes) are decoded from memory; raw payloads are
// already RGBA8.
bool ImportEmbeddedTexture(const EmbeddedTexture& emb, const fs::path& out, bool srgb) {
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
    auto importTex = [&](std::string& ref, bool srgb) {
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
            if (!fs::exists(uafPath, ec) && !ImportEmbeddedTexture(*emb, uafPath, srgb)) {
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
                if (fs::exists(uafPath, ec) || ImportEmbeddedTexture(e, uafPath, srgb)) {
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
        if (!fs::exists(uafPath, ec) && !ImportTexture(resolved, uafPath, srgb)) {
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
        importTex(m.baseColorTex, true); // sRGB
        importTex(m.normalTex, false);
        importTex(m.mrTex, false);
        importTex(m.aoTex, false);
        importTex(m.emissiveTex, true); // emissive maps are sRGB-authored
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
            mat.baseColor = md.material.baseColor;
            mat.metallic = md.material.metallic;
            mat.roughness = md.material.roughness;
            mat.emissiveColor = md.material.emissive;
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

    return uaf::WriteMesh(out, *model, GenGuid(), rig.Valid() ? &rig : nullptr);
}

// Minimal RIFF/WAVE PCM parser.
bool ImportAudio(const fs::path& src, const fs::path& out) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        HBE_ERROR("Importer: '{}' is not a RIFF/WAVE file.", src.string());
        return false;
    }

    uaf::Audio audio;
    usize pos = 12;
    bool haveFmt = false, haveData = false;
    while (pos + 8 <= bytes.size()) {
        char id[4];
        std::memcpy(id, bytes.data() + pos, 4);
        u32 size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);
        const usize body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= bytes.size()) {
            u16 ch = 0, bits = 0;
            u32 rate = 0;
            std::memcpy(&ch, bytes.data() + body + 2, 2);
            std::memcpy(&rate, bytes.data() + body + 4, 4);
            std::memcpy(&bits, bytes.data() + body + 14, 2);
            audio.channels = ch;
            audio.sampleRate = rate;
            audio.bitsPerSample = bits;
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const usize n = std::min<usize>(size, bytes.size() - body);
            audio.pcm.assign(bytes.begin() + body, bytes.begin() + body + n);
            haveData = true;
        }
        pos = body + size + (size & 1); // chunks are word-aligned
    }
    if (!haveFmt || !haveData) {
        HBE_ERROR("Importer: '{}' missing fmt/data chunk.", src.string());
        return false;
    }
    return uaf::WriteAudio(out, audio, GenGuid());
}

} // namespace

bool IsSupportedSource(const fs::path& path) {
    const std::string e = LowerExt(path);
    return IsImage(e) || IsModel(e) || IsAudio(e) || IsFont(e);
}

std::optional<fs::path> Import(const fs::path& src, const fs::path& assetsDir) {
    const std::string e = LowerExt(src);
    fs::path out = assetsDir / (src.stem().string() + ".uaf");

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
    HBE_INFO("Importer: '{}' -> '{}'", src.filename().string(), out.filename().string());
    return out;
}

} // namespace hbe::importer

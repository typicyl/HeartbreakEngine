// Assets/MaterialAsset.h - .hbmat material assets (full PBR).
//
// A material asset is a small JSON file under the project's Assets/ directory
// describing a complete metallic-roughness PBR material: factors, emissive,
// subsurface, and `.uaf` texture references (paths relative to Assets/).
// Scenes reference materials through the MaterialRef component; the editor
// edits them in the Asset Viewer and applies them to entities.
#pragma once

#include "Assets/AcousticMaterial.h"
#include "Core/Types.h"
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams (OpenPBR material values)

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace hbe {

class Renderer;
struct MeshInstance;

namespace rhi { struct TextureHandle; }

struct MaterialAsset {
    std::string name = "Material";
    // Physically-based material VALUES (OpenPBR Surface parameter set). The legacy flat
    // fields (baseColor/metallic/roughness/emissive*/subsurface*/clearcoat*) now live here
    // under OpenPBR names; see RHI/SurfaceMaterial.h for the legacy->OpenPBR mapping. The
    // subsurface tint default stays the deep-crimson blood colour the Skin preset relies on.
    SurfaceParams surface;
    u32 flags = 0; // rhi::MaterialFlags
    // `.uaf` texture references, relative to the project's Assets/ (empty = none).
    std::string albedoTex;
    std::string normalTex;
    std::string mrTex;
    std::string aoTex;
    std::string emissiveTex;
    std::string thicknessTex; // SSS transmission thickness (skin)

    // Acoustic properties for physically-informed audio (absorption/scattering/transmission).
    // `acoustic` is authoritative + serialized; `acousticPreset` is a UI label naming the
    // preset it came from ("Custom" once hand-edited, "Default" for un-authored materials).
    AcousticMaterial acoustic;
    std::string acousticPreset = "Default";
};

namespace assets {

inline constexpr const char* kMaterialExtension = ".hbmat";

bool SaveMaterial(const std::filesystem::path& path, const MaterialAsset& mat);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<MaterialAsset> LoadMaterial(const std::filesystem::path& path);

// Copies the material onto a MeshInstance, uploading referenced textures via
// `renderer`. `texCache` (rel path -> handle) avoids duplicate uploads; pass
// the same map across calls within one load/session.
void ApplyMaterial(Renderer& renderer, const std::filesystem::path& assetsDir,
                   const MaterialAsset& mat, MeshInstance& instance,
                   std::unordered_map<std::string, rhi::TextureHandle>& texCache);

} // namespace assets
} // namespace hbe

// Assets/MaterialAsset.h - .hbmat material assets (full PBR).
//
// A material asset is a small JSON file under the project's Assets/ directory
// describing a complete metallic-roughness PBR material: factors, emissive,
// subsurface, and `.uaf` texture references (paths relative to Assets/).
// Scenes reference materials through the MaterialRef component; the editor
// edits them in the Asset Viewer and applies them to entities.
#pragma once

#include "Core/Types.h"

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
    glm::vec4 baseColor{1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    glm::vec3 emissiveColor{0.0f};
    f32 emissiveIntensity = 1.0f;
    // Scatter/transmission tint. A deep crimson reads as blood under the skin; the old
    // {1.0, 0.3, 0.2} was a sunburn ORANGE that looked artificial (the "Skin" preset sets
    // this too). Only the transmission + no-LUT fallback use it; the pre-integrated LUT
    // carries its own d'Eon profile.
    glm::vec3 subsurfaceColor{0.85f, 0.2f, 0.16f};
    f32 subsurfaceRadius = 1.0f; // SSS scatter scale (skin)
    f32 clearcoat = 0.0f;        // wet/oily clear layer strength (0 = off)
    f32 clearcoatRoughness = 0.08f; // clear layer roughness (low = wet/glossy)
    u32 flags = 0; // rhi::MaterialFlags
    // `.uaf` texture references, relative to the project's Assets/ (empty = none).
    std::string albedoTex;
    std::string normalTex;
    std::string mrTex;
    std::string aoTex;
    std::string emissiveTex;
    std::string thicknessTex; // SSS transmission thickness (skin)
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

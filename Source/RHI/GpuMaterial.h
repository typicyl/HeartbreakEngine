// RHI/GpuMaterial.h - GPU-side OpenPBR material block appended to the per-object constant buffer.
//
// P2 (OpenPBR shading) note. ObjectConstants already carries the metallic-roughness-era material
// values (base color / metalness / roughness / emissive / subsurface color+radius / coat weight+
// roughness) plus the bindless texture indices, and MANY shaders read those field positions
// (MeshPBR, PBR, StrokeSurface, Water). Re-laying-out that buffer is the codebase's #1 silent-
// corruption bug class, so instead of moving those fields we APPEND the OpenPBR parameters that had
// no representation yet as one tail block - the same zero-init-keeps-ABI pattern used for clearcoat/
// paint/morph. The shader then GATHERS the legacy fields + this block into one OpenPBRMaterial view
// (Shaders/OpenPBRMaterial.hlsli). Defining the struct ONCE here and embedding it in both the D3D12
// ObjectCB and the Vulkan ObjectUBO guarantees the two backends stay byte-identical for this block.
//
// Layout: std140-style 16-byte rows. Every glm::vec3 sits at the START of a 16-byte row followed by
// a scalar, so HLSL cbuffer packing matches this C++ layout field-for-field (a vec3 never straddles
// a 16-byte boundary). Keep that discipline when editing. The static_assert guards the total size.
#pragma once

#include "Core/Types.h"
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams (the CPU authoring form this is packed from)

#include <glm/glm.hpp>

namespace hbe::rhi {

// OpenPBR parameters not already present in ObjectConstants' legacy material fields. Mirrors
// `struct OpenPBRMaterialExt` in Shaders/OpenPBRMaterial.hlsli. 11 rows * 16 = 176 bytes.
struct GpuSurfaceMaterialExt {
    // row 0
    f32 geometry_opacity = 1.0f;       // split out of base_color.a (OpenPBR geometry_opacity)
    f32 base_weight = 1.0f;
    f32 base_diffuse_roughness = 0.0f; // Oren-Nayar (0 = Lambert)
    f32 specular_weight = 1.0f;
    // row 1
    glm::vec3 specular_color{1.0f}; f32 specular_ior = 1.5f;
    // row 2
    f32 specular_roughness_anisotropy = 0.0f;
    f32 specular_anisotropy_rotation = 0.0f;
    f32 coat_ior = 1.5f;
    f32 coat_affect_color = 0.0f;
    // row 3
    glm::vec3 coat_color{1.0f}; f32 coat_affect_roughness = 0.0f;
    // row 4
    f32 coat_roughness_anisotropy = 0.0f;
    f32 fuzz_weight = 0.0f;
    f32 fuzz_roughness = 0.5f;
    f32 subsurface_weight = 0.0f;
    // row 5
    glm::vec3 fuzz_color{1.0f}; f32 subsurface_scatter_anisotropy = 0.0f;
    // row 6
    glm::vec3 subsurface_radius_scale{1.0f, 0.5f, 0.25f}; f32 transmission_weight = 0.0f;
    // row 7
    glm::vec3 transmission_color{1.0f}; f32 transmission_depth = 0.0f;
    // row 8
    glm::vec3 transmission_scatter{0.0f}; f32 transmission_scatter_anisotropy = 0.0f;
    // row 9
    f32 transmission_dispersion_scale = 0.0f;
    f32 transmission_dispersion_abbe_number = 20.0f;
    f32 thin_film_weight = 0.0f;
    f32 thin_film_thickness = 0.0f;
    // row 10
    f32 thin_film_ior = 1.33f;
    f32 thin_walled = 0.0f; // bool as float (1 = thin-walled)
    f32 _padExt0 = 0.0f;
    f32 _padExt1 = 0.0f;
};
static_assert(sizeof(GpuSurfaceMaterialExt) == 176,
              "GpuSurfaceMaterialExt must be 11 x 16 bytes to match OpenPBRMaterialExt (HLSL)");

// Packs the appended OpenPBR parameters from the CPU SurfaceParams. The legacy fields
// (base_color/metalness/roughness/emissive/subsurface/coat) are still packed by the existing
// per-field code in FillObjectMaterial; this only fills the new tail block.
inline void FillSurfaceMaterialExt(GpuSurfaceMaterialExt& e, const SurfaceParams& s) {
    e.geometry_opacity = s.base_color.a; // opacity split out of the legacy vec4 base colour
    e.base_weight = s.base_weight;
    e.base_diffuse_roughness = s.base_diffuse_roughness;
    e.specular_weight = s.specular_weight;
    e.specular_color = s.specular_color;
    e.specular_ior = s.specular_ior;
    e.specular_roughness_anisotropy = s.specular_roughness_anisotropy;
    e.specular_anisotropy_rotation = s.specular_anisotropy_rotation;
    e.coat_ior = s.coat_ior;
    e.coat_affect_color = s.coat_affect_color;
    e.coat_color = s.coat_color;
    e.coat_affect_roughness = s.coat_affect_roughness;
    e.coat_roughness_anisotropy = s.coat_roughness_anisotropy;
    e.fuzz_weight = s.fuzz_weight;
    e.fuzz_roughness = s.fuzz_roughness;
    e.subsurface_weight = s.subsurface_weight;
    e.fuzz_color = s.fuzz_color;
    e.subsurface_scatter_anisotropy = s.subsurface_scatter_anisotropy;
    e.subsurface_radius_scale = s.subsurface_radius_scale;
    e.transmission_weight = s.transmission_weight;
    e.transmission_color = s.transmission_color;
    e.transmission_depth = s.transmission_depth;
    e.transmission_scatter = s.transmission_scatter;
    e.transmission_scatter_anisotropy = s.transmission_scatter_anisotropy;
    e.transmission_dispersion_scale = s.transmission_dispersion_scale;
    e.transmission_dispersion_abbe_number = s.transmission_dispersion_abbe_number;
    e.thin_film_weight = s.thin_film_weight;
    e.thin_film_thickness = s.thin_film_thickness;
    e.thin_film_ior = s.thin_film_ior;
    e.thin_walled = s.thin_walled ? 1.0f : 0.0f;
}

} // namespace hbe::rhi

// RHI/SurfaceMaterial.h - hbe::SurfaceParams: the authoritative CPU-side material
// parameter model, named to the OpenPBR Surface v1.1.1 specification.
//
// SurfaceParams is the single source of truth for a material's numeric VALUES. It is
// embedded BY VALUE in MaterialAsset (authoring / .hbmat), MeshInstance (scene component),
// and rhi::DrawItem (per-draw). Before this struct existed, those three structs each
// carried their own copies of baseColor/metallic/roughness/... - a four-way hand-mirror
// with no compile-time cross-check. Consolidating the numeric values here removes that
// duplication; see docs/Design-MaterialX-OpenPBR.md.
//
// TEXTURES ARE DELIBERATELY NOT HERE. Asset paths (MaterialAsset), rhi::TextureHandle
// (MeshInstance/DrawItem) and GPU bindless indices are genuinely different representations
// and stay layer-specific. SurfaceParams owns material VALUES only. materialFlags (the
// feature/PSO-routing bitmask) is likewise NOT here - it is a flags concern, not a value.
//
// P1 SCOPE (representation migration only): the renderer and shaders do NOT yet read the
// OpenPBR-only fields introduced here (base_weight, base_diffuse_roughness, specular_*,
// transmission_*, coat_color/ior/affect_*, coat/spec anisotropy, fuzz_*, thin_film_*,
// subsurface_weight/radius_scale/scatter_anisotropy, thin_walled). The existing GPU packers
// read only the legacy-equivalent fields (base_color, geometry_opacity, base_metalness,
// specular_roughness, subsurface_color/radius, coat_weight, coat_roughness, emission_*) and
// emit byte-identical ObjectConstants. OpenPBR shading of the new fields lands in P2.
//
// DEFAULTS follow two rules so P1 changes nothing visually:
//   * Legacy-mapped fields keep Heartbreak's PRE-EXISTING defaults (e.g. specular_roughness
//     = 0.5 not OpenPBR's 0.3; coat_roughness = 0.08 not 0.03; subsurface_color = deep
//     crimson) so migrated materials and default-constructed instances render identically.
//   * New OpenPBR-only fields use the OpenPBR spec defaults (data-only in P1 -> risk-free).
//
// LEGACY -> OpenPBR (SurfaceParams) MAPPING (also mirrored in the .hbmat loader):
//   baseColor            -> base_color   (glm::vec4; .rgb = OpenPBR base_color, .a = opacity)
//   metallic             -> base_metalness
//   roughness            -> specular_roughness
//   emissiveColor        -> emission_color
//   emissiveIntensity    -> emission_luminance
//   subsurfaceColor      -> subsurface_color
//   subsurfaceRadius     -> subsurface_radius
//   clearcoat            -> coat_weight
//   clearcoatRoughness   -> coat_roughness
//
// OPACITY: OpenPBR models geometry_opacity as a separate scalar, but in P1 the alpha stays
// carried in base_color.a exactly as the legacy baseColor.a was (the transparent path reads
// the same value). Splitting geometry_opacity out is deferred to P2 (the OpenPBR shading
// phase), per "do not change alpha/transparency behaviour until the shading phase".
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe {

// OpenPBR Surface parameter set (values only). Field names follow the OpenPBR spec so the
// mapping to MaterialX's open_pbr_surface node (import/export, P8) is 1:1. Grouped by the
// OpenPBR layering (base / specular / transmission / subsurface / coat / fuzz / thin-film /
// emission / geometry).
struct SurfaceParams {
    // --- Base (diffuse + metalness) ------------------------------------------------
    glm::vec4 base_color{1.0f};                  // LEGACY baseColor (rgb=base_color, a=opacity; HB default 1)
    f32       base_weight = 1.0f;                // NEW (OpenPBR base_weight)
    f32       base_metalness = 0.0f;             // LEGACY metallic
    f32       base_diffuse_roughness = 0.0f;     // NEW (Oren-Nayar; 0 = Lambert as today)

    // --- Specular (dielectric + shared microfacet) ---------------------------------
    f32       specular_weight = 1.0f;                 // NEW
    glm::vec3 specular_color{1.0f};                   // NEW
    f32       specular_roughness = 0.5f;              // LEGACY roughness (HB default 0.5; spec 0.3)
    f32       specular_roughness_anisotropy = 0.0f;   // NEW (0 = isotropic)
    f32       specular_anisotropy_rotation = 0.0f;    // NEW (0..1 turns of the tangent frame)
    f32       specular_ior = 1.5f;                    // NEW (1.5 -> F0 0.04, matching today's fixed F0)

    // --- Transmission (translucent base) -------------------------------------------
    f32       transmission_weight = 0.0f;             // NEW
    glm::vec3 transmission_color{1.0f};               // NEW (Beer's-law tint)
    f32       transmission_depth = 0.0f;              // NEW (0 = surface tint, no volume)
    glm::vec3 transmission_scatter{0.0f};             // NEW (single-scatter albedo)
    f32       transmission_scatter_anisotropy = 0.0f; // NEW (Henyey-Greenstein g)
    f32       transmission_dispersion_scale = 0.0f;   // NEW (0 = no dispersion)
    f32       transmission_dispersion_abbe_number = 20.0f; // NEW
    bool      thin_walled = false;                    // NEW (OpenPBR geometry_thin_walled)

    // --- Subsurface ----------------------------------------------------------------
    f32       subsurface_weight = 0.0f;               // NEW (legacy SSS was flag-gated, no weight)
    glm::vec3 subsurface_color{0.85f, 0.2f, 0.16f};   // LEGACY subsurfaceColor (deep-crimson default kept)
    f32       subsurface_radius = 1.0f;               // LEGACY subsurfaceRadius
    glm::vec3 subsurface_radius_scale{1.0f, 0.5f, 0.25f}; // NEW (OpenPBR Rayleigh default)
    f32       subsurface_scatter_anisotropy = 0.0f;   // NEW

    // --- Coat ----------------------------------------------------------------------
    f32       coat_weight = 0.0f;                     // LEGACY clearcoat
    glm::vec3 coat_color{1.0f};                       // NEW
    f32       coat_roughness = 0.08f;                 // LEGACY clearcoatRoughness (HB 0.08; spec 0.03)
    f32       coat_roughness_anisotropy = 0.0f;       // NEW
    f32       coat_ior = 1.5f;                        // NEW
    f32       coat_affect_color = 0.0f;               // NEW (coat darkening of base)
    f32       coat_affect_roughness = 0.0f;           // NEW

    // --- Fuzz ----------------------------------------------------------------------
    f32       fuzz_weight = 0.0f;                     // NEW (legacy cloth was a flag)
    glm::vec3 fuzz_color{1.0f};                       // NEW
    f32       fuzz_roughness = 0.5f;                  // NEW

    // --- Thin film -----------------------------------------------------------------
    f32       thin_film_weight = 0.0f;                // NEW
    f32       thin_film_thickness = 0.0f;             // NEW (micrometres)
    f32       thin_film_ior = 1.33f;                  // NEW

    // --- Emission ------------------------------------------------------------------
    glm::vec3 emission_color{0.0f};                   // LEGACY emissiveColor
    f32       emission_luminance = 1.0f;              // LEGACY emissiveIntensity

    // --- Geometry ------------------------------------------------------------------
    // (OpenPBR geometry_opacity is carried in base_color.a in P1; see the header note. It
    //  will become a distinct field in P2 when the OpenPBR shader consumes it separately.)

    // Exact value-equality over the WHOLE parameter set (used by Renderer::SameMaterial to
    // decide instancing). Comparing all fields is intentionally strict: two draws instance
    // together only if every material value matches, so a new OpenPBR param can never cause
    // a silent wrong-material merge.
    bool operator==(const SurfaceParams& o) const {
        return base_color == o.base_color && base_weight == o.base_weight &&
               base_metalness == o.base_metalness && base_diffuse_roughness == o.base_diffuse_roughness &&
               specular_weight == o.specular_weight && specular_color == o.specular_color &&
               specular_roughness == o.specular_roughness &&
               specular_roughness_anisotropy == o.specular_roughness_anisotropy &&
               specular_anisotropy_rotation == o.specular_anisotropy_rotation &&
               specular_ior == o.specular_ior &&
               transmission_weight == o.transmission_weight && transmission_color == o.transmission_color &&
               transmission_depth == o.transmission_depth && transmission_scatter == o.transmission_scatter &&
               transmission_scatter_anisotropy == o.transmission_scatter_anisotropy &&
               transmission_dispersion_scale == o.transmission_dispersion_scale &&
               transmission_dispersion_abbe_number == o.transmission_dispersion_abbe_number &&
               thin_walled == o.thin_walled &&
               subsurface_weight == o.subsurface_weight && subsurface_color == o.subsurface_color &&
               subsurface_radius == o.subsurface_radius && subsurface_radius_scale == o.subsurface_radius_scale &&
               subsurface_scatter_anisotropy == o.subsurface_scatter_anisotropy &&
               coat_weight == o.coat_weight && coat_color == o.coat_color &&
               coat_roughness == o.coat_roughness && coat_roughness_anisotropy == o.coat_roughness_anisotropy &&
               coat_ior == o.coat_ior && coat_affect_color == o.coat_affect_color &&
               coat_affect_roughness == o.coat_affect_roughness &&
               fuzz_weight == o.fuzz_weight && fuzz_color == o.fuzz_color && fuzz_roughness == o.fuzz_roughness &&
               thin_film_weight == o.thin_film_weight && thin_film_thickness == o.thin_film_thickness &&
               thin_film_ior == o.thin_film_ior &&
               emission_color == o.emission_color && emission_luminance == o.emission_luminance;
    }
    bool operator!=(const SurfaceParams& o) const { return !(*this == o); }
};

} // namespace hbe

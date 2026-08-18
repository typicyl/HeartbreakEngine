// RHI/MaterialCompiler.h - maps a material's OpenPBR parameters to a curated shader variant.
//
// P3 (shader specialization). The opaque forward pass binds one MeshPBR pixel-shader variant per
// draw so a material only pays for the lobes it actually uses (docs/Design-MaterialX-OpenPBR.md).
// ComputeShaderVariant is the "feature analysis" step: it inspects the active lobes (from the OpenPBR
// weights + the legacy shading-mode flags) and returns the CHEAPEST curated variant that fully covers
// them - or rhi::ShaderVariant::Full when the combination has no dedicated stripped variant, so a
// lobe is NEVER silently dropped. This is deliberately a small hand-picked routing table, not a 2^N
// permutation matrix.
//
// Lobes with no dedicated variant yet (anisotropy -> P4, transmission -> P6, thin-film -> P7) route to
// Full; when those phases add their variants they extend both the enum/cmake registration and the two
// branches marked below.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"             // rhi::ShaderVariant, rhi::MaterialFlags
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams

namespace hbe::material {

inline rhi::ShaderVariant ComputeShaderVariant(const SurfaceParams& s, u32 flags) {
    // Every dedicated variant covers EXACTLY ONE lobe over the base (Std covers none). So the rule
    // is: if the material has one covered lobe, use that variant; otherwise (0 lobes -> Std, or 2+
    // lobes, or any lobe without a dedicated variant) fall back to Full, which contains everything.
    // Hair and eye are ALSO counted as lobes here - an eye material with a wet-cornea coat, or a
    // wet-hair coat, must go to Full so the coat is not stripped (the Eye/Hair variants omit it).
    //
    // The legacy shading-mode flags keep pre-P9 flag-authored materials working; the OpenPBR weights
    // are the forward-looking path (the P9 editor sets them directly).
    const bool coat = s.coat_weight > 0.0f;
    const bool sss  = (flags & rhi::MaterialFlag_Subsurface) != 0u || s.subsurface_weight > 0.0f;
    const bool fuzz = (flags & rhi::MaterialFlag_Cloth) != 0u || s.fuzz_weight > 0.0f;
    const bool hair = (flags & rhi::MaterialFlag_Hair) != 0u;
    const bool eye  = (flags & rhi::MaterialFlag_Eye) != 0u;

    // Lobes parameterised on the GPU but not yet given a dedicated stripped variant: force Full so
    // they render correctly today (anisotropy -> P4, transmission -> P6, thin-film -> P7).
    const bool uncovered = s.specular_roughness_anisotropy > 0.0f ||
                           s.transmission_weight > 0.0f ||
                           s.thin_film_weight > 0.0f;

    const int lobes = static_cast<int>(coat) + static_cast<int>(sss) + static_cast<int>(fuzz) +
                      static_cast<int>(hair) + static_cast<int>(eye);
    if (uncovered || lobes >= 2) return rhi::ShaderVariant::Full; // 2+ lobes / uncovered: no combined variant
    if (lobes == 0)              return rhi::ShaderVariant::Std;   // base only (the common case)
    if (coat)                    return rhi::ShaderVariant::Coat;
    if (sss)                     return rhi::ShaderVariant::Sss;
    if (fuzz)                    return rhi::ShaderVariant::Fuzz;
    if (hair)                    return rhi::ShaderVariant::Hair;
    if (eye)                     return rhi::ShaderVariant::Eye;
    return rhi::ShaderVariant::Full; // unreachable, but keep the fallback explicit
}

} // namespace hbe::material

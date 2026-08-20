// MaterialLayered.hlsli - the GPU twin of the CPU material resolver (Source/Material).
//
// The runtime LIVE path (docs/Design-MaterialAuthoring.md, Part B.2) blends up to 4 material layers
// per pixel with the SAME math the offline CPU resolver (mat::Resolve) uses, so the two agree:
//   * size-independent projection (UV0 / World / Triplanar) -> no UV stretch as a surface/box grows
//   * reoriented normal mapping (RNM) for correct normal blending (never a raw tangent-space lerp)
//   * height-priority weighting (endpoints exact) with optional noise break-up
//
// Pure math (no bindless / no cbuffer). Bindless sampling stays in the caller (MeshPBR.hlsl) where
// gTextures[] is visible; this library supplies projection UVs + the blend operators. Compiles
// standalone on DXIL + SPIR-V via MaterialLayeredTest.hlsl (a --shader-compile gate).
//
// The projection-mode constants mirror mat::Space / mat::BoxProjection on the CPU EXACTLY.
#ifndef HBE_MATERIAL_LAYERED_HLSLI
#define HBE_MATERIAL_LAYERED_HLSLI

#include "Triplanar.hlsli"

#define HBE_PROJ_UV0       0 // mat::Space::UV0
#define HBE_PROJ_UV1       1 // mat::Space::UV1
#define HBE_PROJ_OBJECT    2 // mat::Space::Object
#define HBE_PROJ_WORLD     3 // mat::Space::World
#define HBE_PROJ_TRIPLANAR 4 // mat::Space::Triplanar

// Dominant-axis size-independent tiling UV. World/Object divide POSITION by tileMeters (never the
// brush/surface size), so a 1m tile stays 1m at any scale. Byte-for-byte matches BoxBrush::ProjectUV.
float2 LayeredProjectUV(int mode, float3 worldPos, float3 objectPos, float2 uv0, float2 uv1,
                        float3 normal, float3 tileMeters)
{
    if (mode == HBE_PROJ_UV0) return uv0;
    if (mode == HBE_PROJ_UV1) return uv1;
    float3 p = (mode == HBE_PROJ_OBJECT) ? objectPos : worldPos;
    float3 an = abs(normal);
    float2 uv, tile;
    if (an.y >= an.x && an.y >= an.z) { uv = p.xz; tile = tileMeters.xz; } // floor
    else if (an.x >= an.z)            { uv = p.zy; tile = tileMeters.zy; } // X wall
    else                              { uv = p.xy; tile = tileMeters.xy; } // Z wall
    // Match BoxBrush::ProjectUV EXACTLY: divide only when the tile axis is non-zero (preserving a
    // negative/flipped tile), never clamp the divisor - clamping diverged the live GPU from the
    // offline CPU bake for zero/negative/tiny tiles.
    uv.x = (tile.x != 0.0f) ? uv.x / tile.x : uv.x;
    uv.y = (tile.y != 0.0f) ? uv.y / tile.y : uv.y;
    return uv;
}

// Reoriented Normal Mapping - IDENTICAL to mat::BlendNormalRNM (the CPU oracle).
// RNM(base, flat) == base; RNM(flat, detail) == detail. `strength` fades the detail toward flat.
float3 LayeredBlendNormalRNM(float3 baseN, float3 detailN, float strength)
{
    float3 n2 = lerp(float3(0.0f, 0.0f, 1.0f), detailN, saturate(strength));
    float3 t = baseN + float3(0.0f, 0.0f, 1.0f);
    float3 u = n2 * float3(-1.0f, -1.0f, 1.0f);
    float3 r = t * dot(t, u) - u * t.z;
    float len = length(r);
    return (len > 1e-6f) ? r / len : baseN;
}

// Height-priority blend weight - IDENTICAL to the CPU resolver. The bias term w*(1-w)*4 peaks at
// w=0.5 and vanishes at the endpoints, so mask 0 -> below, mask 1 -> this layer exactly, while a
// taller layer wins more of the transition band. `noise` in [0,1] (pass 0.5 to disable).
float LayeredHeightWeight(float w, float layerHeight, float accHeight, float heightContribution,
                          float noise, float noiseAmount)
{
    float heightDiff = (layerHeight - accHeight) * heightContribution +
                       (noise - 0.5f) * 2.0f * noiseAmount;
    return saturate(w + heightDiff * (w * (1.0f - w) * 4.0f));
}

// Convenience: fold one layer's material sample into the accumulator by a resolved weight. Mirrors
// the CPU LerpSurface over the base metallic-roughness set the GPU forward pass carries.
struct LayerSample {
    float3 albedo;
    float  metallic;
    float  roughness;
    float3 normalTS;
    float  height;
};

void LayeredAccumulate(inout LayerSample acc, LayerSample layer, float w, bool contribNormal,
                       bool contribHeight)
{
    acc.albedo    = lerp(acc.albedo, layer.albedo, w);
    acc.metallic  = lerp(acc.metallic, layer.metallic, w);
    acc.roughness = lerp(acc.roughness, layer.roughness, w);
    if (contribNormal) {
        float3 detail = lerp(float3(0.0f, 0.0f, 1.0f), layer.normalTS, w);
        acc.normalTS = LayeredBlendNormalRNM(acc.normalTS, detail, 1.0f);
    }
    if (contribHeight) acc.height = lerp(acc.height, layer.height, w);
}

#endif // HBE_MATERIAL_LAYERED_HLSLI

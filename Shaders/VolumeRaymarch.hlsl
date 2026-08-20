// Shaders/VolumeRaymarch.hlsl - generic runtime NanoVDB volume raymarcher.
//
// Reads a BAKED NanoVDB grid, uploaded verbatim as a StructuredBuffer<uint> (stride 4), via
// PNanoVDB.h - the pointer-less C99/HLSL port of the NanoVDB read side. One source compiles to
// DXIL (D3D12) and SPIR-V (Vulkan) through DXC. The renderer knows how to render a volume, not
// how it was made: the volume arrives as opaque NanoVDB bytes + a world AABB.
//
// The lighting is a standard volume march (RayBox empty-space clip, a
// short self-shadow march, the Wrenninge/Haggstrom 3-octave multi-scatter approximation,
// Henyey-Greenstein phase, blackbody temperature emission, front-to-back compositing) - all of
// which is FORMAT-AGNOSTIC. Only the source changed: SampleVolume() now traverses a NanoVDB tree
// with PNanoVDB instead of sampling a dense Texture3D. Output is (rgb = inscatter+emission,
// a = transmittance); ApplyHalfRes composites it as scene*a + rgb.
//
// P1: density grid only (grid 0); temperature (a second grid) is added with the baker in P4, so
// the blackbody term is inert here (temp = 0). Fp16/float grids ride PNanoVDB's generic
// pnanovdb_read_float path; quantized grids are a later opt-in (they need per-type readers).
#include "PostCommon.hlsli"

#define PNANOVDB_HLSL
#include <nanovdb/PNanoVDB.h>

// The baked volume grid blob as a StructuredBuffer<uint> (== pnanovdb_buf_t). D3D12: t0 space6
// (root-param 5, the SRV table the legacy Texture3D also used - a buffer SRV and a tex SRV share
// it fine). Vulkan: a DISTINCT storage-buffer binding (6) - binding 5 is a SAMPLED_IMAGE in the
// shared post descriptor-set layout and cannot double as a storage buffer.
[[vk::binding(6, 0)]] StructuredBuffer<uint> gVolumeGrid : register(t0, space6);
// The OPTIONAL temperature grid (same bake bounds as density) for emission glow. D3D12: t1 space6
// (its own root-param descriptor table). Vulkan: storage-buffer binding 7. Bound to a VALID buffer
// EVERY frame (fallback = the density buffer when no temperature); gVolEmission.w (hasTemp) gates
// whether it is actually sampled, so the static shader reference always resolves.
[[vk::binding(7, 0)]] StructuredBuffer<uint> gVolumeTemp : register(t1, space6);

// PostParams packing (set by the backend's volume raymarch pass):
//   p0 = (boundsMin.xyz, stepCount)
//   p1 = (boundsMax.xyz, densityMul)
//   p2 = (emissionMul, shadowSteps, extinction, ditherFrame)
//   p3 = (worldOffset.xyz, unused)
//   gVolAlbedo = (albedo.rgb, emissionMode) ; gVolEmission = (emissionColor.rgb, hasTemp)

float3 Blackbody(float t) {
    t = saturate(t);
    float3 c = lerp(float3(0.05f, 0.004f, 0.0f), float3(0.85f, 0.16f, 0.02f), saturate(t * 3.0f));
    c = lerp(c, float3(1.0f, 0.48f, 0.06f), saturate(t * 2.2f - 0.7f));
    c = lerp(c, float3(1.0f, 0.83f, 0.38f), saturate(t * 2.0f - 1.0f));
    c = lerp(c, float3(1.0f, 0.96f, 0.85f), saturate(t * 6.0f - 5.2f));
    return c;
}

float VR_PhaseHG(float cosT, float g) {
    g = clamp(g, -0.9f, 0.9f);
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosT;
    const float p = (1.0f - g2) / (12.566371f * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
    return min(p, 6.0f);
}

float2 RayBox(float3 ro, float3 rd, float3 bmin, float3 bmax) {
    const float3 inv = 1.0f / rd;
    const float3 a = (bmin - ro) * inv;
    const float3 b = (bmax - ro) * inv;
    const float3 tmin = min(a, b), tmax = max(a, b);
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float VR_IGN(float2 p) {
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

// --- NanoVDB density sample -------------------------------------------------------
// Transform a world position into the grid's index space (the grid carries its own affine
// map, set at bake time) and read the nearest voxel's float value through a read accessor.
// The accessor is stateful (caches the last node path), so it is passed `inout` and reused
// across the whole ray - which is exactly what makes the sparse tree walk cheap.
float VR_SampleDensity(pnanovdb_buf_t buf, pnanovdb_grid_handle_t grid,
                       inout pnanovdb_readaccessor_t acc, float3 wp) {
    const pnanovdb_vec3_t idx = pnanovdb_grid_world_to_indexf(buf, grid, wp);
    const pnanovdb_coord_t ijk = pnanovdb_coord_t(int3(floor(idx + 0.5f))); // nearest voxel
    const pnanovdb_address_t addr =
        pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, buf, acc, ijk);
    return pnanovdb_read_float(buf, addr);
}

float4 PSMain(FSOutput input) : SV_Target {
    const float2 uv = input.positionCS.xy * gOutTexel;
    // worldOffset (p3.xyz) places the baked grid at the entity's world position: the RayBox clip is
    // offset BY +worldOffset, and each sample position is un-offset BY -worldOffset before the grid's
    // baked (local) transform maps it to index space. The two signs MUST stay opposite.
    const float3 worldOffset = gPostParams3.xyz;
    const float3 bmin = gPostParams0.xyz + worldOffset;
    const float3 bmax = gPostParams1.xyz + worldOffset;
    const int   steps = max(1, (int)gPostParams0.w);
    const float densityMul = gPostParams1.w;
    const float emissionMul = gPostParams2.x;
    const int   shadowSteps = max(0, (int)gPostParams2.y);
    const float extinction = max(gPostParams2.z, 1e-3f);
    // Volume color (P1): albedo tints single-scatter; emissionColor/emissionMode tint the glow.
    const float3 volAlbedo    = gVolAlbedo.xyz;
    const int    emissionMode = (int)gVolAlbedo.w;   // 0=blackbody, 1=tint, 2=tint x blackbody
    const float3 volEmissTint = gVolEmission.xyz;

    // Ray from the camera toward the scene surface at this pixel.
    const float depth = SamplePost(gInput2, uv).r;
    const float3 ro = gCameraPosWS;
    const float3 worldEnd = WorldFromDepthPost(uv, min(depth, 0.999999f));
    const float3 rd = normalize(worldEnd - ro);
    const float sceneDist = (depth >= 1.0f) ? 1e7f : length(worldEnd - ro);

    // Clip the march to the volume AABB (skip all empty space) and the scene depth.
    float2 box = RayBox(ro, rd, bmin, bmax);
    box.x = max(box.x, 0.0f);
    box.y = min(box.y, sceneDist);
    if (box.y <= box.x) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    // PNanoVDB grid + a single reusable read accessor for the whole ray (grid 0 at offset 0).
    pnanovdb_buf_t buf = gVolumeGrid;
    pnanovdb_grid_handle_t grid;
    grid.address = pnanovdb_address_null();
    const pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    const pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);

    // Second accessor for the temperature grid (drives emission). Only sampled when hasTemp>0.5.
    const float hasTemp = gVolEmission.w;
    pnanovdb_buf_t tbuf = gVolumeTemp;
    pnanovdb_grid_handle_t tgrid;
    tgrid.address = pnanovdb_address_null();
    const pnanovdb_tree_handle_t ttree = pnanovdb_grid_get_tree(tbuf, tgrid);
    const pnanovdb_root_handle_t troot = pnanovdb_tree_get_root(tbuf, ttree);
    pnanovdb_readaccessor_t tacc;
    pnanovdb_readaccessor_init(tacc, troot);

    const float marchLen = box.y - box.x;
    const float stepLen = marchLen / steps;
    const float dither = VR_IGN(input.positionCS.xy + 5.588238f * gPostParams2.w);
    const float3 L = normalize(gLightDirWS); // toward the sun
    const float3 ambient = gLightColor * gAmbientIntensity;

    float transmittance = 1.0f;
    float3 inscatter = float3(0.0f, 0.0f, 0.0f);
    // EMPTY-SPACE SKIP: smoke fills a tiny fraction of its AABB, so most uniform steps sample
    // background and only pay the density read. When a sample is empty we grow the step (up to 3x)
    // and advance without shading; the instant density appears we snap back to the base step. This
    // is ENERGY-CORRECT in empty regions (d==0 -> exp(-0*step)==1, no matter the step) and only
    // risks under-sampling the leading edge of a feature thinner than ~3 steps, which the 3x cap
    // bounds. The iteration budget stays `steps`, so a dense volume behaves exactly as before.
    float t = box.x + dither * stepLen;
    float skip = stepLen;
    [loop]
    for (int i = 0; i < steps && t < box.y; ++i) {
        const float3 pos = ro + rd * t;
        float d = VR_SampleDensity(buf, grid, acc, pos - worldOffset) * densityMul; // -offset: back to grid local
        d *= saturate((sceneDist - t) * 2.857f); // soft contact with geometry
        if (d < 1e-4f) {
            skip = min(skip * 1.5f, stepLen * 3.0f); // accelerate through empty space
            t += skip;
            continue;
        }
        skip = stepLen; // density found: march at full resolution again

        // Self-shadow: short optical-depth march toward the light through the grid.
        float shadow = 1.0f;
        if (shadowSteps > 0) {
            const float2 lb = RayBox(pos, L, bmin, bmax);
            const float lFar = max(min(lb.y, marchLen), 0.0f);
            const float ls = lFar / shadowSteps;
            float od = 0.0f;
            [loop]
            for (int j = 0; j < shadowSteps; ++j)
                od += VR_SampleDensity(buf, grid, acc, pos + L * ((j + 0.5f) * ls) - worldOffset) *
                      densityMul * ls;
            shadow = exp(-od * extinction);
        }

        // 3-octave multiple-scattering approximation (reuses the one self-shadow value).
        const float cosT = dot(rd, L);
        float msSun = 0.0f;
        [unroll] for (int o = 0; o < 3; ++o) {
            const float a = exp2(-(float)o);
            const float b = pow(0.6f, (float)o);
            const float c = pow(0.55f, (float)o);
            msSun += c * pow(max(shadow, 1e-3f), a) * VR_PhaseHG(cosT, 0.6f * b);
        }
        msSun += 0.06f;
        const float3 sun = gLightColor * (gLightIntensity * (msSun * 0.28f));

        // Temperature emission. Sample the temperature grid (same transform as density) only when one
        // is bound (hasTemp); placed AFTER the density early-out so cool/empty voxels never pay for the
        // temp tree walk. temp==0 -> inert. emissionMode: 0=blackbody(temp), 1=tint, 2=tint x blackbody.
        const float temp = (hasTemp > 0.5f)
                         ? VR_SampleDensity(tbuf, tgrid, tacc, pos - worldOffset) // generic scalar read
                         : 0.0f;
        const float3 bb = Blackbody(temp);
        const float3 ecol = (emissionMode == 1) ? volEmissTint
                          : (emissionMode == 2) ? bb * volEmissTint : bb;
        const float3 emission = ecol * (emissionMul * temp * temp * temp);

        // albedo multiplies the SCATTERED light (single-scatter tint); emission is added separately.
        const float3 scat = (sun + ambient * 0.4f) * volAlbedo * d + emission * d;
        const float stepTrans = exp(-d * stepLen * extinction);
        inscatter += transmittance * scat * stepLen;
        transmittance *= stepTrans;
        if (transmittance < 0.003f) break;
        t += stepLen; // shaded step: advance at the base resolution
    }
    return float4(inscatter, transmittance);
}

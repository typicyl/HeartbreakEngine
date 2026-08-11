// Shaders/Common.hlsli - shared constant buffers, tonemapping, color helpers.
#ifndef HBE_COMMON_HLSLI
#define HBE_COMMON_HLSLI

// One punctual (point or spot) light. Must match rhi::PunctualLight (each line
// packs to one float4).
struct PunctualLight
{
    float3 positionWS;
    float  range;
    float3 color;
    float  intensity;
    float3 directionWS; // spot axis, points away from the light
    uint   isSpot;
    float  innerCos;
    float  outerCos;
    float2 _padL;
};

#define HBE_MAX_PUNCTUAL_LIGHTS 16
#define HBE_MAX_SHADOW_CASCADES 4
#define HBE_MAX_PROBES 64
#define HBE_MAX_DECALS 16
#define HBE_MAX_RIPPLES 16

// One baked local light/reflection probe (matches rhi::ProbeData).
struct Probe
{
    float3 center;
    float  _p0;
    float3 halfExtents;
    float  blend;       // edge-softening band (world units)
    uint   irradianceIndex;
    uint   prefilteredIndex;
    float  prefilteredMaxLod;
    float  _p1;
};

// One forward projected decal (matches rhi::DecalData; each line packs to float4s).
struct Decal
{
    float4x4 invWorld;                    // world -> decal unit cube [-0.5,0.5]
    float3   forwardWS; float opacity;    // projection axis (box local +Z)
    float3   tangentWS; float angleFade;  // decal U axis; pow() on surface-faces-projector
    uint albedoIndex; uint normalIndex; uint mrIndex; uint flags;
    float4   params;                      // x=normalStrength, y=roughness, z=metallic
};

// Per-frame constants (camera + primary directional light + ambient/IBL).
cbuffer FrameConstants : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPosWS;
    float    gExposure;
    float3   gLightDirWS;     // direction *to* the light, normalized
    float    gLightIntensity;
    float3   gLightColor;
    float    gAmbientIntensity;
    uint     gIrradianceIndex;   // bindless IBL maps (0 = none)
    uint     gPrefilteredIndex;
    uint     gBrdfLUTIndex;
    float    gPrefilteredMaxLod;
    float4x4 gInvViewProj;       // unprojects NDC for the sky pass
    uint     gSkyIndex;          // bindless sky/environment equirect (0 = none)
    uint     gOutputLinear;      // 1 = HDR post pipeline active: shaders emit
                                 // linear radiance (exposure/tonemap in post)
    float2   gScreenTexel;       // (1/renderW, 1/renderH): SV_Position.xy * this = screen UV
    // Cascaded shadow maps: per-cascade light frusta rendered into a 2x2 atlas
    // (cascade i occupies tile ((i&1), (i>>1)) of the shadow map).
    float4x4 gCascadeViewProj[HBE_MAX_SHADOW_CASCADES];
    float4   gCascadeSplits;     // view-space far distance of each cascade
    uint     gShadowMapIndex;    // bindless shadow atlas (0 = no shadows)
    uint     gCascadeCount;
    float2   _padFrame1;
    uint     gPunctualCount;     // active entries in gPunctualLights
    uint     gSkinLUTIndex;      // pre-integrated SSS LUT (0 = none)
    uint     gTaaActive;         // 1 when TAA is resolving this frame (0 = FXAA/no AA)
    uint     _padFrame2;
    PunctualLight gPunctualLights[HBE_MAX_PUNCTUAL_LIGHTS];
    float4x4 gPrevViewProj;      // previous frame's (jittered) view-proj, for TAA
    // 3D painterly surface-stroke params (global; 0 = mode off). Appended at the
    // tail so existing shaders are unaffected.
    float4 gStroke0;             // (size world units, width fraction, sharpness, flow)
    float4 gStroke1;             // (bristle, size jitter, angle jitter, distance fade)
    // Local light/reflection probes (per-pixel box-blended; appended at the tail
    // so existing offsets are unchanged). 0 = none -> global sky IBL.
    uint   gProbeCount;
    float3 _padProbe;
    Probe  gProbes[HBE_MAX_PROBES];
    // Baked SH-L1 irradiance volume (the diffuse-GI upgrade). gGiShIndex 0 = none.
    // gGiDepthIndex (0 = none) is the octahedral depth atlas for DDGI visibility.
    float3 gGiOrigin;     uint gGiShIndex;
    float3 gGiInvSpacing; uint gGiDepthIndex;
    int3   gGiDims;       uint _padGi1;
    // Weather for the analytic sky (x = cloud coverage, y = cloud density,
    // z = overcast, w = time seconds). Appended at the tail.
    float4 gWeather;
    // x,y = wind velocity (cloud-UV units/sec); clouds drift by gWeather1.xy * time.
    float4 gWeather1;
    // Weather surface response (appended at the tail). Drives the wet/puddle/snow
    // block in MeshPBR. gWeather2: x = wetness (0..1), y = puddle coverage (0..1),
    // z = snow accumulation (0..1), w = RAIN intensity (0..1, drives puddle ripples; 0 for snow).
    // gWeather3: x = puddle noise world scale (m), y = snow break-up scale (m),
    // z = volumetric clouds (>0.5 = raymarched slab, else 2D layer), w = cloud quality.
    float4 gWeather2;
    float4 gWeather3;
    // Forward projected decals (appended at the tail). gDecalCount active entries.
    uint   gDecalCount; float3 _padDecal;
    Decal  gDecals[HBE_MAX_DECALS];
    // Water surface (Gerstner) - global scene ocean params (read by Water.hlsl only).
    float4 gWaveA[4];      // per wave: (dirX, dirZ, amplitude, wavelength)
    float4 gWaveB[4];      // per wave: (speed, steepness, 0, 0)
    float4 gWaterShallow;  // (rgb grazing tint, fresnel power)
    float4 gWaterDeep;     // (rgb deep body colour, reflection roughness 0..1)
    float4 gWaterParams;   // (foamAmount, rippleStrength, rippleScale, fftOn: >0.5 = FFT ocean)
    uint   gRippleCount;
    float3 gFftParams;     // FFT ocean: (tilePatchSize m, heightScale, unused)
    float4 gRipples[HBE_MAX_RIPPLES]; // (centerX, centerZ, age seconds, strength)
    // Depth-based water: the water PS reads scene depth to grade absorption/foam/soft edges.
    uint   gSceneDepthIndex; // bindless scene-depth SRV (0 = not bound; skip depth grading)
    float  gAbsorptionDepth; // metres of water column to full deep-colour absorption
    float  gShorelineWidth;  // metres of the shoreline foam band
    float  gEdgeFade;        // metres of soft depth-fade where water meets geometry
};

// Per-object constants.
cbuffer ObjectConstants : register(b1)
{
    float4x4 gModel;
    float4x4 gNormalMatrix;   // inverse-transpose of model, for normals
    float4   gBaseColorFactor;
    float    gMetallicFactor;
    float    gRoughnessFactor;
    uint     gAlbedoIndex;     // bindless texture indices (0 = none)
    uint     gNormalIndex;
    uint     gMRIndex;         // glTF: B = metallic, G = roughness
    uint     gAOIndex;
    uint     gMaterialFlags;   // bit0 = subsurface
    float    _pad0;
    float3   gSubsurfaceColor;
    float    _pad1;
    float3   gEmissiveColor;   // linear radiance added after lighting
    float    gEmissiveIntensity;
    uint     gEmissiveIndex;   // bindless emissive map (0 = none)
    uint     gSkinned;         // 1 = vertices carry joint indices/weights
    uint     gBoneOffset;      // first palette entry in gBones for this draw
    uint     gBoneCount;       // palette size; clamps indices (GPU-hang guard)
    float4x4 gPrevModel;       // previous-frame world matrix (motion vectors)
    uint     gPrevBoneOffset;  // previous-frame palette base (skinned motion)
    uint     gThicknessIndex;  // SSS transmission thickness map (0 = none)
    float    gSubsurfaceRadius;// SSS scatter scale (curvature multiplier)
    float    _padObj;
    // Art Editor surface paint canvas (0 = unpainted; composited over albedo).
    uint     gPaintColorIndex; // RGB pigment + A coverage (mipped)
    uint     gPaintHeightIndex;// R relief height (0.5 neutral), perturbs normal
    float    gPaintOpacity;    // global blend of the paint over the material
    float    gPaintHeightScale;// relief strength (0 = colour only)
    float    gPaintLodBias;    // distance -> mip averaging (bigger strokes far away)
    float    gPaintTexel;      // 1 / paint resolution (height differencing)
    uint     gPaintProjMode;   // 0 = mesh UV, 1 = box projection (no stretch)
    float    gPaintBoxInvM;    // 1 / max(extent*scale) for box projection
    float3   gPaintBoxCenter;  // local AABB center
    float    _padBoxC;
    float3   gPaintBoxScale;   // object world scale
    float    _padBoxS;
    // Terrain splat: 4 layers' albedo / normal / metal-rough bindless indices (the
    // weight mask is the emissive slot, the tile scale is gSubsurfaceRadius). Only
    // read when HBE_MAT_TERRAIN_SPLAT is set; 0 for every non-terrain draw.
    uint4    gSplatAlbedo;
    uint4    gSplatNormal;
    uint4    gSplatMR;
    float4   gSplatRough;   // 4 layers' roughness FACTOR (multiplies the MR map's green,
                            // or IS the roughness when a layer has no MR map)
    // GPU instancing: when gInstanced != 0 the VS replaces gModel/gNormalMatrix/
    // gPrevModel with gInstances[(gInstanceBase + SV_InstanceID)*3 + {0,1,2}].
    // Zero-init (the C++ ObjectCB default) keeps every single-draw byte-compatible.
    uint     gInstanced;
    uint     gInstanceBase; // first instance's slot in gInstances (in instances)
    uint2    _padInstanced;
    // Facial blendshapes: gMorphTexIndex (0 = none) is a bindless RGBA16F delta
    // atlas - width = vertex count, one ROW per target = xyz position delta. The VS
    // adds gMorphCount active targets (their atlas rows in gMorphTargets, weights in
    // gMorphWeights) into the vertex BEFORE skinning. Zero-init (the C++ ObjectCB
    // default) keeps every non-morph draw byte-identical.
    uint     gMorphTexIndex;
    uint     gMorphCount;
    uint2    _padMorph;
    uint4    gMorphTargets[2]; // 8 active atlas rows
    float4   gMorphWeights[2]; // 8 active weights (parallel to gMorphTargets)
    // CLEARCOAT: a thin, sharp dielectric layer over the base material - wet skin,
    // sweat, wet eyes, blood sheen, varnish, car paint. A second GGX lobe with its
    // own (low) roughness and a fixed F0=0.04, energy-conserved against the base.
    // Zero-init (gClearcoat = 0) = off, so every existing draw stays byte-identical.
    float    gClearcoat;          // strength (0 = off)
    float    gClearcoatRoughness; // clear layer roughness (low = wet/glossy)
    float    _padCc0;
    float    _padCc1;
};

// Per-frame joint palettes (every skinned draw appends its global*inverseBind
// matrices; gBoneOffset indexes this draw's slice).
//   D3D12 : root SRV at t0, space1.
//   Vulkan: storage buffer at set 0 binding 3.
[[vk::binding(3, 0)]] StructuredBuffer<float4x4> gBones : register(t0, space1);

// Per-frame instance transforms (3 matrices per instance: model, normalMatrix,
// prevModel), appended per instanced run; gInstanceBase indexes the run's slice.
//   D3D12 : root SRV at t1, space1.
//   Vulkan: storage buffer at set 0 binding 4.
[[vk::binding(4, 0)]] StructuredBuffer<float4x4> gInstances : register(t1, space1);

// Material feature flags (must match rhi::MaterialFlags).
#define HBE_MAT_SUBSURFACE  1u
#define HBE_MAT_CLOTH       2u
#define HBE_MAT_EYE         4u
#define HBE_MAT_HAIR        8u
#define HBE_MAT_TRANSPARENT 16u
#define HBE_MAT_NOSHADOW    32u
#define HBE_MAT_PAINTERLY_EXEMPT 128u // dynamic-layer object: write the painterly mask
#define HBE_MAT_TERRAIN_HOLE 256u     // clip terrain pixels where the hole mask (thickness slot) is set
#define HBE_MAT_TERRAIN_SPLAT 512u    // blend 4 tiling layer albedos (albedo/normal/mr/ao slots) by the emissive-slot weight mask
#define HBE_MAT_CENSORED 1024u // entity carries a CensorComponent: paint strokes onto its surface (not the area around it)

// ---------------------------------------------------------------------------
// Bindless texture table.
//   D3D12 : an unbounded SRV array in space0 (Resource Binding Tier 3) plus a
//           static sampler at s0.
//   Vulkan: descriptor set 1 - a SAMPLER at binding 0 and a variable-count
//           SAMPLED_IMAGE array at binding 1 (VK_EXT_descriptor_indexing).
// The texture array is the highest binding so Vulkan can mark it variable-count.
// ---------------------------------------------------------------------------
[[vk::binding(0, 1)]] SamplerState gBindlessSampler : register(s0, space0);
[[vk::binding(1, 1)]] Texture2D    gTextures[]      : register(t0, space0);

// Samples a bindless texture by index. Index 0 is a 1x1 white texture.
float4 SampleBindless(uint index, float2 uv)
{
    return gTextures[NonUniformResourceIndex(index)].Sample(gBindlessSampler, uv);
}

// Samples a bindless texture at an explicit mip level (for prefiltered IBL).
float4 SampleBindlessLod(uint index, float2 uv, float lod)
{
    return gTextures[NonUniformResourceIndex(index)].SampleLevel(gBindlessSampler, uv, lod);
}

// Samples a bindless texture with EXPLICIT screen-space gradients. Well-defined inside
// divergent control flow (e.g. a per-decal loop that `continue`s per lane), where the
// implicit-derivative Sample() would be undefined behaviour.
float4 SampleBindlessGrad(uint index, float2 uv, float2 dx, float2 dy)
{
    return gTextures[NonUniformResourceIndex(index)].SampleGrad(gBindlessSampler, uv, dx, dy);
}

// POINT-fetches a bindless texel (no filtering, no addressing). For masks whose texel
// GRID is authoritative rather than its filtered value - the terrain hole mask is
// indexed by heightfield SAMPLE, so filtering it would blur a hole across the quad
// boundary and misregister it against the grid it was painted on.
float4 LoadBindless(uint index, int2 px)
{
    return gTextures[NonUniformResourceIndex(index)].Load(int3(px, 0));
}

// Texel dimensions of a bindless texture (mip 0).
uint2 BindlessSize(uint index)
{
    uint w, h;
    gTextures[NonUniformResourceIndex(index)].GetDimensions(w, h);
    return uint2(w, h);
}

// --- Baked SH-L1 irradiance volume ----------------------------------------
// The atlas is 4 texels wide (the 4 SH coefficients) x cellCount tall; cell
// index = x + y*dimX + z*dimX*dimY. Samples at exact texel centres (no filtering).
float3 GiCellIrradiance(uint cell, uint cellCount, float3 N)
{
    const float v = (cell + 0.5f) / float(cellCount);
    float3 c0 = SampleBindlessLod(gGiShIndex, float2(0.125f, v), 0.0f).rgb;
    float3 c1 = SampleBindlessLod(gGiShIndex, float2(0.375f, v), 0.0f).rgb;
    float3 c2 = SampleBindlessLod(gGiShIndex, float2(0.625f, v), 0.0f).rgb;
    float3 c3 = SampleBindlessLod(gGiShIndex, float2(0.875f, v), 0.0f).rgb;
    return c0 * 0.282095f + 0.488603f * (c1 * N.y + c2 * N.z + c3 * N.x);
}

// Unit direction -> octahedral UV [0,1]^2 (matches OctDecodeDir in IBL.cpp).
float2 GiOctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 oct = n.xy;
    if (n.z < 0.0f)
        oct = (1.0f - abs(n.yx)) *
              float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return oct * 0.5f + 0.5f;
}

// Trilinearly samples the volume at world position P, evaluated for normal N.
// Each of the 8 corner cells is weighted by its trilinear weight, a front-facing
// term, AND (DDGI) a depth-visibility test: a cell occluded from P by geometry (a
// wall in between) is skipped -> exact leak-free GI.
float3 SampleGiVolume(float3 P, float3 N)
{
    const uint cellCount = uint(gGiDims.x * gGiDims.y * gGiDims.z);
    float3 g = clamp((P - gGiOrigin) * gGiInvSpacing, 0.0f, float3(gGiDims) - 1.0f);
    int3   b = int3(floor(g));
    float3 f = g - float3(b);
    float3 spacing = 1.0f / max(gGiInvSpacing, 1e-4f);
    float3 sum = 0.0f;
    float  wsum = 0.0f;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c = clamp(b + off, int3(0, 0, 0), gGiDims - 1);
        float3 tw3 = lerp(1.0f - f, f, float3(off));
        float tw = tw3.x * tw3.y * tw3.z;
        if (tw <= 0.0f) continue;
        uint cell = uint(c.x + c.y * gGiDims.x + c.z * gGiDims.x * gGiDims.y);
        float3 cellPos = gGiOrigin + float3(c) * spacing;
        // DDGI depth visibility: the cell baked its distance to geometry in every
        // direction; if the surface sits FARTHER than that, a wall is between them,
        // so the cell can't light this surface. Use the SOFT Chebyshev test (the
        // reason the bake stores distance^2 in .g) rather than a hard cull: a binary
        // continue snaps a corner probe on/off as the surface point crosses each cell
        // boundary, and that discontinuity is exactly what makes the volume show up
        // as world-aligned CUBES (worst in shadow, where indirect light dominates).
        float visWeight = 1.0f;
        if (gGiDepthIndex != 0u)
        {
            float2 oct = GiOctEncode(normalize(P - cellPos));
            int ox = clamp(int(oct.x * 8.0f), 0, 7);
            int oy = clamp(int(oct.y * 8.0f), 0, 7);
            float2 moments = SampleBindlessLod(gGiDepthIndex,
                                        float2((oy * 8 + ox + 0.5f) / 64.0f,
                                               (cell + 0.5f) / float(cellCount)),
                                        0.0f).rg; // .x = mean dist, .y = mean(dist^2)
            float bias = max(spacing.x, max(spacing.y, spacing.z)) * 0.25f;
            float dist = length(P - cellPos) - bias;
            if (dist > moments.x)
            {
                float variance = max(abs(moments.x * moments.x - moments.y), 2e-4f);
                float d = dist - moments.x;
                float cheb = variance / (variance + d * d); // [0,1], smooth
                visWeight = cheb * cheb * cheb;             // sharpen (DDGI convention)
            }
        }
        float3 toCell = cellPos - P;
        // Front-facing term: a cell behind / perpendicular to the surface (e.g.
        // across a wall or above a ceiling) contributes little.
        float vis = saturate(dot(N, normalize(toCell + N * 1e-3f)) + 0.15f);
        float w = tw * vis * vis * visWeight;
        if (w <= 0.0f) continue;
        sum += max(GiCellIrradiance(cell, cellCount, N), 0.0f) * w;
        wsum += w;
    }
    return wsum > 1e-4f ? sum / wsum : 0.0f;
}

// Maps a direction to equirectangular UV (matches the CPU IBL generator).
float2 EquirectUV(float3 dir)
{
    const float invTwoPi = 0.15915494f; // 1 / (2*pi)
    const float invPi    = 0.31830989f; // 1 / pi
    float u = atan2(dir.z, dir.x) * invTwoPi + 0.5f;
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) * invPi;
    return float2(u, v);
}

// ---------------------------------------------------------------------------
// Octahedral unit-vector packing (used to store the G-buffer world normal in
// two channels). Encodes to [0,1]^2; decode returns a unit vector.
// ---------------------------------------------------------------------------
float2 SignNotZero(float2 v) { return float2(v.x >= 0.0f ? 1.0f : -1.0f,
                                             v.y >= 0.0f ? 1.0f : -1.0f); }
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-8f);
    float2 e = (n.z >= 0.0f) ? n.xy : ((1.0f - abs(n.yx)) * SignNotZero(n.xy));
    return e * 0.5f + 0.5f;
}
float3 OctDecode(float2 f)
{
    float2 e = f * 2.0f - 1.0f;
    float3 v = float3(e.xy, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f) v.xy = (1.0f - abs(v.yx)) * SignNotZero(v.xy);
    return normalize(v);
}

// Screen-space UV (v down) of a clip-space position; matches the post stack's
// reprojection convention so motion vectors line up with TAA/motion-blur.
float2 ClipToScreenUV(float4 clip)
{
    float2 ndc = clip.xy / clip.w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// ---------------------------------------------------------------------------
// Cascaded directional shadow mapping (PCF over a 2x2 shadow atlas).
// ---------------------------------------------------------------------------
#define HBE_SHADOW_ATLAS_TEXEL (1.0f / 4096.0f)

// Returns the fraction of the directional light reaching `positionWS` (1 = lit).
float ShadowFactor(float3 positionWS, float NdotL)
{
    if (gShadowMapIndex == 0 || gCascadeCount == 0)
        return 1.0f;

    // Cascades are ordered near -> far; the first frustum containing the point
    // is the highest-resolution one.
    [unroll]
    for (uint c = 0; c < HBE_MAX_SHADOW_CASCADES; ++c)
    {
        if (c >= gCascadeCount)
            return 1.0f;

        float4 lp = mul(gCascadeViewProj[c], float4(positionWS, 1.0f));
        float3 ndc = lp.xyz / lp.w;
        float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        const float margin = 0.01f; // skip borders so PCF never crosses tiles
        if (uv.x < margin || uv.x > 1.0f - margin ||
            uv.y < margin || uv.y > 1.0f - margin ||
            ndc.z <= 0.0f || ndc.z >= 1.0f)
            continue; // outside this cascade; try the next (coarser) one

        // Slope-scaled receiver bias on top of the rasterizer's depth bias;
        // coarser cascades cover more world per texel and need more bias.
        const float bias = max(0.0015f * (1.0f - NdotL), 0.0003f) * (1.0f + c * 0.5f);
        const float receiver = ndc.z - bias;

        // This cascade's tile within the 2x2 atlas.
        const float2 tile = float2(c & 1, c >> 1) * 0.5f;
        const float2 tileMin = tile + HBE_SHADOW_ATLAS_TEXEL;
        const float2 tileMax = tile + 0.5f - HBE_SHADOW_ATLAS_TEXEL;

        // Rotated 4-tap PCF (was a 3x3 = 9-tap): ~half the shadow-map samples per
        // lit pixel for near-identical softness. The directional shadow lookup is the
        // dominant DAYTIME GPU cost (the sun casts by day, dims to ~0 at night), so
        // halving it gives the daytime frame the headroom to reach the vsync cap that
        // night already hits. A per-world-position rotation dithers the sparser kernel
        // so it doesn't reveal a fixed grid (same trick as ShadowFactorCheap below).
        //
        // With ONLY the spatial hash the dither is WORLD-LOCKED, so a soft penumbra reads
        // as a stationary salt-and-pepper speckle. When TAA is resolving this frame
        // (gTaaActive), sweep the rotation over time so each surface point samples a
        // DIFFERENT kernel orientation every frame and TAA averages them into a smooth soft
        // shadow - at ZERO extra taps. Gated so the Low preset (no TAA) keeps the stable
        // static dither instead of shimmering. gWeather.w = time.
        float rot =
            frac(sin(dot(positionWS.xz, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;
        rot += gWeather.w * 30.0f * float(gTaaActive);
        const float sr = sin(rot), cr = cos(rot);
        const float2 kBase[4] = {float2(0.9f, 0.9f), float2(-0.9f, 0.9f), float2(0.9f, -0.9f),
                                 float2(-0.9f, -0.9f)};
        float lit = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float2 d = float2(kBase[i].x * cr - kBase[i].y * sr,
                                    kBase[i].x * sr + kBase[i].y * cr) *
                             HBE_SHADOW_ATLAS_TEXEL;
            const float2 suv = clamp(tile + uv * 0.5f + d, tileMin, tileMax);
            const float occluder = SampleBindlessLod(gShadowMapIndex, suv, 0.0f).r;
            lit += (receiver <= occluder) ? 1.0f : 0.0f;
        }
        return lit * 0.25f;
    }
    return 1.0f;
}

// Soft directional shadow for the volumetric-fog ray-march. A single HARD tap
// (returning 0 or 1) makes the shadow map's texels project into the fog as blocky
// god-ray CUBES - worst at half-res fog with a coarse step count, and the reason
// the fog reads as cubes on shadowed surfaces. A small ROTATED 4-tap PCF turns
// those hard texel edges into smooth gradients while staying far cheaper than the
// surface 3x3 (4 taps, only at fog resolution). The per-position rotation keeps
// the softened result from revealing a fixed grid.
float ShadowFactorCheap(float3 positionWS)
{
    if (gShadowMapIndex == 0 || gCascadeCount == 0)
        return 1.0f;
    [unroll]
    for (uint c = 0; c < HBE_MAX_SHADOW_CASCADES; ++c)
    {
        if (c >= gCascadeCount)
            return 1.0f;
        float4 lp = mul(gCascadeViewProj[c], float4(positionWS, 1.0f));
        float3 ndc = lp.xyz / lp.w;
        float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        const float margin = 0.01f;
        if (uv.x < margin || uv.x > 1.0f - margin ||
            uv.y < margin || uv.y > 1.0f - margin ||
            ndc.z <= 0.0f || ndc.z >= 1.0f)
            continue;
        const float bias = 0.0015f * (1.0f + c * 0.5f);
        const float receiver = ndc.z - bias;
        const float2 tile = float2(c & 1, c >> 1) * 0.5f;
        const float2 tileMin = tile + HBE_SHADOW_ATLAS_TEXEL;
        const float2 tileMax = tile + 0.5f - HBE_SHADOW_ATLAS_TEXEL;
        const float2 base = tile + uv * 0.5f;
        // Rotated 4-tap kernel (~2 texels across), angle hashed from world pos.
        const float ang = frac(sin(dot(positionWS, float3(12.9898f, 78.233f, 37.719f)))
                               * 43758.5453f) * 6.2831853f;
        const float2 ox = float2(cos(ang), sin(ang)) * (HBE_SHADOW_ATLAS_TEXEL * 2.0f);
        const float2 oy = float2(-ox.y, ox.x);
        float lit = 0.0f;
        lit += (receiver <= SampleBindlessLod(gShadowMapIndex,
                    clamp(base + ox + oy, tileMin, tileMax), 0.0f).r) ? 1.0f : 0.0f;
        lit += (receiver <= SampleBindlessLod(gShadowMapIndex,
                    clamp(base + ox - oy, tileMin, tileMax), 0.0f).r) ? 1.0f : 0.0f;
        lit += (receiver <= SampleBindlessLod(gShadowMapIndex,
                    clamp(base - ox + oy, tileMin, tileMax), 0.0f).r) ? 1.0f : 0.0f;
        lit += (receiver <= SampleBindlessLod(gShadowMapIndex,
                    clamp(base - ox - oy, tileMin, tileMax), 0.0f).r) ? 1.0f : 0.0f;
        return lit * 0.25f;
    }
    return 1.0f;
}

// ACES filmic tonemapping (Narkowicz approximation).
float3 TonemapACES(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Linear -> sRGB gamma encoding for output to a UNORM back buffer.
float3 LinearToSRGB(float3 c)
{
    return pow(max(c, 0.0f.xxx), (1.0f / 2.2f).xxx);
}

#endif // HBE_COMMON_HLSLI

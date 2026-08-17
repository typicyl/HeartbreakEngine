// Shaders/MeshPBR.hlsl - textured metallic-roughness PBR with IBL.
//
// Material maps (base color / normal / metallic-roughness / AO) are sampled from
// the bindless texture array by per-object index (0 = use the constant factor).
#include "Common.hlsli"
#include "BRDF.hlsli"
#include "OpenPBRSurface.hlsli"

struct VSInput
{
    float3 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    float4 tangentOS  : TANGENT;   // .w = handedness
    float2 uv         : TEXCOORD0;
    uint4  joints     : BLENDINDICES; // skeleton joint indices
    float4 weights    : BLENDWEIGHT;  // all zero on static meshes
    uint   vertexId   : SV_VertexID;  // morph delta atlas addressing
};

struct VSOutput
{
    float4 positionCS : SV_Position;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float2 uv         : TEXCOORD2;
    float4 tangentWS  : TEXCOORD3;
    float4 curClip    : TEXCOORD4; // unjittered-free clip pos (for motion vectors)
    float4 prevClip   : TEXCOORD5; // previous-frame clip pos
    float3 positionOS : TEXCOORD6; // object-space position (box paint projection)
    float3 normalOS   : TEXCOORD7; // object-space normal (box paint projection)
};

// Box paint projection: maps an object-space point + face normal into a 4x4-cell
// atlas (6 cells for the 6 face directions) at a uniform world-space density, so
// paint never stretches even on a non-uniformly scaled mesh. Matches
// paint::BoxProjectUV on the CPU exactly.
float2 BoxPaintUV(float3 posOS, float3 nOS)
{
    float3 wc = (posOS - gPaintBoxCenter) * gPaintBoxScale; // world-centered offset
    float3 an = abs(nOS);
    float2 plane;
    int col, row;
    if (an.x >= an.y && an.x >= an.z) { plane = wc.zy; col = (nOS.x >= 0) ? 0 : 1; row = 0; }
    else if (an.y >= an.z)            { plane = wc.xz; col = (nOS.y >= 0) ? 2 : 3; row = 0; }
    else                              { plane = wc.xy; col = (nOS.z >= 0) ? 0 : 1; row = 1; }
    float2 cell = saturate(plane * gPaintBoxInvM + 0.5f);
    return (float2(col, row) + cell) * 0.25f;
}

// Blends the joint palette starting at `base`; `j` are clamped indices.
float4x4 SkinMatrix(uint base, uint4 j, float4 w)
{
    return gBones[base + j.x] * w.x +
           gBones[base + j.y] * w.y +
           gBones[base + j.z] * w.z +
           gBones[base + j.w] * w.w;
}

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    float3 posOS = input.positionOS;
    float3 nrmOS = input.normalOS;
    float3 tanOS = input.tangentOS.xyz;
    float3 prevPosOS = input.positionOS; // skinned with the previous palette

    // Facial blendshapes: add each active target's weighted position delta into the
    // vertex BEFORE skinning. Sampler-free .Load() (texelFetch) is bit-exact and
    // valid in the VS on both backends. gMorphTexIndex == 0 -> no morphs (skipped,
    // so non-morph draws are untouched). Deltas apply to prevPos too (no spurious
    // motion-vector from the morph itself; acceptable for slow facial motion).
    if (gMorphTexIndex != 0u && gMorphCount != 0u)
    {
        [loop] for (uint m = 0u; m < gMorphCount; ++m)
        {
            uint   row = gMorphTargets[m >> 2u][m & 3u];
            float  w   = gMorphWeights[m >> 2u][m & 3u];
            float3 d   = gTextures[gMorphTexIndex].Load(int3(int(input.vertexId), int(row), 0)).xyz;
            posOS     += w * d;
            prevPosOS += w * d;
        }
    }

    // GPU instancing: instanced runs replace the object CB's transforms with
    // this instance's slice of gInstances (3 matrices per instance). Single
    // draws have gInstanced == 0 and read the CB exactly as before.
    float4x4 model = gModel;
    float4x4 normalMat = gNormalMatrix;
    float4x4 prevModel = gPrevModel;
    if (gInstanced != 0u)
    {
        const uint base = (gInstanceBase + instanceId) * 3u;
        model = gInstances[base + 0u];
        normalMat = gInstances[base + 1u];
        prevModel = gInstances[base + 2u];
    }

    // Skeletal skinning: blend the joint palette (bind -> animated model
    // space), then the entity transform places the result in the world. Joint
    // indices are clamped to the palette so a stale/garbage index can never
    // read past gBones and fault the GPU.
    if (gSkinned != 0 && gBoneCount != 0)
    {
        uint last = gBoneCount - 1;
        uint4 j = min(input.joints, last.xxxx);
        // A vertex with no influences (all weights 0) keeps its bind position
        // instead of collapsing to the origin.
        float wsum = input.weights.x + input.weights.y + input.weights.z + input.weights.w;
        if (wsum > 1e-4f)
        {
            float4x4 skin = SkinMatrix(gBoneOffset, j, input.weights);
            posOS = mul(skin, float4(posOS, 1.0f)).xyz;
            nrmOS = mul((float3x3)skin, nrmOS);
            tanOS = mul((float3x3)skin, tanOS);
            // Previous-frame pose for the velocity buffer (gPrevBoneOffset ==
            // gBoneOffset when there is no history -> zero pose motion).
            float4x4 skinPrev = SkinMatrix(gPrevBoneOffset, j, input.weights);
            prevPosOS = mul(skinPrev, float4(prevPosOS, 1.0f)).xyz;
        }
    }

    VSOutput o;
    float4 posWS = mul(model, float4(posOS, 1.0f));
    o.positionWS = posWS.xyz;
    o.positionCS = mul(gViewProj, posWS);
    o.normalWS   = normalize(mul((float3x3)normalMat, nrmOS));
    o.tangentWS  = float4(normalize(mul((float3x3)model, tanOS)), input.tangentOS.w);
    o.uv         = input.uv;
    o.curClip    = o.positionCS;
    o.prevClip   = mul(gPrevViewProj, mul(prevModel, float4(prevPosOS, 1.0f)));
    o.positionOS = posOS; // object space (post-skin) for box paint projection
    o.normalOS   = nrmOS;
    return o;
}

// Perturbs the world normal with a tangent-space normal map.
float3 ApplyNormalMap(float3 N, float4 T, float2 uv)
{
    // Reconstruct Z from XY rather than reading the stored blue channel: BC5 normal maps carry
    // only R,G (blue = 0), and for a unit tangent-space normal Z = sqrt(1 - X^2 - Y^2). This is
    // exact for uncompressed RGBA8 normals too (Z is derived, not read), so it is format-agnostic.
    float2 nxy = SampleBindless(gNormalIndex, uv).xy * 2.0f - 1.0f;
    float3 tn = float3(nxy, sqrt(saturate(1.0f - dot(nxy, nxy))));
    float3 n = normalize(N);
    float3 t = normalize(T.xyz - n * dot(n, T.xyz)); // Gram-Schmidt
    float3 b = cross(n, t) * T.w;
    return normalize(mul(tn, float3x3(t, b, n)));
}

// Cook-Torrance direct response for one light direction. `radiance` is the
// light's incoming radiance at the shaded point; subsurface materials get a
// colored wrapped diffuse (light bleeds past the terminator, red furthest).
float3 ShadeDirect(float3 N, float3 V, float3 L, float3 radiance, float3 albedo,
                   float metallic, float roughness, float3 dielF0, float NdotV,
                   float curvature, float thickness, float3 tangentWS)
{
    float3 Hv = normalize(V + L);
    float  rawNdotL = dot(N, L);
    float  NdotL = max(rawNdotL, 0.0f);
    float  NdotH = max(dot(N, Hv), 0.0f);
    float  VdotH = max(dot(V, Hv), 0.0f);

    // Roughness-aware grazing Fresnel. Standard Schlick uses F90 = 1, so even a fully
    // rough surface flashes a bright specular rim at glancing angles / low sun (the
    // "wet ground" streak). Real rough surfaces lose that grazing sheen (multiple
    // scattering darkens F90), so fade F90 toward F0 with roughness: smooth surfaces
    // keep full Fresnel, rough surfaces (e.g. terrain at max roughness) read matte.
    float  f90s = 1.0f - roughness * roughness;
    // Skin / eyes: a single dielectric lobe flashes a HARD Fresnel rim at grazing
    // angles - plasticky on a face, and a bright ring around the sclera. Real skin's
    // oil layer is a broad, DIM grazing response, so cap F90 for these materials. This
    // also lets more of the reddened SSS diffuse survive at the silhouette (kdDirect
    // below is 1-Ft), where skin should redden most.
    if ((gMaterialFlags & (HBE_MAT_SUBSURFACE | HBE_MAT_EYE)) != 0u) f90s = min(f90s, 0.5f);
    float3 F90   = max(f90s.xxx, dielF0);
    // OpenPBR base dielectric Fresnel (Schlick from the IOR-derived dielF0). cloth/hair use this;
    // the default specular branch below blends in the F82-tint metal Fresnel by metalness.
    float3 Fdiel = FresnelSchlickF90(VdotH, dielF0, F90);
    float3 Ft    = Fdiel;
    float3 specular;
    if ((gMaterialFlags & HBE_MAT_CLOTH) != 0u)
    {
        // Fabric: Charlie sheen NDF + Neubelt visibility (soft grazing-angle
        // fuzz) instead of GGX, with an achromatic sheen tint.
        specular = D_Charlie(roughness, NdotH) * V_Neubelt(NdotV, NdotL)
                 * (0.3f * radiance * NdotL);
    }
    else if ((gMaterialFlags & HBE_MAT_HAIR) != 0u)
    {
        // Kajiya-Kay anisotropic HAIR: the highlight is a STREAK across the strand
        // direction, not a round dot. Two shifted specular lobes - a sharp, white
        // PRIMARY shifted toward the root and a broad, subsurface-tinted SECONDARY
        // shifted toward the tip - are what read as glossy hair. Tangent = strand
        // direction (hair-card U axis). No textures/extra samples; safe on a GTX 660.
        float3 Th = normalize(tangentWS - N * dot(tangentWS, N)); // strand dir on the surface
        const float shiftP = -0.04f, shiftS = 0.10f;
        float3 t1 = normalize(Th + shiftP * N);
        float3 t2 = normalize(Th + shiftS * N);
        const float dp = dot(t1, Hv), ds = dot(t2, Hv);
        const float expP = lerp(16.0f, 160.0f, saturate(1.0f - roughness)); // sharp primary
        const float sP = pow(sqrt(saturate(1.0f - dp * dp)), expP);
        const float sS = pow(sqrt(saturate(1.0f - ds * ds)), expP * 0.25f);  // broad secondary
        const float3 primary = sP.xxx;                    // white oil highlight (R lobe)
        const float3 secondary = sS * albedo * 0.6f;      // TRT lobe, tinted by hair colour
        specular = (primary + secondary) * Ft * radiance * NdotL;
    }
    else
    {
        // OpenPBR base: one GGX lobe whose Fresnel blends the dielectric response (Fdiel) with the
        // F82-tint metal response by metalness. metallic 0 + white specular_color == the old look.
        float3 Fmetal = FresnelF82Tint(VdotH, albedo, gMatExt.specular_color);
        Ft = lerp(Fdiel, Fmetal, metallic);
        float Dt = DistributionGGX(NdotH, roughness);
        float Gt = GeometrySmith(NdotV, NdotL, roughness);
        specular = (Dt * Gt * Ft) / max(4.0f * NdotV * NdotL, EPSILON) * radiance * NdotL;
        if ((gMaterialFlags & HBE_MAT_SUBSURFACE) != 0u)
        {
            // Skin has a DUAL specular lobe: a tight highlight sitting on a broad, soft
            // oily sheen. A single GGX dot reads plasticky ("weird highlight"); adding a
            // wider second lobe and blending (not adding - energy-preserving) makes the
            // highlight read wet and alive. One extra NDF/G eval, no new textures/samples
            // - safe down to a GTX 660.
            float rBroad = saturate(roughness * 2.0f + 0.18f);
            float Db = DistributionGGX(NdotH, rBroad);
            float Gb = GeometrySmith(NdotV, NdotL, rBroad);
            float3 broad = (Db * Gb * Ft) / max(4.0f * NdotV * NdotL, EPSILON) * radiance * NdotL;
            specular = lerp(specular, broad, 0.35f);
        }
    }

    // OpenPBR diffuse shape: energy-preserving Oren-Nayar (base_diffuse_roughness); sigma 0 == Lambert.
    float3 diffuseResponse = OrenNayarDiffuse(NdotL, NdotV, dot(L, V), gMatExt.base_diffuse_roughness).xxx;
    float3 transmission = 0.0f.xxx;
    if ((gMaterialFlags & HBE_MAT_SUBSURFACE) != 0u)
    {
        // Pre-integrated subsurface scattering: the LUT gives the soft, reddened
        // wrap of light around the terminator (curvature drives how far it
        // bleeds). Falls back to a wrapped diffuse if the LUT is unavailable.
        // Wrapped, reddened subsurface diffuse. Light bleeds a LITTLE past the terminator
        // (the soft skin look) and the transition band picks up the blood-scatter tint -
        // but the FORM is kept (the dark side stays dark). This is a pre-integrated wrap,
        // NOT a wash: the LUT path over-scattered at every curvature and flattened the
        // sphere, so this analytic model replaces it. The wrap width is driven by the
        // UNIFORM material radius (NOT screen-space fwidth curvature) so it can't speckle
        // per-facet at the terminator the way the derivative-based curvature did.
        const float sWrap = 0.20f + gSubsurfaceRadius * 0.10f;
        float wrapped = saturate((rawNdotL + sWrap) / (1.0f + sWrap));
        float band = saturate((sWrap - abs(rawNdotL)) / sWrap) * saturate(rawNdotL + sWrap);
        diffuseResponse = wrapped.xxx * lerp(1.0f.xxx, gSubsurfaceColor, saturate(band * 0.8f));
        // Subtle back-scatter on edge/thin regions (thickness map gates it; 0.5 default).
        float3 transL = normalize(L + N * 0.3f);
        float transDot = pow(saturate(dot(V, -transL)), 3.0f);
        transmission = gSubsurfaceColor * (transDot * (1.0f - thickness) * 0.35f) * radiance;
    }
    float3 kdDirect = (1.0f - Ft) * (1.0f - metallic);
    float3 diffuse = kdDirect * gMatExt.base_weight * albedo / PI * diffuseResponse * radiance;
    float3 result = diffuse + specular + transmission;

    // Clearcoat: a thin CLEAR dielectric layer over the whole material (wet skin,
    // sweat, wet eyes, blood sheen, varnish). A second GGX lobe with a fixed 0.04 F0
    // and its own (usually low) roughness. It also attenuates the layers beneath it by
    // its Fresnel so the surface can't gain energy - the base just dims where the wet
    // sheen takes over. Gated on gClearcoat, so a dry material pays nothing.
    if (gClearcoat > 0.0f)
    {
        // OpenPBR coat: dielectric microfacet layer, F0 from coat_ior (default 1.5 -> 0.04), over
        // the base; coat_color tints the light reaching the layers beneath it.
        const float ccF0 = IorToF0(gMatExt.coat_ior);
        const float ccF = (ccF0 + (1.0f - ccF0) * pow(saturate(1.0f - VdotH), 5.0f)) * saturate(gClearcoat);
        const float ccRough = max(gClearcoatRoughness, 0.02f);
        const float ccD = DistributionGGX(NdotH, ccRough);
        const float ccG = GeometrySmith(NdotV, NdotL, ccRough);
        const float ccSpec = (ccD * ccG * ccF) / max(4.0f * NdotV * NdotL, EPSILON) * NdotL;
        const float3 ccTint = lerp(1.0f.xxx, gMatExt.coat_color, saturate(gClearcoat));
        result = result * (1.0f - ccF) * ccTint + (ccSpec * radiance);
    }
    return result;
}

// Windowed inverse-square falloff (UE4-style): physically-based near the
// light, smoothly reaching exactly zero at `range`.
float DistanceAttenuation(float dist, float range)
{
    float d2 = dist * dist;
    float factor = d2 / max(range * range, EPSILON);
    float window = saturate(1.0f - factor * factor);
    return (window * window) / (d2 + 1.0f);
}

// Forward pass writes the lit colour plus a thin G-buffer (octahedral world
// normal + roughness/metalness) and screen-space motion vectors. The latter two
// targets are only bound for the main HDR scene pass; preview/legacy passes use
// a single-RT pipeline and these extra outputs are discarded.
// --- Weather surface-response noise -------------------------------------
// Cheap 2D value noise + 3-octave fBm, used to break up puddle coverage and snow
// accumulation across world-XZ so neither reads as a uniform coat. World-anchored
// (fed world position / a metre scale) so the pattern is stable as the camera moves.
float Wx_Hash(float2 p)
{
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}
float Wx_Noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = Wx_Hash(i);
    float b = Wx_Hash(i + float2(1.0f, 0.0f));
    float c = Wx_Hash(i + float2(0.0f, 1.0f));
    float d = Wx_Hash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float Wx_Fbm(float2 p)
{
    // Wrap the domain to a large integer period so fp32 keeps full precision near the
    // wrap origin: at raw world coordinates the pre-hash multiply pushes magnitudes into
    // low-ULP territory within a few km, banding the break-up. The pattern then repeats
    // every 1024 * scale metres (kilometres) - imperceptibly far.
    p -= 1024.0f * floor(p / 1024.0f);
    float v = 0.0f, a = 0.5f;
    [unroll] for (int i = 0; i < 3; ++i) { v += a * Wx_Noise(p); p *= 2.0f; a *= 0.5f; }
    return v; // ~[0, 0.875]
}

struct PSOutput
{
    float4 color    : SV_Target0;
    float4 gbuffer  : SV_Target1; // rg = OctEncode(N), b = roughness, a = metallic
    float2 velocity : SV_Target2; // curUV - prevUV (screen UV, v down)
};

PSOutput PSMain(VSOutput input)
{
    // Terrain hole mask: clip painted-transparent terrain pixels so cliff/cave models
    // show through. The mask rides in the (terrain-unused) thickness-texture slot; we
    // discard early, before any shading. Mesh UV == terrain-wide UV for terrain chunks.
    //
    // THIS MUST AGREE WITH THE COLLIDER, PIXEL FOR TRIANGLE. Jolt's heightfield drops a
    // triangle when ANY of its three corner samples is no-collision, so a hole is one
    // whole triangle wide in physics. Filter-sampling the mask at `input.uv` (what this
    // used to do) was wrong twice over: it clipped roughly the painted region, leaving a
    // band up to a full sample wide that was DRAWN as ground with no collision behind it
    // (the player fell through early, always on the dangerous side); and because the mask
    // is n x n while uv runs 0..1 over n-1 quads, it was stretched by half a texel at the
    // terrain edges, misregistering the visible hole against the grid it was painted on.
    if ((gMaterialFlags & HBE_MAT_TERRAIN_HOLE) != 0u && gThicknessIndex != 0u)
    {
        // uv * (n-1) IS the heightfield sample coordinate, so the containing quad is
        // floor() of it and the corners are exact texels.
        int n = int(BindlessSize(gThicknessIndex).x);
        float2 q = input.uv * float(max(n - 1, 1));
        int2 q0 = clamp(int2(floor(q)), int2(0, 0), int2(max(n - 2, 0), max(n - 2, 0)));
        float2 tq = saturate(q - float2(q0));
        // Same MAIN-diagonal split as BuildChunk and Jolt (tz >= tx picks the
        // (0,0),(0,1),(1,1) half); the third corner is what distinguishes them.
        int2 cC = (tq.y >= tq.x) ? q0 + int2(0, 1) : q0 + int2(1, 0);
        float hole = max(max(LoadBindless(gThicknessIndex, q0).r,
                             LoadBindless(gThicknessIndex, q0 + int2(1, 1)).r),
                         LoadBindless(gThicknessIndex, cC).r);
        if (hole > 0.5f) clip(-1.0f);
    }

    float3 V = normalize(gCameraPosWS - input.positionWS);

    // --- Eye: parallax iris -------------------------------------------------
    // The iris sits beneath the refractive cornea, so the visible iris point
    // shifts with view angle. Offset the UV along the tangent-space view ray.
    float2 uv = input.uv;
    if ((gMaterialFlags & HBE_MAT_EYE) != 0u)
    {
        float3 Ng = normalize(input.normalWS);
        float3 Tg = normalize(input.tangentWS.xyz);
        float3 Bg = cross(Ng, Tg) * input.tangentWS.w;
        float3 Vts = float3(dot(V, Tg), dot(V, Bg), dot(V, Ng));
        const float irisDepth = 0.045f;
        uv += (Vts.xy / max(Vts.z, 0.25f)) * irisDepth;
    }

    // --- Material sampling -------------------------------------------------
    float4 albedoTex = SampleBindless(gAlbedoIndex, uv);
    float3 albedo = gBaseColorFactor.rgb * albedoTex.rgb;

    // --- Terrain splat: blend up to 4 FULL MATERIALS (albedo + normal + metal/rough)
    // by a painted weight mask. The 4 layers' textures are in the gSplat* index arrays;
    // the weight mask is the emissive slot (terrain-wide mesh UV); layers tile by world
    // XZ / subsurfaceRadius(=tile). Normal/rough/metal are taken from the blend below.
    const bool splat = (gMaterialFlags & HBE_MAT_TERRAIN_SPLAT) != 0u;
    float3 splatNTS = float3(0.0f, 0.0f, 1.0f); // blended tangent-space normal
    float2 splatMRrm = float2(0.0f, 0.8f);      // blended (metal, rough)
    if (splat)
    {
        float2 tuv = input.positionWS.xz / max(gSubsurfaceRadius, 0.01f);
        float4 wt = (gEmissiveIndex != 0u) ? SampleBindless(gEmissiveIndex, uv)
                                           : float4(1.0f, 0.0f, 0.0f, 0.0f);
        float  wsum = wt.x + wt.y + wt.z + wt.w + 1e-4f;
        float3 a = 0.0f.xxx, n = 0.0f.xxx;
        float2 mr = 0.0f.xx;
        [unroll] for (int Li = 0; Li < 4; ++Li)
        {
            float wl = wt[Li] / wsum;
            uint ai = gSplatAlbedo[Li], ni = gSplatNormal[Li], mi = gSplatMR[Li];
            a  += ((ai != 0u) ? SampleBindless(ai, tuv).rgb : 0.5f.xxx) * wl;
            // Z reconstructed from XY (BC5-safe; exact for uncompressed). ni==0 -> flat (0,0,1).
            float2 snxy = (ni != 0u) ? (SampleBindless(ni, tuv).xy * 2.0f - 1.0f) : float2(0.0f, 0.0f);
            n  += float3(snxy, sqrt(saturate(1.0f - dot(snxy, snxy)))) * wl;
            // glTF MR: blue = metallic, green = roughness. Roughness is the MR map's
            // green (1 if the layer has no MR map) TIMES the material's roughness factor,
            // so cranking a layer material's roughness actually mattes the terrain.
            float lm = (mi != 0u) ? SampleBindless(mi, tuv).b : 0.0f;
            float lr = ((mi != 0u) ? SampleBindless(mi, tuv).g : 1.0f) * gSplatRough[Li];
            mr += float2(lm, lr) * wl;
        }
        albedo = gBaseColorFactor.rgb * a;
        splatNTS = (dot(n, n) > 1e-5f) ? normalize(n) : float3(0.0f, 0.0f, 1.0f);
        splatMRrm = mr;
    }

    float metallic  = gMetallicFactor;
    float roughness = gRoughnessFactor;
    if (gMRIndex != 0 && !splat)
    {
        // glTF packing: blue = metallic, green = roughness.
        float3 mr = SampleBindless(gMRIndex, uv).rgb;
        metallic  *= mr.b;
        roughness *= mr.g;
    }

    // Splat roughness = the blended layer-material roughness, but FLOORED by the terrain's
    // own roughness factor (gRoughnessFactor = the terrain Roughness slider). So cranking
    // the terrain Roughness forces the whole surface matte no matter how glossy a layer's
    // material/MR map is - a direct "make the ground matte" override.
    if (splat) { metallic = splatMRrm.x; roughness = max(splatMRrm.y, gRoughnessFactor); }
    metallic  = saturate(metallic);
    roughness = clamp(roughness, 0.04f, 1.0f);
    // The cornea is a smooth, wet refractive layer: keep a crisp catchlight, but not
    // the near-mirror r=0.06 that made a SINGULAR, aliasing pinpoint. 0.12 is still
    // sharp and stops the shimmer.
    if ((gMaterialFlags & HBE_MAT_EYE) != 0u) roughness = min(roughness, 0.12f);

    float ao = (gAOIndex != 0 && !splat) ? SampleBindless(gAOIndex, uv).r : 1.0f;

    float3 N;
    if (splat)
    {
        // World-aligned tangent frame matching the world-XZ tiling (u = world X), so
        // the blended layer normal maps apply correctly even though the terrain mesh
        // has no per-vertex tangents.
        float3 nn = normalize(input.normalWS);
        float3 t = normalize(float3(1.0f, 0.0f, 0.0f) - nn * nn.x);
        // b aligns with +Z (= the tiling's +V axis), matching the glTF/OpenGL green-up
        // normal-map convention; cross(nn,t) would point -Z and invert relief along Z.
        float3 b = cross(t, nn);
        N = normalize(mul(splatNTS, float3x3(t, b, nn)));
    }
    else
        N = (gNormalIndex != 0) ? ApplyNormalMap(input.normalWS, input.tangentWS, uv)
                                : normalize(input.normalWS);

    if (splat)
    {
        // Geometric specular antialiasing: the tiled layer normal maps + the heightfield
        // make N change fast per pixel, so the specular sparkles at grazing angles
        // ("mirror grain"). Roughen by the per-pixel normal variation so the highlight
        // stops shimmering; this also helps when a layer's authored roughness is high.
        float nv = saturate(length(fwidth(N)) * 1.5f);
        roughness = clamp(max(roughness, nv), 0.04f, 1.0f);
    }

    // Subsurface inputs (consumed by ShadeDirect when the subsurface flag is on):
    // screen-space curvature drives how far light wraps; a thickness map gates
    // back-lit transmission.
    //
    // fwidth() is a per-2x2-quad derivative. On a smooth, dense mesh that reads real
    // surface curvature - but on a low-poly / hard-normal skin mesh it is roughly
    // CONSTANT within each triangle and DIFFERENT per triangle (and spikes at the
    // creases), so the LUT paints each facet a slightly different scatter = the blotchy,
    // triangular patches on un-smoothed skin. Cap the raw ratio and ease its strength so
    // a facet can only ever NUDGE the wrap, never slam it to the deep-scatter end:
    // smoothly-curved skin keeps its soft terminator (its curvature already sits under
    // the cap), faceted skin stops blotching. gSubsurfaceRadius still scales it, so the
    // material's "SSS Radius" dials the effect up without bringing the blotches back.
    float rawCurv = length(fwidth(N)) / max(length(fwidth(input.positionWS)), 1e-4f);
    float curvature = saturate(min(rawCurv, 1.0f) * gSubsurfaceRadius * 0.15f);
    float thickness = (gThicknessIndex != 0) ? SampleBindless(gThicknessIndex, uv).r : 0.5f;

    // --- Art Editor surface paint -------------------------------------------
    // A per-object paint stack (flattened on the CPU) composited over the base
    // material: colour canvas (RGB albedo, A coverage) + material canvas (R metal,
    // G roughness, B relief height, A material coverage). Far surfaces sample
    // coarser mips (normal minification + a distance bias) so strokes average into
    // broader washes; the relief height perturbs the normal so strokes catch light.
    if (gPaintColorIndex != 0)
    {
        // Paint coordinate: mesh UV, or a box projection that doesn't stretch on
        // scaled geometry. ddx/ddy are read HERE (uniform control flow) for the
        // manual mip LOD so the texture reads below are SampleLevel - safe inside
        // the per-pixel coverage branch.
        float2 puv = (gPaintProjMode == 1u) ? BoxPaintUV(input.positionOS, input.normalOS) : uv;
        float2 dUVdx = ddx(puv);
        float2 dUVdy = ddy(puv);
        float  texSize = (gPaintTexel > 0.0f) ? (1.0f / gPaintTexel) : 1024.0f;
        float  d2 = max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy)) * texSize * texSize;
        float  baseLod = 0.5f * log2(max(d2, 1e-8f));
        float  pdist = length(gCameraPosWS - input.positionWS);
        float  distLod = max(0.0f, log2(max(pdist, 1.0f) * 0.25f)) * gPaintLodBias;
        float  lod = max(baseLod + distLod, 0.0f);

        float4 paint = SampleBindlessLod(gPaintColorIndex, puv, lod);
        float4 pmat  = (gPaintHeightIndex != 0) ? SampleBindlessLod(gPaintHeightIndex, puv, lod)
                                                : float4(0.0f, 0.5f, 0.5f, 0.0f);
        float  pa = saturate(paint.a * gPaintOpacity); // colour coverage
        float  ma = saturate(pmat.a * gPaintOpacity);  // material coverage
        [branch] if (pa > 0.0039f || ma > 0.0039f)     // skip un-painted texels
        {
            albedo    = lerp(albedo, paint.rgb, pa);
            metallic  = lerp(metallic, pmat.r, ma);     // painted PBR material
            roughness = lerp(roughness, pmat.g, ma);
            if (gPaintHeightScale > 0.0f && gPaintTexel > 0.0f && ma > 0.0039f)
            {
                // Central-difference the relief height (B channel, 0.5 neutral).
                float hL = SampleBindlessLod(gPaintHeightIndex, puv - float2(gPaintTexel, 0.0f), lod).b;
                float hR = SampleBindlessLod(gPaintHeightIndex, puv + float2(gPaintTexel, 0.0f), lod).b;
                float hD = SampleBindlessLod(gPaintHeightIndex, puv - float2(0.0f, gPaintTexel), lod).b;
                float hU = SampleBindlessLod(gPaintHeightIndex, puv + float2(0.0f, gPaintTexel), lod).b;

                float3 Ng = normalize(N);
                float3 Tg = normalize(input.tangentWS.xyz - Ng * dot(Ng, input.tangentWS.xyz));
                float3 Bg = cross(Ng, Tg) * input.tangentWS.w;
                float  amt = gPaintHeightScale * ma * 48.0f; // impasto: ridges rake light
                // Tilt the normal away from rising height (gradient points uphill).
                // Clamp the slope so a steep bristle ridge tilts hard but never folds
                // the normal past the surface (keeps the facets light-catching, stable).
                float2 grad = float2(hR - hL, hU - hD) * amt;
                grad = clamp(grad, -2.0f, 2.0f);
                N = normalize(Ng - (Tg * grad.x + Bg * grad.y));
            }
            // Keep painted PBR values in the BRDF-safe range.
            metallic  = saturate(metallic);
            roughness = clamp(roughness, 0.04f, 1.0f);
        }
    }

    // --- Forward projected decals -------------------------------------------
    // Each decal box projects its albedo/normal/MR onto the surfaces it overlaps, blended
    // into the material BEFORE lighting (correctly lit, conforms to any geometry) and
    // BEFORE the weather block (so snow/wet sit on top of a decal). Uniform branch on the
    // decal count keeps it free when there are none.
    [branch] if (gDecalCount > 0u)
    {
        float3 geoN = normalize(input.normalWS);
        // World-position derivatives taken in UNIFORM flow (the branch is on a frame
        // constant), so the per-decal loop below - which `continue`s divergently - can
        // sample with EXPLICIT gradients instead of undefined implicit ones.
        float3 dPwx = ddx(input.positionWS);
        float3 dPwy = ddy(input.positionWS);
        for (uint di = 0; di < gDecalCount; ++di)
        {
            Decal dc = gDecals[di];
            float facing = dot(geoN, -dc.forwardWS); // surface must face the projector
            if (facing <= 0.0f) continue;
            float3 lp = mul(dc.invWorld, float4(input.positionWS, 1.0f)).xyz;
            float3 al = abs(lp);
            if (al.x > 0.5f || al.y > 0.5f || al.z > 0.5f) continue; // outside the box
            float2 duv = lp.xy + 0.5f;                               // project along local Z
            // Explicit UV gradients: d(lp) = invWorld * d(Pw) (w=0, translation drops out).
            float2 duvDx = mul(dc.invWorld, float4(dPwx, 0.0f)).xy;
            float2 duvDy = mul(dc.invWorld, float4(dPwy, 0.0f)).xy;
            float4 dalb = (dc.albedoIndex != 0u)
                              ? SampleBindlessGrad(dc.albedoIndex, duv, duvDx, duvDy)
                              : float4(1.0f, 1.0f, 1.0f, 1.0f);
            float edge = saturate((0.5f - max(al.x, al.y)) * 8.0f);  // soft UV border
            float cov = dalb.a * dc.opacity * pow(saturate(facing), dc.angleFade) * edge;
            if (cov <= 0.001f) continue;
            if (dc.albedoIndex != 0u) albedo = lerp(albedo, dalb.rgb, cov);
            if (dc.normalIndex != 0u)
            {
                float3 nts = SampleBindlessGrad(dc.normalIndex, duv, duvDx, duvDy).xyz * 2.0f - 1.0f;
                // Right-handed frame matching the mesh convention (ApplyNormalMap uses
                // b = cross(n, t)): with N = -forwardWS, B = cross(tangentWS, forwardWS)
                // gives cross(T, B) = +N, so a decal normal map's green channel is not
                // inverted relative to the same map on a mesh surface.
                float3 B = cross(dc.tangentWS, dc.forwardWS);
                float3 dN = normalize(nts.x * dc.tangentWS + nts.y * B + nts.z * (-dc.forwardWS));
                N = normalize(lerp(N, dN, cov * dc.params.x));
            }
            if (dc.mrIndex != 0u)
            {
                float3 mr = SampleBindlessGrad(dc.mrIndex, duv, duvDx, duvDy).rgb; // b=metal g=rough
                metallic  = lerp(metallic, mr.b, cov);
                roughness = lerp(roughness, mr.g, cov);
            }
            else
            {
                roughness = lerp(roughness, dc.params.y, cov);
                metallic  = lerp(metallic, dc.params.z, cov);
            }
        }
        roughness = clamp(roughness, 0.02f, 1.0f);
        metallic  = saturate(metallic);
    }

    // --- Weather surface response (wet / puddles / snow) --------------------
    // Global ground response driven by the weather state (gWeather2/gWeather3),
    // written into albedo/roughness/metallic/N BEFORE lighting so the wet gloss and
    // the puddle mirrors flow through the sun, punctual lights, IBL AND - via the
    // G-buffer (roughness/normal) - the screen-space reflections. Skipped entirely
    // when the scene is dry and snowless, so clear weather costs one compare.
    {
        float wetAmt    = gWeather2.x;
        float puddleAmt = gWeather2.y;
        float snowAmt   = gWeather2.z;
        [branch] if (wetAmt > 0.001f || puddleAmt > 0.001f || snowAmt > 0.001f)
        {
            float3 Pw  = input.positionWS;
            float  up  = normalize(input.normalWS).y; // geometric up-facing-ness
            float  puddleScale = max(gWeather3.x, 0.5f);
            float  snowScale   = max(gWeather3.y, 0.5f);

            // Environmental weather is a WORLD-SURFACE effect, not a character one:
            // standing water on skin or snow on a cornea reads wrong (and the snow
            // block would otherwise stomp the deliberate HBE_MAT_EYE roughness cap).
            // Skip snow/puddles on eye/skin/hair; wet darkening still applies to
            // skin/hair/cloth (wet hair and a wet coat are realistic) but not the eye
            // (its wet cornea is already modelled by clearcoat / the eye path).
            const bool wxEye  = (gMaterialFlags & HBE_MAT_EYE) != 0u;
            const bool wxChar = (gMaterialFlags &
                                 (HBE_MAT_EYE | HBE_MAT_SUBSURFACE | HBE_MAT_HAIR)) != 0u;

            // Wetness (porosity darkening): a wet surface is darker and glossier
            // everywhere, not just in the puddles. Disney's wet approximation.
            if (wetAmt > 0.001f && !wxEye)
            {
                float w = saturate(wetAmt);
                albedo   *= lerp(1.0f, 0.62f, w);
                roughness = lerp(roughness, roughness * 0.35f + 0.03f, w);
            }

            // Puddles: standing water collects in flat, up-facing, "low" spots picked
            // by a world-XZ fBm thresholded against the coverage. Where it floods, the
            // surface becomes a near-mirror dark dielectric with an up normal, so the
            // existing SSR pass reflects the scene in it. The +Y epsilon keeps the
            // lerp->normalize non-zero at the measure-zero N == -Y, puddle == 0.5 case.
            if (puddleAmt > 0.001f && !wxChar)
            {
                float flat   = saturate((up - 0.55f) / 0.35f);
                float pn     = Wx_Fbm(Pw.xz / puddleScale);
                float puddle = flat * saturate((puddleAmt - pn) * 4.0f);
                if (puddle > 0.001f)
                {
                    albedo    = lerp(albedo, float3(0.02f, 0.03f, 0.035f), puddle);
                    roughness = lerp(roughness, 0.03f, puddle);
                    metallic  = lerp(metallic, 0.0f, puddle);
                    N = normalize(lerp(N, float3(0.0f, 1.0f, 0.0f), puddle) + float3(0.0f, 1e-4f, 0.0f));
                    // Rain ripples: while it is actively raining (precip intensity in
                    // gWeather2.w), animate the puddle surface with a couple of moving
                    // noise taps so standing water reads as rained-on, not glass.
                    float rip = gWeather2.w * puddle;
                    if (rip > 0.001f)
                    {
                        float  t  = gWeather.w;               // seconds
                        float2 rp = Pw.xz * 6.0f;
                        const float e = 0.15f;
                        float h0 = Wx_Noise(rp + t * 1.3f) + Wx_Noise(rp * 2.1f - t * 1.7f) * 0.5f;
                        float hx = Wx_Noise(rp + float2(e, 0.0f) + t * 1.3f) +
                                   Wx_Noise((rp + float2(e, 0.0f)) * 2.1f - t * 1.7f) * 0.5f;
                        float hy = Wx_Noise(rp + float2(0.0f, e) + t * 1.3f) +
                                   Wx_Noise((rp + float2(0.0f, e)) * 2.1f - t * 1.7f) * 0.5f;
                        float2 g = float2(hx - h0, hy - h0) / e;
                        N = normalize(N + float3(g.x, 0.0f, g.y) * (rip * 0.15f));
                    }
                }
            }

            // Snow accumulation: settles on up-facing surfaces (steep slopes shed it),
            // broken up by noise so it patches in rather than a uniform white coat.
            // Sits on TOP of wet/puddles - snow is the last thing to land.
            if (snowAmt > 0.001f && !wxChar)
            {
                float slope = saturate((up - 0.30f) / 0.5f);
                float sn    = Wx_Fbm(Pw.xz / snowScale);
                float cov   = slope * saturate((snowAmt * 1.4f - 0.2f - (sn - 0.4f) * 0.6f) * 3.0f);
                cov = saturate(cov) * saturate(snowAmt * 4.0f);
                if (cov > 0.001f)
                {
                    albedo    = lerp(albedo, float3(0.90f, 0.92f, 0.96f), cov);
                    roughness = lerp(roughness, 0.82f, cov);
                    metallic  = lerp(metallic, 0.0f, cov);
                    N = normalize(lerp(N, float3(0.0f, 1.0f, 0.0f), cov * 0.6f) + float3(0.0f, 1e-4f, 0.0f));
                }
            }

            roughness = clamp(roughness, 0.02f, 1.0f);
            metallic  = saturate(metallic);
        }
    }

    // --- Direct lighting ----------------------------------------------------
    float  NdotV = max(dot(N, V), EPSILON);
    // OpenPBR dielectric F0 from the specular IOR (default 1.5 -> 0.04), scaled by specular_weight
    // and tinted by specular_color. Skin keeps its slightly lower waxy reflectance (<= 0.028).
    float3 dielF0 = (IorToF0(gMatExt.specular_ior) * gMatExt.specular_weight) * gMatExt.specular_color;
    if ((gMaterialFlags & HBE_MAT_SUBSURFACE) != 0u) dielF0 = min(dielF0, 0.028f.xxx);
    float3 F0 = lerp(dielF0, albedo, metallic); // mixed dielectric/metal F0 for the ambient split-sum

    // Sun (one directional light, shadowed).
    float3 L = normalize(gLightDirWS);
    float  shadow = ShadowFactor(input.positionWS, max(dot(N, L), 0.0f));
    float3 Lo = ShadeDirect(N, V, L, gLightColor * gLightIntensity, albedo,
                            metallic, roughness, dielF0, NdotV, curvature, thickness,
                            input.tangentWS.xyz) * shadow;

    // Punctual lights: point (0), spot (1), rect/area (2).
    for (uint li = 0; li < gPunctualCount; ++li)
    {
        PunctualLight light = gPunctualLights[li];
        float3 P = input.positionWS;
        float3 Lp;
        float  dist;
        float  atten;
        if (light.isSpot == 2u)
        {
            // Rect/area light: shade from the closest point on the panel (the
            // representative-point approximation) for a soft near-field area look.
            // innerCos/outerCos carry the half-width/height; directionWS is the
            // emitting normal.
            float3 n = normalize(light.directionWS);
            float3 up0 = abs(n.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 right = normalize(cross(up0, n));
            float3 up = cross(n, right);
            float3 d = P - light.positionWS;
            float2 uv = clamp(float2(dot(d, right), dot(d, up)),
                              float2(-light.innerCos, -light.outerCos),
                              float2(light.innerCos, light.outerCos));
            float3 nearest = light.positionWS + right * uv.x + up * uv.y;
            float3 toLight = nearest - P;
            dist = length(toLight);
            if (dist >= light.range) continue;
            Lp = toLight / max(dist, EPSILON);
            // One-sided unless twoSided (_padL.x): only lit on the +normal face.
            float side = dot(P - light.positionWS, n);
            atten = DistanceAttenuation(dist, light.range) *
                    ((side > 0.0f || light._padL.x > 0.5f) ? 1.0f : 0.0f);
        }
        else
        {
            float3 toLight = light.positionWS - P;
            dist = length(toLight);
            if (dist >= light.range) continue;
            Lp = toLight / max(dist, EPSILON);
            atten = DistanceAttenuation(dist, light.range);
            if (light.isSpot == 1u)
            {
                // Angular falloff between the inner and outer cone.
                float cd = dot(-Lp, normalize(light.directionWS));
                atten *= saturate((cd - light.outerCos) /
                                  max(light.innerCos - light.outerCos, 1e-4f));
            }
        }
        if (atten <= 0.0f)
            continue;
        Lo += ShadeDirect(N, V, Lp, light.color * light.intensity * atten, albedo,
                          metallic, roughness, dielF0, NdotV, curvature, thickness,
                          input.tangentWS.xyz);
    }

    // --- Ambient: image-based lighting (local probes over a global sky) ----
    float3 F  = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 Rdir = reflect(-V, N);
    float2 uvN = EquirectUV(N);
    float2 uvR = EquirectUV(Rdir);

    float3 ambient;
    if (gGiShIndex != 0u)
    {
        // Baked SH-L1 irradiance volume: smooth, directional, leak-weighted diffuse
        // GI (the upgrade over box probes). Specular comes from the LOCAL volume too
        // (the SH evaluated in the reflection direction) so a sealed room reflects
        // its own interior, not the global sky - no blue sky leak indoors.
        float3 irradiance  = SampleGiVolume(input.positionWS, N);
        float3 diffuseIBL  = irradiance * albedo;
        float3 specEnv     = SampleGiVolume(input.positionWS, Rdir);
        float2 brdf        = SampleBindless(gBrdfLUTIndex, float2(NdotV, roughness)).rg;
        float3 specularIBL = specEnv * (F * brdf.x + brdf.y);
        float specOcc = saturate(pow(NdotV + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
        float horizon = saturate(1.0f + dot(Rdir, normalize(input.normalWS)));
        specularIBL *= specOcc * horizon * horizon;
        ambient = (kD * diffuseIBL + specularIBL) * gAmbientIntensity * ao;
    }
    else if (gIrradianceIndex != 0 || gProbeCount > 0)
    {
        // Local light/reflection probes: per-pixel, box-weighted blend. A surface
        // deep inside a probe samples the room's baked environment; in the fade
        // band at the box edge it blends with neighbours / the sky, so walking
        // between rooms is smooth (no popping).
        float3 irr = 0.0f, pre = 0.0f;
        float  wSum = 0.0f;
        [loop] for (uint pi = 0; pi < gProbeCount; ++pi)
        {
            Probe pr = gProbes[pi];
            if (pr.irradianceIndex == 0) continue;
            float3 dd = abs(input.positionWS - pr.center) - pr.halfExtents;
            float outside = max(max(dd.x, dd.y), dd.z); // <= 0 inside, 0 at the face
            float w = saturate(-outside / max(pr.blend, 1e-3f));
            if (w <= 0.0f) continue;
            // Leak reduction: a probe on the BACK side of the surface shouldn't
            // light it (a sky probe above the ceiling must not bleed onto the room
            // below). Weight by how much the probe sits toward the surface normal.
            float3 toProbe = pr.center - input.positionWS;
            float len = length(toProbe);
            float3 pdir = len > 1e-3f ? toProbe / len : N;
            w *= saturate(dot(N, pdir) * 0.6f + 0.5f);
            if (w <= 1e-4f) continue;
            irr += SampleBindless(pr.irradianceIndex, uvN).rgb * w;
            pre += SampleBindlessLod(pr.prefilteredIndex, uvR,
                                     roughness * pr.prefilteredMaxLod).rgb * w;
            wSum += w;
        }

        float3 skyIrr = (gIrradianceIndex != 0) ? SampleBindless(gIrradianceIndex, uvN).rgb : 0.0f;
        float3 skyPre = (gPrefilteredIndex != 0)
                            ? SampleBindlessLod(gPrefilteredIndex, uvR,
                                                roughness * gPrefilteredMaxLod).rgb
                            : 0.0f;
        float3 irradiance, prefiltered;
        if (wSum > 0.0f)
        {
            float localW = saturate(wSum); // < 1 only in the edge fade band
            irradiance  = lerp(skyIrr, irr / wSum, localW);
            prefiltered = lerp(skyPre, pre / wSum, localW);
        }
        else
        {
            irradiance = skyIrr;
            prefiltered = skyPre;
        }

        float3 diffuseIBL  = irradiance * albedo;
        float2 brdf        = SampleBindless(gBrdfLUTIndex, float2(NdotV, roughness)).rg;
        float3 specularIBL = prefiltered * (F * brdf.x + brdf.y);

        // Occlude the reflection so matte / hidden surfaces don't mirror the
        // environment as a sheen (specular occlusion + horizon occlusion).
        float specOcc = saturate(pow(NdotV + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
        float horizon = saturate(1.0f + dot(Rdir, normalize(input.normalWS)));
        specularIBL *= specOcc * horizon * horizon;

        ambient = (kD * diffuseIBL + specularIBL) * gAmbientIntensity * ao;
    }
    else
    {
        ambient = kD * albedo * gAmbientIntensity * ao;
    }

    // Clearcoat environment reflection: the wet/oily sheen mirrors the sky + bright
    // sources. A thin Fresnel-weighted lobe over the ambient result, sampling the
    // prefiltered sky at the clear layer's (usually low) roughness. Fresnel here uses
    // NdotV so the sheen rims the silhouette - exactly where wet skin catches light.
    if (gClearcoat > 0.0f)
    {
        const float ccF0a = IorToF0(gMatExt.coat_ior);
        const float ccFa =
            (ccF0a + (1.0f - ccF0a) * pow(saturate(1.0f - NdotV), 5.0f)) * saturate(gClearcoat);
        float3 ccEnv = (gPrefilteredIndex != 0)
            ? SampleBindlessLod(gPrefilteredIndex, uvR,
                                max(gClearcoatRoughness, 0.02f) * gPrefilteredMaxLod).rgb
            : 0.0f;
        ambient = ambient * (1.0f - ccFa) + ccEnv * (ccFa * gAmbientIntensity * ao);
    }

    // --- Emissive (self-illumination, unaffected by lighting) --------------
    float3 emissive = gEmissiveColor * gEmissiveIntensity;
    if (gEmissiveIndex != 0 && !splat) // for splat the emissive slot is the weight mask
        emissive *= SampleBindless(gEmissiveIndex, uv).rgb;

    float3 color = ambient + Lo + emissive;
    if (gOutputLinear == 0)
    {
        // Legacy direct-to-LDR path (no post stack): tonemap inline.
        color *= gExposure;
        color  = TonemapACES(color);
        color  = LinearToSRGB(color);
    }

    // HDR alpha doubles as the PAINTERLY MASK in the post pipeline: 1 = dynamic-layer
    // object (exempt -> stays crisp), 0 = static (gets the painted finish). A later
    // composite reads it to restore lit colour over the painterly result. (Legacy
    // direct-to-LDR path has no post, so opaque stays fully covered = 1.)
    float outAlpha = 1.0f;
    if (gOutputLinear != 0) {
        // 2-bit painterly mask: bit0 = dynamic-exempt (restore crisp), bit1 =
        // censored object (paint strokes onto its surface). 0=static, .33=dynamic,
        // .67=censored static, 1=censored dynamic. Decoded by the composite + strokes.
        uint mbits = 0u;
        if ((gMaterialFlags & HBE_MAT_PAINTERLY_EXEMPT) != 0u) mbits |= 1u;
        if ((gMaterialFlags & HBE_MAT_CENSORED) != 0u) mbits |= 2u;
        outAlpha = float(mbits) / 3.0f;
    }
    // Alpha-blended materials (e.g. painterly stroke decals) output straight-alpha
    // coverage = base alpha * albedo-texture alpha; the transparent pass blends them
    // over the lit scene (this overrides the mask for those rare dynamic decals).
    if ((gMaterialFlags & HBE_MAT_TRANSPARENT) != 0u)
        outAlpha = saturate(gMatExt.geometry_opacity * albedoTex.a); // OpenPBR geometry_opacity (== legacy baseColor.a)

    PSOutput o;
    o.color    = float4(color, outAlpha);
    o.gbuffer  = float4(OctEncode(N), roughness, metallic);
    o.velocity = ClipToScreenUV(input.curClip) - ClipToScreenUV(input.prevClip);
    return o;
}

// Shaders/MeshPBR.hlsl - textured metallic-roughness PBR with IBL.
//
// Material maps (base color / normal / metallic-roughness / AO) are sampled from
// the bindless texture array by per-object index (0 = use the constant factor).
#include "Common.hlsli"
#include "BRDF.hlsli"

struct VSInput
{
    float3 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    float4 tangentOS  : TANGENT;   // .w = handedness
    float2 uv         : TEXCOORD0;
    uint4  joints     : BLENDINDICES; // skeleton joint indices
    float4 weights    : BLENDWEIGHT;  // all zero on static meshes
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

VSOutput VSMain(VSInput input)
{
    float3 posOS = input.positionOS;
    float3 nrmOS = input.normalOS;
    float3 tanOS = input.tangentOS.xyz;
    float3 prevPosOS = input.positionOS; // skinned with the previous palette

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
    float4 posWS = mul(gModel, float4(posOS, 1.0f));
    o.positionWS = posWS.xyz;
    o.positionCS = mul(gViewProj, posWS);
    o.normalWS   = normalize(mul((float3x3)gNormalMatrix, nrmOS));
    o.tangentWS  = float4(normalize(mul((float3x3)gModel, tanOS)), input.tangentOS.w);
    o.uv         = input.uv;
    o.curClip    = o.positionCS;
    o.prevClip   = mul(gPrevViewProj, mul(gPrevModel, float4(prevPosOS, 1.0f)));
    o.positionOS = posOS; // object space (post-skin) for box paint projection
    o.normalOS   = nrmOS;
    return o;
}

// Perturbs the world normal with a tangent-space normal map.
float3 ApplyNormalMap(float3 N, float4 T, float2 uv)
{
    float3 tn = SampleBindless(gNormalIndex, uv).xyz * 2.0f - 1.0f;
    float3 n = normalize(N);
    float3 t = normalize(T.xyz - n * dot(n, T.xyz)); // Gram-Schmidt
    float3 b = cross(n, t) * T.w;
    return normalize(mul(tn, float3x3(t, b, n)));
}

// Cook-Torrance direct response for one light direction. `radiance` is the
// light's incoming radiance at the shaded point; subsurface materials get a
// colored wrapped diffuse (light bleeds past the terminator, red furthest).
float3 ShadeDirect(float3 N, float3 V, float3 L, float3 radiance, float3 albedo,
                   float metallic, float roughness, float3 F0, float NdotV,
                   float curvature, float thickness)
{
    float3 Hv = normalize(V + L);
    float  rawNdotL = dot(N, L);
    float  NdotL = max(rawNdotL, 0.0f);
    float  NdotH = max(dot(N, Hv), 0.0f);
    float  VdotH = max(dot(V, Hv), 0.0f);

    float3 Ft = FresnelSchlick(VdotH, F0);
    float3 specular;
    if ((gMaterialFlags & HBE_MAT_CLOTH) != 0u)
    {
        // Fabric: Charlie sheen NDF + Neubelt visibility (soft grazing-angle
        // fuzz) instead of GGX, with an achromatic sheen tint.
        specular = D_Charlie(roughness, NdotH) * V_Neubelt(NdotV, NdotL)
                 * (0.3f * radiance * NdotL);
    }
    else
    {
        float Dt = DistributionGGX(NdotH, roughness);
        float Gt = GeometrySmith(NdotV, NdotL, roughness);
        specular = (Dt * Gt * Ft) / max(4.0f * NdotV * NdotL, EPSILON) * radiance * NdotL;
    }

    float3 diffuseResponse = NdotL.xxx;
    float3 transmission = 0.0f.xxx;
    if ((gMaterialFlags & HBE_MAT_SUBSURFACE) != 0u)
    {
        // Pre-integrated subsurface scattering: the LUT gives the soft, reddened
        // wrap of light around the terminator (curvature drives how far it
        // bleeds). Falls back to a wrapped diffuse if the LUT is unavailable.
        diffuseResponse = (gSkinLUTIndex != 0)
            ? SampleBindless(gSkinLUTIndex, float2(rawNdotL * 0.5f + 0.5f, curvature)).rgb
            : saturate((rawNdotL.xxx + gSubsurfaceColor * 0.5f) / (1.0f + gSubsurfaceColor * 0.5f));
        // Translucency: light transported through thin, back-lit regions
        // (Frostbite-style transmission), strongest where thickness is low.
        float3 transL = normalize(L + N * 0.25f);
        float  transDot = pow(saturate(dot(V, -transL)), 4.0f);
        transmission = gSubsurfaceColor * (transDot * (1.0f - thickness)) * radiance;
    }
    float3 kdDirect = (1.0f - Ft) * (1.0f - metallic);
    float3 diffuse = kdDirect * albedo / PI * diffuseResponse * radiance;
    return diffuse + specular + transmission;
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
struct PSOutput
{
    float4 color    : SV_Target0;
    float4 gbuffer  : SV_Target1; // rg = OctEncode(N), b = roughness, a = metallic
    float2 velocity : SV_Target2; // curUV - prevUV (screen UV, v down)
};

PSOutput PSMain(VSOutput input)
{
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

    float metallic  = gMetallicFactor;
    float roughness = gRoughnessFactor;
    if (gMRIndex != 0)
    {
        // glTF packing: blue = metallic, green = roughness.
        float3 mr = SampleBindless(gMRIndex, uv).rgb;
        metallic  *= mr.b;
        roughness *= mr.g;
    }
    metallic  = saturate(metallic);
    roughness = clamp(roughness, 0.04f, 1.0f);
    // The cornea is a smooth, wet refractive layer: force a sharp specular so
    // eyes get a crisp catchlight.
    if ((gMaterialFlags & HBE_MAT_EYE) != 0u) roughness = min(roughness, 0.06f);

    float ao = (gAOIndex != 0) ? SampleBindless(gAOIndex, uv).r : 1.0f;

    float3 N = (gNormalIndex != 0) ? ApplyNormalMap(input.normalWS, input.tangentWS, uv)
                                   : normalize(input.normalWS);

    // Subsurface inputs (consumed by ShadeDirect when the subsurface flag is on):
    // screen-space curvature drives how far light wraps; a thickness map gates
    // back-lit transmission.
    float curvature = saturate(length(fwidth(N)) /
                               max(length(fwidth(input.positionWS)), 1e-4f) * gSubsurfaceRadius);
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

    // --- Direct lighting ----------------------------------------------------
    float  NdotV = max(dot(N, V), EPSILON);
    float3 F0 = lerp(0.04f.xxx, albedo, metallic);

    // Sun (one directional light, shadowed).
    float3 L = normalize(gLightDirWS);
    float  shadow = ShadowFactor(input.positionWS, max(dot(N, L), 0.0f));
    float3 Lo = ShadeDirect(N, V, L, gLightColor * gLightIntensity, albedo,
                            metallic, roughness, F0, NdotV, curvature, thickness) * shadow;

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
                          metallic, roughness, F0, NdotV, curvature, thickness);
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

    // --- Emissive (self-illumination, unaffected by lighting) --------------
    float3 emissive = gEmissiveColor * gEmissiveIntensity;
    if (gEmissiveIndex != 0)
        emissive *= SampleBindless(gEmissiveIndex, uv).rgb;

    float3 color = ambient + Lo + emissive;
    if (gOutputLinear == 0)
    {
        // Legacy direct-to-LDR path (no post stack): tonemap inline.
        color *= gExposure;
        color  = TonemapACES(color);
        color  = LinearToSRGB(color);
    }

    // Alpha-blended materials (e.g. painterly stroke decals) output straight-alpha
    // coverage = base alpha * albedo-texture alpha; the transparent pass blends
    // them over the lit scene. Opaque materials stay fully covered.
    float outAlpha = 1.0f;
    if ((gMaterialFlags & HBE_MAT_TRANSPARENT) != 0u)
        outAlpha = saturate(gBaseColorFactor.a * albedoTex.a);

    PSOutput o;
    o.color    = float4(color, outAlpha);
    o.gbuffer  = float4(OctEncode(N), roughness, metallic);
    o.velocity = ClipToScreenUV(input.curClip) - ClipToScreenUV(input.prevClip);
    return o;
}

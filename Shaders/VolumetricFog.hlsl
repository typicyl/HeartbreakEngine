// Shaders/VolumetricFog.hlsl - ray-marched volumetric fog + light scattering.
//
// Marches from the camera to the scene depth, accumulating sun in-scatter
// (sampled through the cascaded shadow map, so windows / gaps cast god-rays)
// plus punctual-light in-scatter with a Henyey-Greenstein phase over an
// exponential height fog. Composites the result into the HDR colour
// (out = scene * transmittance + inscatter) so the rays bloom downstream.
//
// Inputs : gInput0 = HDR scene colour, gInput2 = scene depth (R32F)
// Params0: (density, heightFalloff, anisotropy g, stepCount)
// Params1: (sunIntensity, fogHeight, maxDistance, ambientScatter)
#include "PostCommon.hlsli"

static const float HBE_FOG_PI = 3.14159265f;

float3 WorldFromDepth(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

// Henyey-Greenstein phase function (g > 0 = forward scattering).
// The forward peak scales as 1/(1-g)^2 and EXPLODES as g -> 1 (g=0.76 peaks ~2.4,
// g=0.95 ~62, g=0.99 ~1580). That runaway multiplier amplifies the fog march's
// necessarily-coarse per-step sun-shadow into glaring blocky "cubes" - which is why
// cranking anisotropy makes them appear. Clamp g for numerical safety AND cap the
// peak so a strong forward-scatter look stays bounded and artifact-free.
float PhaseHG(float cosT, float g)
{
    g = clamp(g, -0.9f, 0.9f);
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosT;
    float p = (1.0f - g2) / (4.0f * HBE_FOG_PI * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
    return min(p, 8.0f); // bound the forward peak (~strong but stable)
}

// Interleaved Gradient Noise (Jimenez): evenly-distributed per-pixel offset with
// far better spectral properties than a sine hash (no structured blocks), and
// ANIMATED by the frame index in gPostParams3.x - each frame shifts the pattern
// so TAA integrates the march into smooth fog. params3.x is 0 when TAA is off
// (static IGN; no crawling noise).
float IGN(float2 p)
{
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

// Outputs the fog contribution ONLY, packed as (rgb = inscatter, a = transmittance),
// NOT scene*transmittance+inscatter. Rendered at reduced resolution; ApplyHalfRes
// composites it over the full-res HDR (out = scene * a + rgb) so the sharp scene never
// gets downsampled - only the smooth fog does.
float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;

    const float density = gPostParams0.x;
    const float heightFalloff = gPostParams0.y;
    const float g = gPostParams0.z;
    const int   steps = max(1, (int)gPostParams0.w);
    const float sunInt = gPostParams1.x;
    const float fogHeight = gPostParams1.y;
    const float maxDist = gPostParams1.z;
    const float ambient = gPostParams1.w;
    const float3 fogTint = gPostParams2.rgb; // fog colour (all in-scatter)
    const float godRays = gPostParams2.w;    // boost on the shadowed sun shafts

    const float depth = SamplePost(gInput2, uv).r;
    const float3 worldEnd = WorldFromDepth(uv, min(depth, 0.999999f));
    const float3 ro = gCameraPosWS;
    const float3 toEnd = worldEnd - ro;
    const float endLen = length(toEnd);
    const float dist = (depth >= 1.0f) ? maxDist : min(endLen, maxDist);
    const float3 rd = (endLen > 1e-4f) ? toEnd / endLen : float3(0.0f, 0.0f, 1.0f);

    const float stepLen = dist / steps;
    const float dither = IGN(input.positionCS.xy + 5.588238f * gPostParams3.x);
    const float3 L = normalize(gLightDirWS); // direction TO the sun
    const float sunPhase = PhaseHG(dot(rd, L), g);

    float transmittance = 1.0f;
    float3 inscatter = 0.0f.xxx;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float t = (i + dither) * stepLen;
        const float3 pos = ro + rd * t;

        // Exponential height fog: densest at/below fogHeight, thinning upward.
        const float dens = density * exp(-heightFalloff * max(pos.y - fogHeight, 0.0f));
        if (dens < 1e-6f) continue;
        const float sigma = dens;

        // Sun in-scatter, shadowed by the CSM. The godRays boost emphasizes the
        // shafts cast through gaps (the shadowed sun term) only. 1-tap shadow per
        // step (not 9-tap PCF) - dithered accumulation makes them look the same.
        const float sunVis = ShadowFactorCheap(pos);
        float3 scat = gLightColor * (gLightIntensity * sunInt * sunVis * sunPhase * godRays);
        // Isotropic sky/ambient in-scatter.
        scat += ambient * gLightColor;

        // Punctual in-scatter (windowed inverse-square + HG; unshadowed).
        for (uint li = 0; li < gPunctualCount; ++li)
        {
            PunctualLight pl = gPunctualLights[li];
            const float3 toL = pl.positionWS - pos;
            const float d2 = dot(toL, toL);
            const float r2 = pl.range * pl.range;
            if (d2 >= r2) continue;
            const float dl = sqrt(d2);
            const float3 Lp = toL / max(dl, 1e-4f);
            const float win = saturate(1.0f - (d2 / r2) * (d2 / r2));
            float atten = (win * win) / (d2 + 1.0f);
            if (pl.isSpot != 0u)
            {
                const float cd = dot(-Lp, normalize(pl.directionWS));
                atten *= saturate((cd - pl.outerCos) / max(pl.innerCos - pl.outerCos, 1e-4f));
            }
            scat += pl.color * (pl.intensity * atten * PhaseHG(dot(rd, Lp), g));
        }

        const float stepTrans = exp(-sigma * stepLen);
        inscatter += transmittance * (scat * fogTint) * sigma * stepLen; // fog colour tint
        transmittance *= stepTrans;
        if (transmittance < 0.004f) break; // fully fogged - remaining steps add nothing
    }

    return float4(inscatter, transmittance); // composited over the scene by ApplyHalfRes
}

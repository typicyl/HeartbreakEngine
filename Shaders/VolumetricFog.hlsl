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
float PhaseHG(float cosT, float g)
{
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosT;
    return (1.0f - g2) / (4.0f * HBE_FOG_PI * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
}

float Hash(float2 p) { return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f); }

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 scene = SamplePost(gInput0, uv).rgb;

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
    const float dither = Hash(input.positionCS.xy);
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

    return float4(scene * transmittance + inscatter, 1.0f);
}

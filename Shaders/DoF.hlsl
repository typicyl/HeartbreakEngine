// Shaders/DoF.hlsl - depth of field (single-pass disk bokeh, post-tonemap).
//
// Per pixel we reconstruct world distance from the depth buffer (same method as
// SSAO/TAA, so it's backend-agnostic), derive a circle-of-confusion from the
// focus distance/range, and gather a golden-angle disk of colour samples scaled
// by that radius. Samples that are sharper than the centre are down-weighted so
// in-focus foreground doesn't bleed into the blurred background.
//
// Inputs : gInput0 = resolved colour (tonemapped LDR)
//          gInput2 = scene depth (R32F)
// Params : gPostParams0 = (focusDistance, focusRange, maxBlurTexels, enabled)
#include "PostCommon.hlsli"

float3 WorldFromDepth(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

// Circle of confusion: 0 inside the focus band, ramping to 1 by 2x range away.
float CoC(float2 uv, float focusDist, float focusRange)
{
    const float depth = SamplePost(gInput2, uv).r;
    const float dist = distance(WorldFromDepth(uv, depth), gCameraPosWS);
    return saturate((abs(dist - focusDist) - focusRange) / max(focusRange, 1e-3f));
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 center = SamplePost(gInput0, uv).rgb;
    if (gPostParams0.w < 0.5f) return float4(center, 1.0f); // disabled: copy

    const float focusDist = gPostParams0.x;
    const float focusRange = gPostParams0.y;
    const float maxBlur = gPostParams0.z; // source texels

    const float coc = CoC(uv, focusDist, focusRange);
    const float radius = coc * maxBlur;
    if (radius < 0.75f) return float4(center, 1.0f); // effectively in focus

    float3 sum = center;
    float weight = 1.0f;
    const int kTaps = 24;
    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        const float t = (i + 0.5f) / kTaps;
        const float a = i * 2.39996323f;            // golden angle
        const float2 offs = float2(cos(a), sin(a)) * sqrt(t) * radius * gInTexel;
        const float3 s = SamplePost(gInput0, uv + offs).rgb;
        const float scoc = CoC(uv + offs, focusDist, focusRange);
        // Admit samples at least as blurred as the centre fully; scale down
        // sharper (likely in-focus foreground) ones so they don't smear in.
        const float w = (scoc + 0.05f >= coc) ? 1.0f : scoc;
        sum += s * w;
        weight += w;
    }
    return float4(sum / weight, 1.0f);
}

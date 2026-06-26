// Shaders/FXAA.hlsl - fast approximate anti-aliasing over the LDR image.
//
// Classic compact FXAA 3.11 variant: estimates the local edge direction from
// the diagonal luma gradient and blends two short-axis taps with two
// long-axis taps, falling back to the short blend when the long one escapes
// the local luma range.
//
// Inputs : gInput0 = tonemapped sRGB LDR image
// Params : gPostParams0 = (enabled, unused, unused, unused)
#include "PostCommon.hlsli"

#define FXAA_REDUCE_MIN (1.0f / 128.0f)
#define FXAA_REDUCE_MUL (1.0f / 8.0f)
#define FXAA_SPAN_MAX   8.0f

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float2 t = gInTexel;

    const float3 rgbM = SamplePost(gInput0, uv).rgb;
    if (gPostParams0.x < 0.5f)
        return float4(rgbM, 1.0f); // disabled: plain copy

    const float3 rgbNW = SamplePost(gInput0, uv + t * float2(-1, -1)).rgb;
    const float3 rgbNE = SamplePost(gInput0, uv + t * float2( 1, -1)).rgb;
    const float3 rgbSW = SamplePost(gInput0, uv + t * float2(-1,  1)).rgb;
    const float3 rgbSE = SamplePost(gInput0, uv + t * float2( 1,  1)).rgb;

    const float lumaNW = Luma(rgbNW);
    const float lumaNE = Luma(rgbNE);
    const float lumaSW = Luma(rgbSW);
    const float lumaSE = Luma(rgbSE);
    const float lumaM  = Luma(rgbM);

    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25f * FXAA_REDUCE_MUL,
                                FXAA_REDUCE_MIN);
    const float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -FXAA_SPAN_MAX.xx, FXAA_SPAN_MAX.xx) * t;

    const float3 rgbA = 0.5f * (SamplePost(gInput0, uv + dir * (1.0f / 3.0f - 0.5f)).rgb +
                                SamplePost(gInput0, uv + dir * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 rgbB = rgbA * 0.5f + 0.25f * (SamplePost(gInput0, uv + dir * -0.5f).rgb +
                                               SamplePost(gInput0, uv + dir *  0.5f).rgb);

    const float lumaB = Luma(rgbB);
    const float3 color = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(color, 1.0f);
}

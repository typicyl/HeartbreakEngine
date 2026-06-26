// Shaders/BloomDown.hlsl - bloom downsample (13-tap, Jimenez/CoD-style).
//
// The first chain pass also applies a soft-knee bright-pass filter so only
// over-threshold radiance enters the blur pyramid.
//
// Inputs : gInput0 = source (HDR scene color for the first pass, else the
//                    previous bloom mip)
// Params : gPostParams0 = (threshold, knee, isFirstPass, unused)
#include "PostCommon.hlsli"

// Quadratic soft threshold: full contribution above `threshold`, smooth
// rolloff starting `knee` below it.
float3 Prefilter(float3 c, float threshold, float knee)
{
    const float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-4f);
    const float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4f);
    return c * contribution;
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float2 t = gInTexel;

    // 13 bilinear taps arranged as 4 overlapping 2x2 boxes + corner ring.
    const float3 a = SamplePost(gInput0, uv + t * float2(-2, -2)).rgb;
    const float3 b = SamplePost(gInput0, uv + t * float2( 0, -2)).rgb;
    const float3 c = SamplePost(gInput0, uv + t * float2( 2, -2)).rgb;
    const float3 d = SamplePost(gInput0, uv + t * float2(-2,  0)).rgb;
    const float3 e = SamplePost(gInput0, uv).rgb;
    const float3 f = SamplePost(gInput0, uv + t * float2( 2,  0)).rgb;
    const float3 g = SamplePost(gInput0, uv + t * float2(-2,  2)).rgb;
    const float3 h = SamplePost(gInput0, uv + t * float2( 0,  2)).rgb;
    const float3 i = SamplePost(gInput0, uv + t * float2( 2,  2)).rgb;
    const float3 j = SamplePost(gInput0, uv + t * float2(-1, -1)).rgb;
    const float3 k = SamplePost(gInput0, uv + t * float2( 1, -1)).rgb;
    const float3 l = SamplePost(gInput0, uv + t * float2(-1,  1)).rgb;
    const float3 m = SamplePost(gInput0, uv + t * float2( 1,  1)).rgb;

    float3 color = e * 0.125f
                 + (a + c + g + i) * 0.03125f
                 + (b + d + f + h) * 0.0625f
                 + (j + k + l + m) * 0.125f;

    if (gPostParams0.z > 0.5f)
        color = Prefilter(color, gPostParams0.x, gPostParams0.y);

    return float4(max(color, 0.0f.xxx), 1.0f);
}

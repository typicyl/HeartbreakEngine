// Shaders/BloomUp.hlsl - bloom upsample (9-tap tent filter).
//
// Rendered with additive blending onto the next-larger bloom mip, so the
// pyramid accumulates progressively wider blurs on the way back up.
//
// Inputs : gInput0 = the smaller (more blurred) bloom mip
// Params : gPostParams0 = (scatter scale, unused, unused, unused)
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float2 t = gInTexel;

    float3 color =
        SamplePost(gInput0, uv + t * float2(-1, -1)).rgb * 1.0f +
        SamplePost(gInput0, uv + t * float2( 0, -1)).rgb * 2.0f +
        SamplePost(gInput0, uv + t * float2( 1, -1)).rgb * 1.0f +
        SamplePost(gInput0, uv + t * float2(-1,  0)).rgb * 2.0f +
        SamplePost(gInput0, uv).rgb                      * 4.0f +
        SamplePost(gInput0, uv + t * float2( 1,  0)).rgb * 2.0f +
        SamplePost(gInput0, uv + t * float2(-1,  1)).rgb * 1.0f +
        SamplePost(gInput0, uv + t * float2( 0,  1)).rgb * 2.0f +
        SamplePost(gInput0, uv + t * float2( 1,  1)).rgb * 1.0f;
    color *= (1.0f / 16.0f) * gPostParams0.x;

    return float4(color, 1.0f);
}

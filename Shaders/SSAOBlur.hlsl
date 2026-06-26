// Shaders/SSAOBlur.hlsl - 5x5 box blur over the raw SSAO term.
//
// The SSAO pass trades banding for per-pixel noise; this removes the noise.
// Inputs : gInput0 = raw SSAO
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;

    float sum = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            sum += SamplePost(gInput0, uv + float2(x, y) * gInTexel).r;
        }
    }
    const float ao = sum / 25.0f;
    return float4(ao.xxx, 1.0f);
}

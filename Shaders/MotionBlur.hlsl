// Shaders/MotionBlur.hlsl - camera motion blur from depth reprojection.
//
// Per pixel we read the screen-space velocity from the G-buffer motion vectors
// (curUV - prevUV, written by the mesh pass) and average colour samples along
// it. Because the velocity buffer captures per-object motion, this blurs moving
// and skinned geometry as well as camera movement.
//
// Inputs : gInput0 = resolved colour, gInput3 = velocity buffer (RG)
// Params : gPostParams0 = (intensity, maxRadiusTexels, enabled, unused)
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 color = SamplePost(gInput0, uv).rgb;
    if (gPostParams0.z < 0.5f) return float4(color, 1.0f); // disabled

    // Screen-space velocity, scaled and clamped to a sane max length.
    float2 velocity = SamplePost(gInput3, uv).xy * gPostParams0.x;
    const float2 maxLen = gPostParams0.y * gInTexel;
    velocity = clamp(velocity, -maxLen, maxLen);
    if (dot(velocity, velocity) < 1e-9f) return float4(color, 1.0f);

    // Average symmetric taps along the velocity vector.
    const int kTaps = 12;
    float3 sum = color;
    float weight = 1.0f;
    [unroll]
    for (int i = 1; i < kTaps; ++i)
    {
        const float t = (i / float(kTaps - 1)) - 0.5f; // -0.5 .. 0.5
        sum += SamplePost(gInput0, uv + velocity * t).rgb;
        weight += 1.0f;
    }
    return float4(sum / weight, 1.0f);
}

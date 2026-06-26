// Shaders/TAA.hlsl - temporal anti-aliasing resolve (post-tonemap, LDR).
//
// The scene is rendered each frame with a sub-pixel jitter baked into the
// camera's view-projection (so successive frames sample different sub-pixel
// positions). This pass reprojects the previous frame's resolved color into the
// current frame using the depth buffer + the current/previous view-projections
// (the SAME depth->world reconstruction SSAO uses, so it is backend-agnostic),
// clamps it to the local neighbourhood to suppress ghosting, and blends a small
// amount of the current frame in. Over many frames this accumulates a
// supersampled, temporally stable image.
//
// Inputs : gInput0 = current tonemapped LDR
//          gInput1 = history (previous TAA output)
//          gInput3 = velocity buffer (RG, curUV - prevUV; per-object motion)
// Params : gPostParams0 = (currentWeight, historyValid, unused, unused)
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 cur = SamplePost(gInput0, uv).rgb;

    const float currentWeight = gPostParams0.x; // e.g. 0.1 (10% current / 90% history)
    const float historyValid = gPostParams0.y;

    // Reproject via the per-object motion vector: history texel = uv - velocity.
    // Unlike depth reprojection this tracks moving / skinned geometry, so they
    // accumulate without ghosting.
    const float2 velocity = SamplePost(gInput3, uv).xy;
    const float2 uvPrev = uv - velocity;

    // No usable history (first frame, off-screen reprojection):
    // fall back to the current frame.
    if (historyValid < 0.5f || any(uvPrev < 0.0f) || any(uvPrev > 1.0f))
        return float4(cur, 1.0f);

    float3 history = SamplePost(gInput1, uvPrev).rgb;

    // Neighbourhood clamp: constrain history to the 3x3 colour range of the
    // current frame so moving edges / disocclusions reject stale colour
    // instead of smearing it.
    float3 nMin = cur;
    float3 nMax = cur;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 c = SamplePost(gInput0, uv + float2(x, y) * gInTexel).rgb;
            nMin = min(nMin, c);
            nMax = max(nMax, c);
        }
    }
    history = clamp(history, nMin, nMax);

    return float4(lerp(history, cur, currentWeight), 1.0f);
}

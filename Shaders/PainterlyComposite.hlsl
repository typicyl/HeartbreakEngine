// Shaders/PainterlyComposite.hlsl - restore dynamic-layer objects over the painterly.
//
// The painterly pass (Kuwahara + brush strokes) stylises the WHOLE screen. Dynamic-
// layer objects (player / NPCs / interactables) should read crisp against that painted
// static world, so this pass lerps per-pixel back to the un-painted lit colour using
// the painterly MASK the forward pass wrote into the HDR alpha (1 = dynamic/exempt,
// 0 = static/painted). Dynamic objects keep their full lighting/shadows/GI/fog (that's
// gInput1, the pre-painterly lit HDR) - only the painted stylisation is skipped. The
// mask comes from the untouched forward HDR (gInput2.a), so the static/dynamic edge is
// pixel-crisp regardless of the half-res Kuwahara.
//
// Inputs : gInput0 = painterly result (rgb, Kuwahara + brush strokes), gInput1 =
//          pre-painterly lit HDR (rgb), gInput2 = forward HDR (a = painterly mask),
//          gInput3 = scene depth (R32F)
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 paint = SamplePost(gInput0, uv).rgb; // painted (Kuwahara + strokes)
    const float3 lit   = SamplePost(gInput1, uv).rgb; // crisp lit colour (dynamic objects)
    const float2 mb = DecodeMaskBits(SamplePost(gInput2, uv).a); // .x=exempt .y=censored
    float        mask = mb.x;                          // 1 = restore crisp, 0 = keep paint

    // Painterly censor: only the CENSORED object's own pixels are affected by the
    // sphere, so the painted look stays ON the object (not the floor/walls inside the
    // sphere). For a censored DYNAMIC object this suppresses the crisp restore within
    // the feathered sphere; a censored STATIC object is already painted (mask 0).
    if (mb.y > 0.5f && gCensorCount.x > 0u)
    {
        const float depth = SamplePost(gInput3, uv).r; // 1 = sky
        if (depth < 1.0f)
        {
            const float3 wp = WorldFromDepthPost(uv, depth);
            mask *= 1.0f - saturate(CensorWeightWS(wp));
        }
    }
    return float4(lerp(paint, lit, saturate(mask)), 1.0f);
}

// Shaders/Exposure.hlsl - average scene luminance + temporal eye adaptation.
//
// Renders to a 1x1 target. The pixel samples a grid across the HDR scene,
// averages log-luminance (geometric mean, the standard for exposure), then
// eases the previous frame's adapted value toward it so exposure changes smooth
// over time instead of popping. The tonemap reads this to derive auto-exposure.
//
// Inputs : gInput0 = HDR scene colour, gInput1 = previous adapted luminance (1x1)
// Params : gPostParams0 = (adaptSpeed, deltaTime, historyValid, unused)
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const int N = 16; // 16x16 = 256 samples across the frame
    float sumLog = 0.0f;
    [unroll]
    for (int y = 0; y < N; ++y)
    {
        [unroll]
        for (int x = 0; x < N; ++x)
        {
            const float2 uv = (float2(x, y) + 0.5f) / N;
            const float3 c = SamplePost(gInput0, uv).rgb;
            sumLog += log(max(dot(c, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f));
        }
    }
    const float avgLum = exp(sumLog / (N * N));

    // Only read the previous adapted value when it exists (avoids sampling an
    // uninitialised history target on the first frame / after a resize).
    float adapted = avgLum;
    if (gPostParams0.z >= 0.5f)
    {
        const float prev = SamplePost(gInput1, float2(0.5f, 0.5f)).r;
        const float speed = gPostParams0.x;
        const float dt = gPostParams0.y;
        adapted = lerp(prev, avgLum, saturate(1.0f - exp(-dt * speed)));
    }
    return float4(adapted.xxx, 1.0f);
}

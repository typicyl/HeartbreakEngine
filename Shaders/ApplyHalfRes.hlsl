// Shaders/ApplyHalfRes.hlsl - composite a reduced-resolution effect buffer back over
// the full-res HDR scene. SSGI and volumetric fog now render into a half/quarter-res
// target packed as (rgb = additive term, a = transmittance); this pass resolves it at
// full res through the bilinear sampler and applies:  out = scene * a + rgb.
// That is scene + GI for SSGI (which writes a = 1) and scene * transmittance + inscatter
// for fog - so one pass composites either effect. Computing the expensive ray march at
// reduced res + upscaling here is ~4-16x cheaper and near-lossless for these
// low-frequency effects (TAA denoises the GI gather downstream).
// Inputs: gInput0 = full-res HDR scene, gInput1 = reduced-res effect (rgb + a).
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 scene = SamplePost(gInput0, uv).rgb;

    // gPostParams0.x = blur radius in INPUT (low-res) texels. 0 => a single crisp
    // bilinear tap (SSGI keeps its detail). > 0 => a 5x5 BILATERAL low-pass of the
    // low-res buffer: volumetric fog's per-march sun-shadow is blocky at WORLD scale
    // (grazing-sun shadow-map aliasing) and reads as cubes; bilinear upscaling can't
    // remove structure coarser than a texel, so we low-pass it here. The blur is
    // DEPTH-WEIGHTED (gInput2 = depth) so it smooths within a surface but never bleeds
    // fog across silhouettes - a plain Gaussian bled bright sky-inscatter onto the hill
    // ridges (a rim halo the painterly pass then faithfully drew).
    const float blur = gPostParams0.x;
    float4 fx;
    if (blur > 0.0f)
    {
        const float2 texel = gInTexel * blur;
        const float dc = SamplePost(gInput2, uv).r; // centre depth (1 = sky/far)
        float4 acc = 0.0f;
        float wsum = 0.0f;
        [unroll] for (int y = -2; y <= 2; ++y)
        {
            [unroll] for (int x = -2; x <= 2; ++x)
            {
                const float2 o = float2(x, y);
                const float2 suv = uv + o * texel;
                const float sw = exp(-0.5f * dot(o, o) / 1.6f);       // spatial (sigma ~1.26)
                const float dt = SamplePost(gInput2, suv).r;
                const float ew = exp(-500.0f * abs(dt - dc));         // depth edge-stop
                const float w = sw * ew;
                acc += SamplePost(gInput1, suv) * w;
                wsum += w;
            }
        }
        fx = acc / max(wsum, 1e-4f); // centre tap (w=1) guarantees wsum > 0
    }
    else
    {
        fx = SamplePost(gInput1, uv); // bilinear-upscaled from the low-res buffer
    }
    return float4(scene * fx.a + fx.rgb, 1.0f);
}

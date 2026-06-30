// Shaders/Copy.hlsl - passthrough / bilinear upscale of a single input.
//
// Samples gInput0 at the output UV through the linear clamp sampler, so reading a
// half-resolution buffer at full-res UVs bilinear-upscales it. Used to bring the
// half-res Kuwahara underpainting back to full res before the brush strokes and the
// rest of the post chain composite over it (the Kuwahara is a smoothing filter, so
// computing it at half res + upscaling here is near-lossless but ~4x cheaper).
#include "PostCommon.hlsli"

float4 PSMain(FSOutput input) : SV_Target
{
    return SamplePost(gInput0, input.positionCS.xy * gOutTexel);
}

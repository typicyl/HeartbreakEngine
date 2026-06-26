// Shaders/SSAO.hlsl - Ground-Truth Ambient Occlusion (GTAO, Jimenez et al. 2016).
//
// Horizon-based AO that uses the G-buffer SHADING normal (instead of a noisy
// depth-derivative normal) and integrates the exact cosine-weighted visibility
// per slice, then re-adds bounced energy with a multi-bounce approximation.
// Replaces the old 12-tap hemisphere SSAO; reuses the same half-res target,
// blur pass, settings and bindless slot, so the rest of the stack is unchanged.
//
// Inputs : gInput0 = scene depth (R32F), gInput1 = G-buffer (octN.rg/rough/metal)
// Params : gPostParams0 = (radius, intensity, depthBias, unused)
// Output : visibility (1 = open, 0 = occluded), typically half-res.
#include "PostCommon.hlsli"

#define HBE_GTAO_SLICES 3
#define HBE_GTAO_STEPS  6
static const float HBE_PI     = 3.14159265f;
static const float HBE_HALFPI = 1.57079633f;

float3 WorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}
float Hash(float2 p) { return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f); }

// Jimenez 2016 multi-bounce: re-adds light bounced back into cavities so AO does
// not darken unrealistically (cubic fit for a mid-grey ~0.3 albedo).
float MultiBounce(float visibility)
{
    const float a = 0.27852f, b = -0.79683f, c = 1.51686f;
    return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float depth = SamplePost(gInput0, uv).r;
    if (depth >= 1.0f) return 1.0f.xxxx; // sky: fully open

    const float radius = gPostParams0.x;
    const float intensity = gPostParams0.y;

    const float3 P = WorldPos(uv, depth);
    const float3 N = OctDecode(SamplePost(gInput1, uv).rg);
    const float3 V = normalize(gCameraPosWS - P);

    // World-space screen tangents (a texel right / down) so a 2D screen-space
    // slice direction maps to a world-space marching direction.
    const float2 uvX = uv + float2(gInTexel.x, 0.0f);
    const float2 uvY = uv + float2(0.0f, gInTexel.y);
    const float3 ddx = WorldPos(uvX, SamplePost(gInput0, uvX).r) - P;
    const float3 ddy = WorldPos(uvY, SamplePost(gInput0, uvY).r) - P;

    // March length in texels covering the world-space radius.
    const float worldPerTexel = max(length(ddx), 1e-5f);
    const float ssRadius = clamp(radius / worldPerTexel, 2.0f, 96.0f);
    const float rnd = Hash(input.positionCS.xy);

    float visibility = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (int s = 0; s < HBE_GTAO_SLICES; ++s)
    {
        const float phi = (s + rnd) * (HBE_PI / HBE_GTAO_SLICES);
        const float2 dir = float2(cos(phi), sin(phi));
        const float3 sliceDir = normalize(dir.x * ddx + dir.y * ddy);

        // Slice plane = span(V, sliceDir); project the normal into it.
        const float3 planeN = normalize(cross(sliceDir, V));
        const float3 projN = N - planeN * dot(N, planeN);
        const float projLen = length(projN);
        if (projLen < 1e-4f) continue;
        const float3 projNn = projN / projLen;

        // Signed angle of the projected normal from V within the slice.
        float n = acos(clamp(dot(projNn, V), -1.0f, 1.0f));
        n *= sign(dot(projNn, sliceDir));

        // Horizon search: max elevation (cos to V) on each side, with a smooth
        // distance falloff so far occluders across depth gaps fade out.
        float cosH1 = -1.0f, cosH2 = -1.0f;
        [unroll]
        for (int k = 1; k <= HBE_GTAO_STEPS; ++k)
        {
            const float t = (k - 0.5f * rnd) / HBE_GTAO_STEPS;
            const float2 off = dir * (ssRadius * t) * gInTexel;
            {
                const float2 su = uv + off;
                const float sd = SamplePost(gInput0, su).r;
                if (sd < 1.0f) {
                    const float3 D = WorldPos(su, sd) - P;
                    const float len = length(D);
                    const float fall = saturate(1.0f - len / radius);
                    cosH1 = max(cosH1, lerp(-1.0f, dot(D / max(len, 1e-5f), V), fall));
                }
            }
            {
                const float2 su = uv - off;
                const float sd = SamplePost(gInput0, su).r;
                if (sd < 1.0f) {
                    const float3 D = WorldPos(su, sd) - P;
                    const float len = length(D);
                    const float fall = saturate(1.0f - len / radius);
                    cosH2 = max(cosH2, lerp(-1.0f, dot(D / max(len, 1e-5f), V), fall));
                }
            }
        }

        // Horizon angles relative to V (+side positive, -side negative), each
        // clamped to the hemisphere around the projected normal.
        float h1 =  acos(clamp(cosH1, -1.0f, 1.0f));
        float h2 = -acos(clamp(cosH2, -1.0f, 1.0f));
        h1 = n + min(h1 - n,  HBE_HALFPI);
        h2 = n + max(h2 - n, -HBE_HALFPI);

        const float sinN = sin(n);
        const float cosN = cos(n);
        const float arc =
            0.25f * (-cos(2.0f * h1 - n) + cosN + 2.0f * h1 * sinN) +
            0.25f * (-cos(2.0f * h2 - n) + cosN + 2.0f * h2 * sinN);

        visibility += projLen * arc;
        weightSum  += projLen;
    }

    float vis = (weightSum > 1e-4f) ? saturate(visibility / weightSum) : 1.0f;
    vis = MultiBounce(vis);
    const float ao = lerp(1.0f, vis, saturate(intensity));
    return float4(ao.xxx, 1.0f);
}

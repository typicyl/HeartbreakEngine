// Shaders/SSGI.hlsl - screen-space global illumination (one diffuse bounce).
//
// Gathers indirect diffuse from on-screen lit surfaces: cosine-weighted rays
// over the hemisphere around the G-buffer normal, ray-marched against the depth
// buffer; each hit contributes the lit HDR colour of that surface (weighted by
// how much it faces the receiver). Composited additively into the HDR -> colour
// bleeding from nearby geometry. Screen-space, so off-screen light is missed and
// the effect fades toward the screen edges; TAA downstream denoises the gather.
//
// Inputs : gInput0 = HDR colour, gInput1 = G-buffer (octN.rg/rough.b/metal.a),
//          gInput2 = depth (R32F)
// Params0: (intensity, radius, sampleCount, enabled)
#include "PostCommon.hlsli"

static const float HBE_SSGI_GOLDEN = 2.39996323f; // golden angle (radians)

float3 WorldFromDepth(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

float2 ProjectUV(float3 worldPos, out float clipW)
{
    float4 c = mul(gViewProj, float4(worldPos, 1.0f));
    clipW = c.w;
    return float2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f);
}

float Hash(float2 p) { return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f); }

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 hdr = SamplePost(gInput0, uv).rgb;
    if (gPostParams0.w < 0.5f) return float4(hdr, 1.0f); // disabled

    const float depth = SamplePost(gInput2, uv).r;
    if (depth >= 1.0f) return float4(hdr, 1.0f); // sky receives no GI

    const float intensity = gPostParams0.x;
    const float radius = gPostParams0.y;
    const int   K = max(1, (int)gPostParams0.z);

    const float4 g = SamplePost(gInput1, uv);
    const float3 N = OctDecode(g.rg);
    const float  metal = g.a;
    const float3 P = WorldFromDepth(uv, depth);

    const float3 T = normalize(abs(N.y) < 0.99f ? cross(N, float3(0, 1, 0))
                                                : cross(N, float3(1, 0, 0)));
    const float3 Bt = cross(N, T);
    const float rnd = Hash(input.positionCS.xy);
    const float rndAngle = rnd * 6.2831853f;
    const int STEPS = 12;
    const float3 bias = N * 0.02f; // lift the ray off the surface

    float3 indirect = 0.0f.xxx;
    [loop]
    for (int i = 0; i < K; ++i)
    {
        // Cosine-weighted hemisphere direction (importance sampling: the cosine
        // term is folded into the sample distribution, so rays are equal weight).
        const float u1 = (i + rnd) / K;
        const float phi = rndAngle + i * HBE_SSGI_GOLDEN;
        const float r = sqrt(u1);
        const float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(1.0f - u1, 0.0f)));
        const float3 dir = normalize(T * local.x + Bt * local.y + N * local.z);

        const float stepLen = radius / STEPS;
        bool hit = false;
        float2 hitUV = uv;
        [loop]
        for (int s = 1; s <= STEPS; ++s)
        {
            const float3 sp = P + dir * (stepLen * s) + bias;
            float clipW;
            const float2 suv = ProjectUV(sp, clipW);
            if (clipW <= 0.0f || any(suv < 0.0f) || any(suv > 1.0f)) break;
            const float sd = SamplePost(gInput2, suv).r;
            if (sd >= 1.0f) continue; // sky along the ray, keep marching
            const float3 surf = WorldFromDepth(suv, sd);
            const float diff = distance(sp, gCameraPosWS) - distance(surf, gCameraPosWS);
            if (diff > 0.0f && diff < stepLen * 2.0f) { hitUV = suv; hit = true; break; }
        }
        if (hit)
        {
            const float3 hitN = OctDecode(SamplePost(gInput1, hitUV).rg);
            const float3 toHit = normalize(WorldFromDepth(hitUV, SamplePost(gInput2, hitUV).r) - P);
            // Only gather radiance from surfaces facing back toward the receiver.
            const float facing = saturate(dot(hitN, -toHit));
            indirect += SamplePost(gInput0, hitUV).rgb * facing;
        }
    }
    indirect /= K;

    // Add one diffuse bounce; metals take no diffuse GI.
    return float4(hdr + indirect * (intensity * (1.0f - metal)), 1.0f);
}

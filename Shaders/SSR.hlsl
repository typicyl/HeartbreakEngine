// Shaders/SSR.hlsl - screen-space reflections (HDR, pre-tonemap).
//
// Position is reconstructed from the depth buffer; the surface normal and
// roughness come from the thin G-buffer (gInput1), so reflections follow the
// per-pixel shading normal and fade out on rough/diffuse materials (glossy,
// per-material reflectivity instead of a uniform mirror). We march the
// reflection ray in world space, project each step to screen, and test it
// against the depth buffer; on a hit we add the reflected scene colour,
// weighted by Fresnel, roughness and a screen-edge fade.
//
// Inputs : gInput0 = HDR scene colour, gInput1 = G-buffer (octN.rg/rough.b/metal.a),
//          gInput2 = scene depth (R32F)
// Params : gPostParams0 = (intensity, maxDistance, enabled, unused)
#include "PostCommon.hlsli"

float3 WorldFromDepth(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

float2 ProjectToUV(float3 worldPos, out float w)
{
    const float4 clip = mul(gViewProj, float4(worldPos, 1.0f));
    w = clip.w;
    return float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 hdr = SamplePost(gInput0, uv).rgb;
    if (gPostParams0.z < 0.5f) return float4(hdr, 1.0f); // disabled

    const float depth = SamplePost(gInput2, uv).r;
    if (depth >= 1.0f) return float4(hdr, 1.0f); // sky: nothing to reflect from

    const float3 P = WorldFromDepth(uv, depth);
    // Shading normal + roughness straight from the G-buffer (accurate per-pixel,
    // includes normal-mapped detail) instead of a depth-derivative estimate.
    const float4 g = SamplePost(gInput1, uv);
    const float3 N = OctDecode(g.rg);
    const float roughness = g.b;
    // Very rough/diffuse surfaces don't produce a coherent screen reflection.
    if (roughness >= 0.85f) return float4(hdr, 1.0f);
    const float3 V = normalize(P - gCameraPosWS);
    const float3 R = reflect(V, N);
    if (dot(R, N) <= 0.0f) return float4(hdr, 1.0f);

    const float maxDist = gPostParams0.y;
    const int kSteps = 32;
    const float stepLen = maxDist / kSteps;

    float3 prev = P;
    bool hit = false;
    float2 hitUV = uv;
    [loop]
    for (int i = 1; i <= kSteps; ++i)
    {
        const float3 sp = P + R * (stepLen * i);
        float w;
        const float2 suv = ProjectToUV(sp, w);
        if (w <= 0.0f || any(suv < 0.0f) || any(suv > 1.0f)) break;

        const float3 surf = WorldFromDepth(suv, SamplePost(gInput2, suv).r);
        const float diff = distance(sp, gCameraPosWS) - distance(surf, gCameraPosWS);
        if (diff > 0.0f && diff < stepLen * 2.0f)
        {
            // Binary refine between the last miss and this hit for a tight UV.
            float3 a = prev, b = sp;
            [unroll]
            for (int j = 0; j < 5; ++j)
            {
                const float3 mid = (a + b) * 0.5f;
                float mw;
                const float2 muv = ProjectToUV(mid, mw);
                const float3 ms = WorldFromDepth(muv, SamplePost(gInput2, muv).r);
                if (distance(mid, gCameraPosWS) > distance(ms, gCameraPosWS)) b = mid;
                else a = mid;
            }
            float bw;
            hitUV = ProjectToUV(b, bw);
            hit = true;
            break;
        }
        prev = sp;
    }
    if (!hit) return float4(hdr, 1.0f);

    const float3 reflColor = SamplePost(gInput0, hitUV).rgb;
    // Fade near screen edges and reflect more at grazing angles (Schlick).
    const float2 e = abs(hitUV * 2.0f - 1.0f);
    const float edgeFade = saturate(1.0f - pow(max(e.x, e.y), 4.0f));
    const float fresnel = 0.04f + 0.96f * pow(1.0f - saturate(dot(N, -V)), 5.0f);
    // Glossy falloff: smooth/metallic surfaces reflect sharply, rough ones fade.
    const float gloss = saturate(1.0f - roughness / 0.85f);
    return float4(hdr + reflColor * (fresnel * gloss * gPostParams0.x * edgeFade), 1.0f);
}

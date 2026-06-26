// Shaders/Tonemap.hlsl - HDR resolve: AO + bloom + exposure + ACES + grade, with
// an optional painterly (oil) finish: an ANISOTROPIC Kuwahara filter that gathers
// along the local image structure, so detail collapses into confident brush
// strokes that FLOW ALONG forms (not screen-space streaks). Edges where values
// are similar bleed together; high-contrast edges stay crisp. A warm-light /
// cool-shadow grade finishes it. The smear removes thin lines / clean machine
// edges while keeping the image readable.
//
// Inputs : gInput0 = HDR scene color (RGBA16F)
//          gInput1 = bloom mip 0 (RGBA16F; ignored when bloom intensity = 0)
//          gInput2 = blurred SSAO (white texture when SSAO is off)
//          gInput3 = adapted luminance (1x1; 0 = auto-exposure off)
// Params : gPostParams0 = (bloom intensity, AO strength, vignette, saturation)
//          gPostParams1 = (contrast, autoExposureKey, autoExpMin, autoExpMax)
//          gPostParams2 = (stroke size [0=off], warm/cool, stroke flow, -)
// Output : display-ready sRGB-encoded LDR.
#include "PostCommon.hlsli"

// Reinhard-normalised luma in [0,1) so variance behaves across the HDR range.
float NLuma(float3 c) { const float l = Luma(c); return l / (1.0f + l); }

// Anisotropic generalized Kuwahara on the HDR colour. The local structure tensor
// (Sobel on luma) gives the edge orientation + anisotropy; the sampling ellipse
// is elongated ALONG the edge so strokes follow form. The ellipse is split into 8
// sectors; the lowest-variance sectors dominate -> flat painterly masses with
// directional strokes. `radius` = stroke size, `flow` = how elongated (0..1).
float3 AnisoKuwahara(uint tex, float2 uv, float radius, float flow)
{
    const float2 d = gInTexel;

    // --- structure tensor from a 3x3 Sobel on luma --------------------------
    float l[9];
    int idx = 0;
    [unroll] for (int sy = -1; sy <= 1; ++sy)
        [unroll] for (int sx = -1; sx <= 1; ++sx)
            l[idx++] = Luma(SamplePost(tex, uv + float2(sx, sy) * d).rgb);
    const float gx = (l[2] + 2.0f * l[5] + l[8]) - (l[0] + 2.0f * l[3] + l[6]);
    const float gy = (l[6] + 2.0f * l[7] + l[8]) - (l[0] + 2.0f * l[1] + l[2]);
    const float Sxx = gx * gx, Syy = gy * gy, Sxy = gx * gy;
    const float trace = Sxx + Syy;
    const float disc = sqrt(max((Sxx - Syy) * (Sxx - Syy) + 4.0f * Sxy * Sxy, 0.0f));
    const float lambda1 = 0.5f * (trace + disc);
    const float lambda2 = 0.5f * (trace - disc);
    const float anis = trace > 1e-5f ? (lambda1 - lambda2) / trace : 0.0f;

    // Gradient direction (major eigenvector); the edge runs perpendicular to it.
    const float phi = 0.5f * atan2(2.0f * Sxy, Sxx - Syy);
    const float cphi = cos(phi), sphi = sin(phi);

    // Ellipse: short across the gradient, long along the edge (scaled by flow).
    // The kernel is CAPPED (rAlong <= 6 -> <= 13x13 taps) so the cost stays bounded
    // even at max stroke size + flow (it's an O(R^2) per-pixel gather).
    const float a = max(anis, 0.0f) * saturate(flow);
    const float rAlong = min(radius * (1.0f + a * 1.5f), 6.0f);      // along the edge (capped)
    const float rAcross = clamp(radius * (1.0f - a * 0.45f), 1.5f, rAlong); // across the gradient
    const int Ri = (int)ceil(rAlong);

    // --- 8-sector accumulation over the oriented ellipse --------------------
    float3 cSum[8];
    float nSum[8], n2Sum[8], wSum[8];
    [unroll] for (int s = 0; s < 8; ++s) { cSum[s] = 0.0f; nSum[s] = 0.0f; n2Sum[s] = 0.0f; wSum[s] = 0.0f; }

    const float kSector = 8.0f / 6.2831853f;
    [loop] for (int y = -Ri; y <= Ri; ++y)
    {
        [loop] for (int x = -Ri; x <= Ri; ++x)
        {
            const float2 o = float2(x, y);
            // Into the edge frame: ex = along gradient, ey = along edge.
            const float ex = (o.x * cphi + o.y * sphi) / rAcross;
            const float ey = (-o.x * sphi + o.y * cphi) / rAlong;
            const float r2 = ex * ex + ey * ey;
            if (r2 > 1.0f) continue; // outside the ellipse

            const float3 c = SamplePost(tex, uv + o * d).rgb;
            const float nl = NLuma(c);
            const float w = exp(-2.5f * r2); // soft radial falloff
            int sec = (int)floor((atan2(o.y, o.x) + 3.14159265f) * kSector);
            sec = clamp(sec, 0, 7);
            cSum[sec] += c * w;
            nSum[sec] += nl * w;
            n2Sum[sec] += nl * nl * w;
            wSum[sec] += w;
        }
    }

    // Blend sectors weighted toward the most uniform (lowest variance) ones.
    float3 outC = 0.0f;
    float outW = 0.0f;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        if (wSum[k] < 1e-4f) continue;
        const float mean = nSum[k] / wSum[k];
        const float var = max(n2Sum[k] / wSum[k] - mean * mean, 0.0f);
        const float w = 1.0f / (1.0f + pow(var * 64.0f, 4.0f));
        outC += (cSum[k] / wSum[k]) * w;
        outW += w;
    }
    return outW > 1e-5f ? outC / outW : SamplePost(tex, uv).rgb;
}

// Warm lights, cool shadows: shadows shift toward a desaturated cool blue-purple,
// highlights toward warm, halftones stay the most neutral. `s` = strength.
float3 WarmCoolGrade(float3 c, float s)
{
    const float l = Luma(c);
    const float shadowW = saturate(1.0f - l * 2.0f);
    const float lightW = saturate(l * 2.0f - 1.0f);
    const float3 cool = float3(0.86f, 0.93f, 1.10f);
    const float3 warm = float3(1.10f, 1.00f, 0.86f);
    float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), cool, shadowW * s);
    tint = lerp(tint, warm, lightW * s);
    c *= tint;
    const float midW = (1.0f - shadowW) * (1.0f - lightW);
    c = lerp(c, Luma(c).xxx, midW * s * 0.35f);
    return c;
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float strokeSize = gPostParams2.x;
    const bool painterly = strokeSize > 0.5f;

    // The painterly smear is a BLEND over the original by `strength`, so it can be
    // a light finish over hand-painted detail rather than abstracting everything.
    float3 hdr = SamplePost(gInput0, uv).rgb;
    if (painterly)
    {
        const float3 k = AnisoKuwahara(gInput0, uv, strokeSize, gPostParams2.z);
        hdr = lerp(hdr, k, saturate(gPostParams2.w));
    }

    // Screen-space AO darkens indirect light; applied to the full signal here
    // (cheap post-multiply, standard for forward pipelines without a G-buffer).
    const float ao = SamplePost(gInput2, uv).r;
    hdr *= lerp(1.0f, ao, gPostParams0.y);

    // Energy-conserving-ish bloom mix.
    const float3 bloom = SamplePost(gInput1, uv).rgb;
    hdr = lerp(hdr, bloom, saturate(gPostParams0.x));

    // Exposure: manual (gExposure) times auto-exposure when an adapted-luminance
    // map is bound (gInput3 != 0). autoExp = key / adaptedLuminance, clamped.
    float exposure = gExposure;
    if (gInput3 != 0)
    {
        const float adapted = SamplePost(gInput3, float2(0.5f, 0.5f)).r;
        exposure *= clamp(gPostParams1.y / max(adapted, 1e-4f), gPostParams1.z, gPostParams1.w);
    }
    float3 color = TonemapACES(hdr * exposure);

    // Light grade: saturation and contrast around mid-gray.
    const float luma = Luma(color);
    color = lerp(luma.xxx, color, gPostParams0.w);
    color = saturate((color - 0.18f) * gPostParams1.x + 0.18f);

    // Painterly finish: warm/cool grade; photographic vignette is skipped.
    if (painterly)
    {
        color = saturate(WarmCoolGrade(color, gPostParams2.y));
    }
    else
    {
        const float2 v = uv * 2.0f - 1.0f;
        color *= 1.0f - gPostParams0.z * dot(v, v) * 0.5f;
    }

    color = LinearToSRGB(color);
    return float4(color, 1.0f);
}

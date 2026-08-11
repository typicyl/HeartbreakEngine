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

// --- Cinematic color grade -------------------------------------------------
// White balance as a simple per-channel gain (von-Kries-ish): temp warms (R up /
// B down), tint pushes green<->magenta (G). Applied in LINEAR before the tone curve
// so it reads like a camera white-balance, not a paint-over.
float3 WhiteBalance(float3 c, float temp, float tint)
{
    const float3 g = float3(1.0f + 0.25f * temp, 1.0f - 0.20f * tint, 1.0f - 0.25f * temp);
    return c * g;
}

// Lift / Gamma / Gain - the ASC-CDL shadows / midtones / highlights wheels, applied
// in DISPLAY space after tone-mapping. Neutral (lift 0, gamma 1, gain 1) = identity.
float3 LiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain)
{
    c = c * gain;                       // highlights (multiply)
    c = c + lift * (1.0f - c);          // shadows (lift the darks)
    c = pow(max(c, 0.0f), 1.0f / max(gamma, 1e-3f)); // midtones (power)
    return c;
}

// Animated film grain: hashed per-pixel luminance noise, scrolled by time so it
// shimmers like real grain instead of a fixed dither. `amt` ~0.02-0.05 is filmic.
float3 FilmGrain(float3 c, float2 uv, float amt, float time)
{
    const float2 p = uv * float2(1920.0f, 1080.0f) + frac(time) * 137.0f;
    const float n = frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f) - 0.5f;
    // A touch stronger in the mids/shadows than the highlights (where grain reads worst).
    const float w = 1.0f - Luma(c) * 0.7f;
    return c + n * amt * w;
}

// --- Selectable tone-mapping operators (SDR) -------------------------------
// The output stage is modular: pick the operator with gPostParams3.x (set from
// PostSettings::tonemapOperator). Each operator maps exposed HDR-linear colour to
// DISPLAY-LINEAR [0,1]; the shared grade + LinearToSRGB below then encode to the
// 8-bit sRGB back buffer, so adding an operator is a localized change here.
// True HDR-display output (HDR10/PQ, scRGB) is a deliberately separate future
// phase - these are all SDR display transforms.
#define TM_ACES 0u
#define TM_AGX  1u
#define TM_TONY 2u

// AgX (Troy Sobotka) - minimal RGB approximation (Benjamin Wrensch). The matrix
// literals are copied verbatim from the reference GLSL (column-major there); using
// mul(vector, matrix) here reproduces GLSL's `mat * vec` without transposing.
float3 AgxContrastApprox(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x +
           0.4298f * x2 + 0.1191f * x - 0.00232f;
}
float3 TonemapAgX(float3 val)
{
    const float3x3 agxMat = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 agxMatInv = float3x3(
        1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393f;
    const float maxEv = 4.026069f;

    val = mul(val, agxMat);
    val = clamp(log2(max(val, 1e-10f)), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    val = AgxContrastApprox(val);   // sigmoid -> display-encoded [0,1]
    val = mul(val, agxMatInv);      // outset
    // EOTF back to display-LINEAR so the shared LinearToSRGB re-encodes it (rather
    // than baking a second gamma). Net back-buffer value == canonical AgX output.
    val = pow(max(val, 0.0f), 2.2f);
    return saturate(val);
}

// Tony McMapface (Tomasz Stachowiak) ships as a 48^3 display-transform LUT; this
// is an ANALYTIC reconstruction of its character - neutral, hue-preserving, with
// highlights desaturating gracefully toward white instead of clipping to a
// saturated primary. A real LUT can be dropped in later (the operator select makes
// that a localized change).
float3 TonemapTony(float3 c)
{
    c = max(c, 0.0f);
    // Per-channel extended Reinhard with a white point: filmic compression that
    // preserves hue better than a luminance-only curve (the "notorious six").
    const float white = 4.0f;
    const float3 toned = (c * (1.0f + c / (white * white))) / (1.0f + c);
    // Bleach highlights toward their own luminance as the scene gets bright.
    const float lum = dot(toned, float3(0.2126f, 0.7152f, 0.0722f));
    const float peak = max(max(c.r, c.g), c.b);
    const float bleach = smoothstep(1.0f, 6.0f, peak) * 0.7f;
    return saturate(lerp(toned, lum.xxx, bleach));
}

float3 ApplyTonemap(uint op, float3 x)
{
    if (op == TM_AGX) return TonemapAgX(x);
    if (op == TM_TONY) return TonemapTony(x);
    return TonemapACES(x); // TM_ACES (default / unknown)
}

float4 PSMain(FSOutput input) : SV_Target
{
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float strokeSize = gPostParams2.x;
    const bool painterly = strokeSize > 0.5f;

    // Chromatic aberration: split the RGB fetch radially toward the frame edges
    // (a lens effect, so it scales with distance from centre). Only pays the extra
    // taps when dialed in.
    const bool gradeOn = gGrade1.w > 0.5f;
    float3 hdr;
    if (gradeOn && gGrade0.w > 0.0001f)
    {
        const float2 caOff = (uv - 0.5f) * gGrade0.w * 0.02f;
        hdr.r = SamplePost(gInput0, uv + caOff).r;
        hdr.g = SamplePost(gInput0, uv).g;
        hdr.b = SamplePost(gInput0, uv - caOff).b;
    }
    else
    {
        hdr = SamplePost(gInput0, uv).rgb;
    }
    // The painterly smear is a BLEND over the original by `strength`, so it can be
    // a light finish over hand-painted detail rather than abstracting everything.
    if (painterly)
    {
        const float3 k = AnisoKuwahara(gInput0, uv, strokeSize, gPostParams2.z);
        hdr = lerp(hdr, k, saturate(gPostParams2.w));
    }

    // White balance (temperature / tint) in LINEAR, before the tone curve. Applied
    // AFTER the painterly blend on purpose: AnisoKuwahara re-samples the raw gInput0,
    // so white-balancing earlier would be cancelled at high stroke strength.
    if (gradeOn)
        hdr = WhiteBalance(hdr, gGrade0.x, gGrade0.y);

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
    const uint tmOp = (uint)(gPostParams3.x + 0.5f);
    float3 color = ApplyTonemap(tmOp, hdr * exposure);

    // Light grade: saturation and contrast around mid-gray.
    const float luma = Luma(color);
    color = lerp(luma.xxx, color, gPostParams0.w);
    color = saturate((color - 0.18f) * gPostParams1.x + 0.18f);

    // Cinematic grade: lift/gamma/gain colour wheels + film grain (display space).
    if (gradeOn)
    {
        color = LiftGammaGain(color, gGrade1.rgb, gGrade2.rgb, gGrade3.rgb);
        if (gGrade0.z > 0.0001f)
            color = FilmGrain(color, uv, gGrade0.z, gGrade2.w);
        color = saturate(color);
    }

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

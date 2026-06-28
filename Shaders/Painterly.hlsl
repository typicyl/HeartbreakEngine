// Shaders/Painterly.hlsl - scene-driven painterly (oil-on-canvas) pass.
//
// Repaints the fully-lit HDR scene as BOLD directional brush strokes, using the
// G-buffer so it is NOT a flat 2D filter:
//   * A smoothed structure tensor of the lit image -> a coherent flow field:
//     per-pixel stroke ORIENTATION + how elongated (anisotropy). Strokes follow
//     the forms instead of pointing every which way.
//   * Anisotropic Kuwahara over an ORIENTED ELLIPSE split into 8 sectors, keeping
//     the lowest-variance sector -> flat, directional, palette-knife masses. The
//     ellipse is sampled on a FIXED grid in unit-disc space, so a 25 px stroke
//     costs the same as a 6 px one (constant ~180 taps) and holds frame budget.
//   * Each tap is weighted by DEPTH + NORMAL discontinuity (G-buffer) so strokes
//     STOP at silhouettes and objects keep their form (lost & found edges).
//   * Light-colour tint: gentle chroma lift + warm-light / cool-shadow grade, so
//     a coloured light bleeds its hue into the paint.
//   * Low-frequency, stroke-ALIGNED bristle streaks + a subtle canvas tooth sell
//     "paint sitting on canvas" (deliberately NOT high-frequency static).
//
// Inputs : gInput0 = lit HDR colour (RGBA16F, chained through the post stack)
//          gInput1 = G-buffer (octN.rg / rough.b / metal.a)
//          gInput2 = depth (R32F; 1 = sky)
// Params : gPostParams0 = (stroke size [1..7], stroke flow, blend strength, edge keep)
//          gPostParams1 = (light tint, warm/cool, canvas cell px, canvas strength)
//          gPostParams2 = (stroke detail, posterize levels [<2 = off], -, -)
// Output : HDR (still pre-tonemap).
#include "PostCommon.hlsli"

static const float kPI = 3.14159265f;

// NaN/Inf-safe colour fetch: max() returns the finite operand for a NaN input on
// all our targets, so this also scrubs garbage propagated from upstream passes
// (the old version smeared those into black blobs via the Kuwahara average).
float3 FetchHDR(uint tex, float2 uv) { return max(SamplePost(tex, uv).rgb, 0.0f); }

float NLuma(float3 c) { const float l = Luma(c); return l / (1.0f + l); }

// Cheap hash + value noise for bristle / canvas tooth.
float Hash(float2 p) {
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}
float ValueNoise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash(i + float2(0, 0)), Hash(i + float2(1, 0)), u.x),
                lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

// Warm lights, cool shadows (painter's bias), strength `s`. Deliberately gentle.
float3 WarmCool(float3 c, float s) {
    const float l = Luma(c);
    const float shadowW = saturate(1.0f - l * 2.0f);
    const float lightW = saturate(l * 2.0f - 1.0f);
    // Deliberately subtle so it tints rather than recolours (keeps lighting physical).
    float3 tint = lerp(float3(1, 1, 1), float3(0.95f, 0.975f, 1.04f), shadowW * s); // cool dark
    tint = lerp(tint, float3(1.04f, 1.00f, 0.95f), lightW * s);                     // warm light
    return c * tint;
}

float4 PSMain(FSOutput input) : SV_Target {
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float2 d = gInTexel; // 1 / sceneRes

    const float size = gPostParams0.x;
    if (size < 0.5f) return float4(FetchHDR(gInput0, uv), 1.0f); // disabled passthrough
    const float flow = saturate(gPostParams0.y);
    const float strength = saturate(gPostParams0.z);
    const float edgeKeep = saturate(gPostParams0.w);
    const float lightTint = gPostParams1.x;
    const float warmCool = gPostParams1.y;
    const float canvasCell = max(gPostParams1.z, 2.0f);
    const float canvasStr = gPostParams1.w;
    const float strokeDetail = gPostParams2.x;
    const float levels = gPostParams2.y;

    const float3 orig = FetchHDR(gInput0, uv);
    // Silhouette edge-stopping only matters when edgeKeep > 0; at edgeKeep == 0 the
    // per-tap depth weight is multiplied out (lerp t=0), so skip the centre + per-tap
    // depth samples and their exp() entirely. edgeKeep is a uniform, so this branch is
    // coherent across the whole pass (free) and the output is identical.
    const bool useEdge = edgeKeep > 0.001f;
    const float dc = useEdge ? SamplePost(gInput2, uv).r : 0.0f; // centre depth

    // Stroke length scale in pixels (the slider reads 1..7; paint wants real size).
    const float px = size * 3.0f;

    // --- smoothed structure tensor (Sobel at stroke scale) -> flow field ------
    // Sampling the gradient a few pixels out (not 1 px) gives a COHERENT
    // orientation at the scale of the strokes, instead of per-pixel noise.
    const float es = clamp(px * 0.35f, 1.0f, 4.0f);
    float lum[9];
    int idx = 0;
    [unroll] for (int sy = -1; sy <= 1; ++sy)
        [unroll] for (int sx = -1; sx <= 1; ++sx)
            lum[idx++] = Luma(FetchHDR(gInput0, uv + float2(sx, sy) * es * d));
    const float gx = (lum[2] + 2.0f * lum[5] + lum[8]) - (lum[0] + 2.0f * lum[3] + lum[6]);
    const float gy = (lum[6] + 2.0f * lum[7] + lum[8]) - (lum[0] + 2.0f * lum[1] + lum[2]);
    const float Sxx = gx * gx, Syy = gy * gy, Sxy = gx * gy;
    const float tr = Sxx + Syy;
    const float disc = sqrt(max((Sxx - Syy) * (Sxx - Syy) + 4.0f * Sxy * Sxy, 0.0f));
    const float anis = tr > 1e-5f ? disc / tr : 0.0f;        // 0 flat .. 1 strongly oriented
    // phi = angle of the major eigenvector (max-gradient = ACROSS the edge).
    const float phi = 0.5f * atan2(2.0f * Sxy, Sxx - Syy);
    const float cphi = cos(phi), sphi = sin(phi);
    const float2 across = float2(cphi, sphi);   // short axis (across the edge)
    const float2 along = float2(-sphi, cphi);   // long axis  (along the edge / flow)

    // Oriented ellipse: long along the form, short across it.
    const float a = anis * flow;
    const float rAlong = clamp(px * (1.0f + a * 1.3f), 2.0f, 26.0f);
    const float rAcross = clamp(px * (1.0f - a * 0.55f), 1.5f, rAlong);

    // --- 8-sector anisotropic Kuwahara on a fixed unit-disc grid -------------
    float3 cSum[8];
    float nSum[8], n2Sum[8], wSum[8];
    [unroll] for (int s = 0; s < 8; ++s) { cSum[s] = 0; nSum[s] = 0; n2Sum[s] = 0; wSum[s] = 0; }
    const float kSector = 8.0f / 6.2831853f;
    const float depthEdge = 700.0f * (0.15f + edgeKeep); // higher = stricter silhouettes

    // 9x9 grid (~64 disc taps). The Kuwahara output is smooth, so dropping from an
    // 11x11 grid is nearly invisible but ~1/3 cheaper. Each tap also samples ONE
    // texture (HDR colour) + depth; the per-tap NORMAL sample was removed - depth
    // discontinuity alone stops strokes at silhouettes (different objects differ in
    // depth), which is what made this pass cost ~5 ms fullscreen.
    // UNROLLED: the grid cell `e`, its `r2`, the spatial weight exp(-2*r2), the
    // disc cull and the sector atan2 are all CELL-CONSTANT (same for every pixel),
    // so unrolling lets the compiler fold them to literals - ~64 atan2 + ~64 exp per
    // pixel vanish, and `sec` becomes a static index. Identical output, big ALU cut.
    const int STEPS = 4;
    const float inv = 1.0f / STEPS;
    [unroll] for (int gyi = -STEPS; gyi <= STEPS; ++gyi) {
        [unroll] for (int gxi = -STEPS; gxi <= STEPS; ++gxi) {
            const float2 e = float2(gxi, gyi) * inv; // [-1,1]^2
            const float r2 = dot(e, e);
            if (r2 > 1.0f) continue;
            // Map the unit-disc sample to the oriented ellipse, in pixels.
            const float2 off = e.x * rAcross * across + e.y * rAlong * along;
            const float2 suv = uv + off * d;
            const float3 c = FetchHDR(gInput0, suv);

            // Don't pull colour across a silhouette: weight by depth proximity to
            // the centre pixel (lost & found edges). Only when edgeKeep > 0 - else the
            // sample + exp are skipped (see useEdge above). Depth alone catches object
            // boundaries; the old per-tap normal sample was dropped for cost.
            float wEdge = 1.0f;
            if (useEdge) {
                const float ds = SamplePost(gInput2, suv).r;
                wEdge = lerp(1.0f, exp(-depthEdge * abs(ds - dc)), edgeKeep);
            }
            const float w = exp(-2.0f * r2) * wEdge + 1e-5f;
            const float nl = NLuma(c);
            const int sec = clamp((int)floor((atan2(e.y, e.x) + kPI) * kSector), 0, 7);
            cSum[sec] += c * w;
            nSum[sec] += nl * w;
            n2Sum[sec] += nl * nl * w;
            wSum[sec] += w;
        }
    }

    // Blend sectors by inverse variance: the flattest sector dominates (-> a
    // mass of one colour), but neighbours still contribute (no 8-way pinwheel).
    float3 painted = 0.0f;
    float outW = 0.0f;
    [unroll] for (int k = 0; k < 8; ++k) {
        if (wSum[k] < 1e-4f) continue;
        const float mean = nSum[k] / wSum[k];
        const float var = max(n2Sum[k] / wSum[k] - mean * mean, 0.0f);
        const float vw = 1.0f / (1.0f + pow(var * 42.0f, 3.0f));
        painted += (cSum[k] / wSum[k]) * vw;
        outW += vw;
    }
    painted = outW > 1e-5f ? painted / outW : orig;

    float3 col = lerp(orig, painted, strength);

    // --- Light-colour tint: gentle chroma lift, then warm-light/cool-shadow --
    if (lightTint > 0.0f) {
        const float l = Luma(col);
        col = l + (col - l) * (1.0f + lightTint * 0.6f);
        col = WarmCool(col, warmCool * (0.4f + 0.6f * lightTint));
    } else if (warmCool > 0.0f) {
        col = WarmCool(col, warmCool);
    }
    col = max(col, 0.0f);

    // --- Posterize value into painterly steps (chroma preserved) -------------
    if (levels >= 2.0f) {
        const float l = max(Luma(col), 1e-4f);
        const float q = floor(l * levels + 0.5f) / levels;
        col *= q / l;
    }

    // --- Visible brush strokes: multi-scale, directional texture ------------
    // The Kuwahara gives smooth masses; THIS is what makes them read as paint.
    // Work in the stroke frame (pa = along the flow, pc = across it) and stack
    // two scales: broad palette-knife marks that run along the stroke, plus fine
    // bristle lines. Strong contrast so the marks are actually visible.
    const float2 puv = uv / d; // pixel coordinates
    if (strokeDetail > 0.0f) {
        const float pa = dot(puv, along);
        const float pc = dot(puv, across);
        // Broad strokes: elongated ~3:1 along the flow.
        const float broad = ValueNoise(float2(pc / max(rAcross * 0.7f, 1.5f),
                                              pa / max(rAlong * 1.7f, 6.0f)));
        // Fine bristle lines: high frequency across, streaked along.
        const float bristle = ValueNoise(float2(pc / 2.2f, pa / max(rAlong * 0.9f, 4.0f)));
        // Centre both to [-0.5,0.5]; sharpen the broad term so marks have edges.
        const float marks = (broad - 0.5f) * 1.3f + (bristle - 0.5f) * 0.5f;
        // Modulate value AND a touch of the existing chroma so strokes feel laid
        // down, not just dimmed. *1.6 makes them read at the default detail level.
        col *= 1.0f + marks * strokeDetail * 1.6f;
    }
    if (canvasStr > 0.0f) {
        // Soft value-noise tooth (no raw sin grid -> no Nyquist moiré).
        const float weave = ValueNoise(puv / canvasCell) - 0.5f;
        col *= 1.0f + weave * canvasStr;
    }

    return float4(max(col, 0.0f), 1.0f);
}

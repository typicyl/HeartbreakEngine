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

// 0 = the DIRECTIONAL BRUSH-STROKE filter (default: O(R) line integral, reads as
//     dragged pigment). 1 = the legacy 8-sector anisotropic Kuwahara (O(R^2) disc
//     gather, reads as rounded blobs) - kept for A/B and as a fallback.
#ifndef HBE_PAINTERLY_KUWAHARA
#define HBE_PAINTERLY_KUWAHARA 0
#endif

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
    // Silhouette edge-stopping is ALWAYS on: without it the Kuwahara pulls flat
    // bright-sky taps into ridge pixels (the sky's near-zero variance wins the
    // inverse-variance blend) and paints a bright RIM along silhouettes. edgeKeep
    // now only scales HOW strict the depth stop is, never disables it.
    const bool useEdge = true;
    const float dc = SamplePost(gInput2, uv).r; // centre depth

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

    // Shared by BOTH filter paths (declared here so the #if below can use them):
    // how much of the gather survived the silhouette edge-stop, and how strict
    // that stop is. edgeKeep only scales the strictness, never disables it.
    float spatialSum = 0.0f, edgeSum = 0.0f;
    const float depthEdge = 700.0f * (0.35f + 0.65f * edgeKeep);

#if !HBE_PAINTERLY_KUWAHARA
    // =====================================================================
    // DIRECTIONAL BRUSH-STROKE FILTER (default)
    // =====================================================================
    // Replaces the 8-sector anisotropic Kuwahara. Two reasons:
    //
    // 1. COST. Kuwahara gathers a (2*STEPS+1)^2 = 81-tap DISC and keeps four
    //    8-element sector accumulators (~80 live floats). It is O(R^2) and was
    //    the single most expensive pass in the frame (2.3 ms at 1080p).
    //    Smearing ALONG one line is O(R): 2*TAPS+1 samples, no accumulator
    //    arrays, no per-sector variance.
    //
    // 2. LOOK. A disc gather picks the flattest SECTOR, which produces rounded
    //    isotropic MASSES - the "blobs". A brush stroke is not a blob: it is
    //    pigment dragged along a direction. Integrating along the flow tangent
    //    is literally that operation (a line-integral convolution), so the
    //    result reads as a stroke without needing to fake one.
    //
    // The piece that makes it read as SEPARATE strokes rather than a smooth
    // directional blur is QUANTISATION: the sampling line is snapped to a
    // stroke lattice, so every pixel inside one stroke integrates the SAME
    // line and lands on the same colour, producing a discrete mark with a
    // visible edge against its neighbour. A continuous smear would just look
    // like motion blur.
    const float aniso = anis;

    // COHERENCE: how much real orientation this pixel actually has. In a smooth
    // gradient (sky) the structure tensor's trace is ~0, so `anis` and `phi` are
    // essentially NOISE. Everything directional below is scaled by this, so flat
    // regions get a smooth, near-isotropic mark instead of a confidently-wrong one.
    const float coh = saturate(aniso * flow);

    // Stroke frame. `along` is per-pixel and rotates freely, so a stroke needs a
    // COHERENT frame or neighbouring pixels sample different distant points (that
    // was the silhouette speckle). But a HARD angle snap is worse: it turns the
    // noisy angle field of a smooth sky into large plateaus of one angle separated
    // by hard boundaries - which is what read as blocky pixelation. So quantise
    // FINELY and blend back toward the true tangent by coherence: coh~0 (flat) ->
    // unquantised and smooth; coh~1 (a real form edge) -> snapped and coherent.
    const float kTwoPi = 6.28318531f;
    const float kAngSteps = 24.0f;
    const float rawAng = atan2(along.y, along.x);
    const float qAng = floor(rawAng * (kAngSteps / kTwoPi) + 0.5f) * (kTwoPi / kAngSteps);
    const float useAng = lerp(rawAng, qAng, coh);
    const float2 tangent = float2(cos(useAng), sin(useAng));
    const float2 acrossQ = float2(-tangent.y, tangent.x);

    // Stroke geometry. Length runs with the flow; strokes stay chunky in flat
    // regions (where there is no orientation to follow) by falling back to a
    // shorter, more isotropic mark instead of smearing an arbitrary direction.
    const float strokeLen = px * lerp(0.55f, 1.6f, coh);
    const float strokeWidth = max(px * 0.45f, 1.5f);
    const float2 cellSize = float2(max(strokeLen, 1.0f), max(strokeWidth, 1.0f));

    // SMOOTH pull toward the stroke spine, instead of snapping to a lattice cell.
    // floor() is a STEP function: every cell boundary is a discontinuity, and that
    // discontinuity IS the pixelation - anchoring only shrank its amplitude, it did
    // not remove it. sin(2pi*frac) is periodic and C1-continuous, and vanishes at
    // BOTH the spine and the boundary, so pigment still gathers along spines while
    // neighbouring strokes blend into each other instead of tiling. Scaled by `coh`
    // so smooth regions are not clustered at all.
    const float2 sp = float2(dot(uv / d, tangent), dot(uv / d, acrossQ));
    const float2 fc = frac(sp / cellSize);
    const float2 pull = sin(kTwoPi * fc) * (cellSize / kTwoPi) * (0.55f * coh);
    const float2 baseUv = uv + (pull.x * tangent + pull.y * acrossQ) * d;

    // Line integral along the stroke. TAPS is a compile-time constant so the
    // loop unrolls; the body has NO `continue` (that silently defeats [unroll]
    // on the SPIR-V path - see the BrushStrokes/Painterly perf notes).
    const int TAPS = 6;                       // 13 samples vs Kuwahara's 81
    const float step = strokeLen / (float)(TAPS * 2 + 1);
    // Seed with the shading pixel so a stroke can NEVER be composed entirely of
    // samples from the other side of a silhouette, and so the mark softens into
    // its own pixel instead of hard-edging against the neighbouring cell.
    float3 acc = orig * 0.35f;
    float accW = 0.35f;
    [unroll(13)] for (int t = -TAPS; t <= TAPS; ++t) {
        const float2 suv = baseUv + tangent * ((float)t * step) * d;
        // Depth edge-stop, same intent as the Kuwahara version: never drag
        // pigment across a silhouette, or bright sky bleeds onto ridge pixels.
        const float ds = SamplePost(gInput2, suv).r;
        const float wEdge = exp(-depthEdge * abs(ds - dc));
        // Triangular weight: the centre of the stroke carries the most pigment,
        // which is what gives a mark its loaded middle and dry ends.
        // NO epsilon floor here - it used to make every weight equal when the
        // edge-stop killed the whole line, so the result was an UNWEIGHTED mean of
        // the wrong-side taps (full-strength sky on a terrain pixel) and the
        // `orig` fallback below was dead code.
        const float wt = (1.0f - abs((float)t) / (float)(TAPS + 1)) * wEdge;
        acc += FetchHDR(gInput0, suv) * wt;
        accW += wt;
        spatialSum += 1.0f;
        edgeSum += wEdge;
    }
    float3 painted = accW > 1e-4f ? acc / accW : orig;

    // Bristle break-up ACROSS the stroke: a cheap 1D modulation of the value so
    // the mark is not a flat slab of colour. This is the same idea as the
    // stroke-detail term below but derived from the lattice, so it is free.
    if (strokeDetail > 0.0f) {
        const float bristle = frac(sp.y / max(strokeWidth, 1.0f) * 3.0f);
        // Scaled by `coh` too: without it, turning Stroke texture up sprays
        // high-frequency modulation across smooth skies where there is no stroke.
        const float b = (bristle - 0.5f) * strokeDetail * 0.25f * coh;
        painted *= 1.0f + b;
    }
#else
    // --- 8-sector anisotropic Kuwahara on a fixed unit-disc grid -------------
    float3 cSum[8];
    float nSum[8], n2Sum[8], wSum[8];
    [unroll] for (int s = 0; s < 8; ++s) { cSum[s] = 0; nSum[s] = 0; n2Sum[s] = 0; wSum[s] = 0; }
    // Track how much of the ellipse survived the silhouette edge-stop: near a hard
    // edge most taps are killed, leaving each sector a tiny, noisy sample -> the
    // grainy rim. We blend back to the crisp original there (below).
    // NOTE: spatialSum / edgeSum / depthEdge are declared ABOVE the #if - both
    // filter paths report edge coverage through them.
    const float kSector = 8.0f / 6.2831853f;

    // 9x9 grid (~64 disc taps). The Kuwahara output is smooth, so dropping from an
    // 11x11 grid is nearly invisible but ~1/3 cheaper. Each tap also samples ONE
    // texture (HDR colour) + depth; the per-tap NORMAL sample was removed - depth
    // discontinuity alone stops strokes at silhouettes (different objects differ in
    // depth), which is what made this pass cost ~5 ms fullscreen.
    // UNROLLED: the grid cell `e`, its `r2`, the spatial weight exp(-2*r2), the
    // disc cull and the sector atan2 are all CELL-CONSTANT (same for every pixel),
    // so unrolling lets the compiler fold them to literals - ~64 atan2 + ~64 exp per
    // pixel vanish, and `sec` becomes a static index. Identical output, big ALU cut.
    // CRITICAL - the unroll is load-bearing, and it must happen on BOTH backends.
    // `sec` below indexes cSum/nSum/n2Sum/wSum. Unrolled, `sec` is a compile-time
    // constant and those arrays stay in REGISTERS. Not unrolled, the index is
    // dynamic and the compiler is forced to put all four arrays in scratch memory,
    // turning every one of the ~64 taps into a local-memory read-modify-write.
    //
    // That is exactly what used to happen on Vulkan: DXC honoured [unroll] for
    // DXIL (2705 instructions, 110 inlined samples, ZERO allocas) but NOT for
    // SPIR-V (902 instructions, four `OpVariable ... Function` arrays), which made
    // this pass ~2.5-3x slower on Vulkan than D3D12 for identical HLSL.
    // The blocker was the `continue` below - rewriting it as a positive `if` lets
    // the SPIR-V backend unroll too. Explicit trip counts make the intent
    // unambiguous rather than relying on the compiler inferring the bound.
    //
    // If you touch this loop, re-verify with:
    //   dxc -T ps_6_5 -E PSMain -spirv ... -Fo p.spv && spirv-dis p.spv | grep "OpVariable %_ptr_Function"
    // Any Function-storage ARRAY there means the unroll broke and this pass just
    // got several times slower on Vulkan.
    const int STEPS = 4;
    const float inv = 1.0f / STEPS;
    [unroll(9)] for (int gyi = -STEPS; gyi <= STEPS; ++gyi) {
        [unroll(9)] for (int gxi = -STEPS; gxi <= STEPS; ++gxi) {
            const float2 e = float2(gxi, gyi) * inv; // [-1,1]^2
            const float r2 = dot(e, e);
            // Positive form (was `if (r2 > 1.0f) continue;`): identical maths, but
            // the early-continue stopped DXC's SPIR-V backend from unrolling.
            if (r2 <= 1.0f) {
            // Map the unit-disc sample to the oriented ellipse, in pixels.
            const float2 off = e.x * rAcross * across + e.y * rAlong * along;
            const float2 suv = uv + off * d;
            const float3 c = FetchHDR(gInput0, suv);

            // Don't pull colour across a silhouette: weight by depth proximity to
            // the centre pixel (lost & found edges). NO edgeKeep floor here - the old
            // lerp(1, w, edgeKeep) left every cross-silhouette tap a 1-edgeKeep weight
            // floor, which let the flat bright sky win the variance blend at ridge
            // pixels (the rim bug). Depth alone catches object boundaries.
            float wEdge = 1.0f;
            if (useEdge) {
                const float ds = SamplePost(gInput2, suv).r;
                wEdge = exp(-depthEdge * abs(ds - dc));
            }
            const float sw = exp(-2.0f * r2);
            const float w = sw * wEdge + 1e-5f;
            spatialSum += sw;
            edgeSum += sw * wEdge;
            const float nl = NLuma(c);
            const int sec = clamp((int)floor((atan2(e.y, e.x) + kPI) * kSector), 0, 7);
            cSum[sec] += c * w;
            nSum[sec] += nl * w;
            n2Sum[sec] += nl * nl * w;
            wSum[sec] += w;
            } // r2 <= 1 (disc cull)
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
#endif // HBE_PAINTERLY_KUWAHARA

    // Silhouette de-speckle: `edgeFrac` is the share of the ellipse that survived
    // the edge-stop. On a thin band at a hard silhouette (hill against sky) it drops
    // toward 0 - only a few noisy taps remain per sector, which is the grainy rim.
    // Fall back to the crisp original there (also keeps silhouettes sharp); flat
    // interiors keep edgeFrac~1 and stay fully painted, so nothing else is softened.
    // Band widened 0.25/0.6 -> 0.35/0.85: at a hard silhouette the PARTIAL-
    // coverage pixels (a few surviving taps) were still being painted, and a
    // handful of noisy taps is exactly what reads as speckle. Falling back
    // further keeps the rim clean. Applies to both filter paths.
    const float edgeFrac = spatialSum > 1e-5f ? saturate(edgeSum / spatialSum) : 1.0f;
    painted = lerp(orig, painted, smoothstep(0.35f, 0.85f, edgeFrac));

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
        // down, not just dimmed. *1.6 makes them read at the default detail level;
        // clamped so marks can never over-amplify already-bright edge pixels.
        col *= 1.0f + clamp(marks * strokeDetail * 1.6f, -0.35f, 0.35f);
    }
    if (canvasStr > 0.0f) {
        // Soft value-noise tooth (no raw sin grid -> no Nyquist moiré).
        const float weave = ValueNoise(puv / canvasCell) - 0.5f;
        col *= 1.0f + weave * canvasStr;
    }

    return float4(max(col, 0.0f), 1.0f);
}

// Shaders/BrushStrokes.hlsl - real stroke-based painterly rendering (SBR).
//
// Unlike Painterly.hlsl (a screen-space FILTER that smooths the image into
// oriented masses), this pass SPLATS thousands of actual brush-stroke marks
// across the screen and alpha-blends them over that smoothed underpainting:
//   * One instanced quad per stroke (no vertex buffer; built from SV_InstanceID).
//   * Strokes sit on a jittered screen grid; each samples the lit HDR for its
//     colour (so it picks up the light's colour) and the local structure-tensor
//     flow for its ORIENTATION (so strokes follow the forms / run along edges).
//   * A flat-brush footprint with bristle streaks in the pixel shader makes each
//     mark read as laid-down paint, not a sprite.
//   * Drawn in 2 layers from the C++ side (coarse then fine) over the Kuwahara
//     base, building the image the way a painter blocks in then details.
//
// Inputs : gInput0 = lit HDR colour (pre-Painterly, for crisp stroke colour)
//          gInput1 = G-buffer (unused here; reserved)
//          gInput2 = depth (R32F; 1 = sky) - silhouette shortening + 3D censor test
//          gInput3 = forward HDR (a = 2-bit painterly mask; bit1 = censored object).
//                    NOTE: composite uses the reverse (gInput2 = mask, gInput3 = depth).
// Params : gPostParams0 = (stroke length px, width fraction, size jitter, amount)
//          gPostParams1 = (colour jitter, edge keep, angle jitter, bristle detail)
//          gPostParams2 = (grid cols, grid rows, flow strength, layer seed)
//          gPostParams3 = area mask rect (minX, minY, maxX, maxY); full = (0,0,1,1)
// Output : HDR, alpha-blended (SRC_ALPHA, INV_SRC_ALPHA) onto the painterly base.
//
// NOTE: this pass has its OWN instanced VSMain, so it can't include
// PostCommon.hlsli (that defines a fullscreen-triangle VSMain that would clash).
// The post constants + clamp sampler below mirror PostCommon.hlsli exactly.
#include "Common.hlsli"

cbuffer PostConstants : register(b1)
{
    uint   gInput0;
    uint   gInput1;
    uint   gInput2;
    uint   gInput3;
    float2 gOutTexel;
    float2 gInTexel;
    float4 gPostParams0;
    float4 gPostParams1;
    float4 gPostParams2;
    float4 gPostParams3; // area mask (minX, minY, maxX, maxY in screen UV; full = no mask)
    // World-anchored censors (3D sphere test). Per censor: gCensors[i] =
    // (worldCenter.xyz, worldRadius); strength/feather packed per .x..w. Layout
    // kept byte-identical to PostCommon.hlsli / PostUBO (shared b1 cbuffer).
    float4 gCensors[4];
    float4 gCensorStrength;
    float4 gCensorFeather;
    uint4  gCensorCount;
};
[[vk::binding(2, 0)]] SamplerState gClampSampler : register(s1, space0);

float4 SamplePost(uint index, float2 uv)
{
    return gTextures[NonUniformResourceIndex(index)].SampleLevel(gClampSampler, uv, 0.0f);
}
float Luma(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

static const float kPI = 3.14159265f;

float Hash21(float2 p) {
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}
float VN(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i + float2(0, 0)), Hash21(i + float2(1, 0)), u.x),
                lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}
float3 FetchHDR(uint tex, float2 uv) {
    return max(gTextures[NonUniformResourceIndex(tex)].SampleLevel(gClampSampler, uv, 0.0f).rgb, 0.0f);
}
// Unproject a screen UV + depth back to a world position (for world-anchored strokes).
float3 WorldFromDepth(float2 uv, float depth) {
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

// A/B switch for the vertex-side coverage cull + conditional flow Sobel. 1 = the
// optimised path (default). Set to 0 to measure the pre-optimisation cost with an
// otherwise identical build - the only honest way to attribute a saving.
#ifndef HBE_STROKE_CULL
#define HBE_STROKE_CULL 1
#endif

struct VSOut {
    float4 pos : SV_Position;
    float2 luv : TEXCOORD0; // local quad uv 0..1 (x along stroke, y across)
    float3 col : TEXCOORD1; // stroke colour (HDR)
    float2 rnd : TEXCOORD2; // per-stroke randoms (bristle phase, value jitter)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const float lenPx = gPostParams0.x;
    const float widthFrac = gPostParams0.y;
    const float sizeJit = gPostParams0.z;
    const float edgeKeep = gPostParams1.y;
    const float angleJit = gPostParams1.z;
    const float cols = max(gPostParams2.x, 1.0f);
    const float rows = max(gPostParams2.y, 1.0f);
    const float flowStr = gPostParams2.z;
    const float seed = gPostParams2.w;

    const float2 res = 1.0f / gOutTexel; // screen size in px
    const float2 d = gInTexel;
    const uint ci = (uint)cols;
    const float cx = (float)(iid % ci);
    const float cy = (float)(iid / ci);
    const float2 cell = float2(res.x / cols, res.y / rows);

    // WORLD-ANCHORED placement: the strokes are seeded on a SCREEN grid (one instance per
    // cell), but each instance reconstructs the surface point under its cell from depth,
    // snaps it to a distance-scaled WORLD grid, and reprojects to screen. So a stroke
    // STAYS ON THE SURFACE as the camera moves (no screen-space "shower-door" swimming),
    // and its per-stroke randoms key off the WORLD cell, so the marks are also temporally
    // stable. The world cell size = the world extent of one screen cell at that depth, so
    // on-screen stroke density stays uniform (no gaps at grazing angles). Sky pixels (no
    // surface) fall back to the old screen-anchored grid. STOP-MOTION: strokes are
    // GLUED + BOIL: strokes anchor to the OBJECT (object-relative world cell below), so
    // they stay stuck to the surface and always cover it as the camera/object moves
    // (never reveal it underneath). The stop-motion "boil" comes from the time-quantized
    // seed (boilPhase) folded into hk -> the stroke pattern re-rolls only at the tick rate.
    const float2 sampPx = (float2(cx, cy) + 0.5f) * cell;
    const float2 sampUv = sampPx * gOutTexel;
    const float depth = SamplePost(gInput2, sampUv).r;

    float2 hk;
    float2 centerPx;
    bool inCensor = false; // this cell's surface is inside a censor sphere
    if (depth < 1.0f) {
        const float3 P  = WorldFromDepth(sampUv, depth);
        const float3 Px = WorldFromDepth(sampUv + float2(d.x, 0.0f), depth);
        float cellW = max(length(Px - P) * cell.x, 0.01f); // world size of one screen cell
        // If this cell's surface point is inside a censor sphere, anchor the grid to
        // the OBJECT: snap relative to the censor center (which rides with the entity)
        // so the strokes stick to the moving surface instead of crawling across it.
        float3 cOrigin = float3(0.0f, 0.0f, 0.0f);
        [unroll] for (uint ci = 0u; ci < 4u; ++ci) {
            if (ci >= gCensorCount.x) break;
            if (distance(P, gCensors[ci].xyz) < gCensors[ci].w) {
                cOrigin = gCensors[ci].xyz; inCensor = true; break;
            }
        }
        // CONSISTENT BOIL: inside a censor, size the world grid from the CAMERA->censor
        // distance instead of the per-pixel depth. That distance is constant while you
        // orbit, so the grid (and every stroke) stays fixed as the camera moves -> the
        // strokes only change at the boil rate (no "speed-up" with camera motion), yet
        // still scale with distance on a dolly. (Per-pixel cellW foreshortens with view
        // angle, which made the strokes re-snap every frame = the apparent speed-up.)
        if (inCensor) {
            // ~angular-size-per-pixel (0.0007) * the layer's screen-cell width (px) *
            // the camera->censor distance = a world cell that holds steady on orbit and
            // keeps each layer's density. Tune 0.0007 if the stroke spacing looks off.
            cellW = max(distance(gCameraPosWS, cOrigin) * cell.x * 0.0007f, 0.01f);
        }
        const float3 wc = round((P - cOrigin) / cellW);          // object-rel (or world) cell
        hk = wc.xz + wc.y * 0.7f + seed * float2(1.7f, 2.3f);    // stable seed (rides w/ object)
        const float2 jw = float2(Hash21(hk), Hash21(hk + 11.3f)) - 0.5f;
        const float3 anchor = (wc + float3(jw.x, 0.0f, jw.y) * 0.9f) * cellW + cOrigin;
        const float4 clip = mul(gViewProj, float4(anchor, 1.0f));
        centerPx = (clip.w > 1e-4f)
            ? float2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f) * res
            : float2(-1e5f, -1e5f); // behind the camera -> offscreen (quad is culled)
    } else {
        // Sky: no surface to anchor to - keep the screen grid + a stable per-cell jitter.
        hk = float2(cx + 1.0f, cy + 1.0f) + seed * float2(1.7f, 2.3f);
        const float2 jit = float2(Hash21(hk), Hash21(hk + 11.3f)) - 0.5f;
        centerPx = sampPx + jit * cell * 0.95f;
    }
    const float2 uv = centerPx * gOutTexel;

    // --- EARLY COVERAGE CULL -------------------------------------------------
    // Everything below this point costs ~16 texture fetches per vertex (a 5-tap
    // colour average, a 9-tap Sobel, and two silhouette depth taps), and the VS
    // runs SIX TIMES per stroke - once per quad vertex - even though all of that
    // work depends only on SV_InstanceID.
    //
    // The coverage test used to live only in the pixel shader, so a stroke outside
    // the paint area paid the whole VS cost six times and was then discarded. With
    // "real brush strokes" OFF the coverage box is EMPTY, which meant every stroke
    // on screen did that: the pass was near its full cost while drawing nothing.
    //
    // Testing coverage HERE, off the one depth sample already taken, collapses a
    // culled stroke to a degenerate triangle before any of that work happens.
    // The PS test stays (it is per-pixel and does the soft edges); this is a
    // conservative early-out, so the visual result is unchanged.
    {
        const bool inBox = uv.x >= gPostParams3.x && uv.x <= gPostParams3.z &&
                           uv.y >= gPostParams3.y && uv.y <= gPostParams3.w;
        // A censored stroke is always kept: censors paint even when the global box
        // is empty (that is the censor-only mode).
        if (HBE_STROKE_CULL && !inBox && !inCensor) {
            VSOut culled;
            culled.pos = float4(0.0f, 0.0f, 0.0f, 0.0f); // w=0 -> degenerate, clipped
            culled.luv = float2(0.0f, 0.0f);
            culled.col = float3(0.0f, 0.0f, 0.0f);
            culled.rnd = float2(0.0f, 0.0f);
            return culled;
        }
    }

    // Average the stroke colour over a small area at the stroke scale (a 5-tap cross)
    // rather than a single point. A fixed-position stroke that point-samples the scene
    // flickers frame-to-frame because the scene under it changes fast - specular
    // sparkle + the per-frame TAA sub-pixel jitter. Averaging makes each mark's colour
    // temporally stable, so the strokes stop the "fast flicker".
    const float2 cs = max(lenPx, 4.0f) * 0.25f * d;
    float3 c = FetchHDR(gInput0, uv)
             + FetchHDR(gInput0, uv + float2(cs.x, 0.0f))
             + FetchHDR(gInput0, uv - float2(cs.x, 0.0f))
             + FetchHDR(gInput0, uv + float2(0.0f, cs.y))
             + FetchHDR(gInput0, uv - float2(0.0f, cs.y));
    c *= 0.2f;

    // --- flow orientation: Sobel on luma at stroke scale -> structure tensor --
    // Nine texture fetches, and BOTH of the cases below discard the result:
    //   * a censored stroke uses randAng outright (see `ang` assignment), and
    //   * flowStr == 0 makes `oriented` zero, so the lerp returns randAng anyway.
    // Computing it regardless was ~9 wasted fetches per vertex (54 per stroke) in
    // exactly the censor-only configuration this feature ships in.
    const float randAng = (Hash21(hk + 3.7f) * 2.0f - 1.0f) * kPI;
    const bool needFlow = (!HBE_STROKE_CULL) || (!inCensor && flowStr > 0.0f);
    float edgeAng = randAng;
    float oriented = 0.0f;
    if (needFlow) {
        const float es = max(lenPx * 0.18f, 1.5f);
        float l[9];
        int k = 0;
        [unroll] for (int sy = -1; sy <= 1; ++sy)
            [unroll] for (int sx = -1; sx <= 1; ++sx)
                l[k++] = Luma(FetchHDR(gInput0, uv + float2(sx, sy) * es * d));
        const float gx = (l[2] + 2.0f * l[5] + l[8]) - (l[0] + 2.0f * l[3] + l[6]);
        const float gy = (l[6] + 2.0f * l[7] + l[8]) - (l[0] + 2.0f * l[1] + l[2]);
        const float gmag = sqrt(gx * gx + gy * gy);
        const float phi = 0.5f * atan2(2.0f * gx * gy, gx * gx - gy * gy); // gradient dir
        // Strokes run ALONG the edge (perp to gradient). In flat regions the
        // gradient is meaningless, so blend toward a random angle by flow strength.
        edgeAng = phi + 1.5707963f;
        oriented = saturate(gmag * flowStr * 6.0f);
    }
    // Censored strokes use a BOIL-STABLE orientation (random per object-cell, keyed off
    // the time-quantized seed) instead of the live image-gradient angle. The live angle
    // is recomputed every frame, so it re-orients continuously as the camera orbits =
    // the "speed-up" the user saw; the stable angle only re-rolls at the boil rate.
    float ang = inCensor ? randAng : lerp(randAng, edgeAng, oriented);
    ang += (Hash21(hk + 7.1f) - 0.5f) * angleJit * 1.4f;
    const float2 dir = float2(cos(ang), sin(ang));
    const float2 perp = float2(-dir.y, dir.x);

    // size with per-stroke jitter
    const float r1 = Hash21(hk + 23.9f);
    float len = lenPx * (1.0f + (r1 - 0.5f) * 2.0f * sizeJit);
    const float wid = len * widthFrac;

    // Silhouette awareness: if the stroke would bridge a depth discontinuity,
    // shorten it so paint doesn't smear across object edges.
    if (edgeKeep > 0.0f) {
        const float dc = SamplePost(gInput2, uv).r;
        const float dEnd = SamplePost(gInput2, uv + dir * len * 0.5f * d).r;
        const float cut = saturate(1.0f - abs(dEnd - dc) * 60.0f);
        len *= lerp(1.0f, cut, edgeKeep);
    }

    // quad corner (two triangles)
    float2 corners[6] = {float2(-0.5f, -0.5f), float2(0.5f, -0.5f), float2(0.5f, 0.5f),
                         float2(-0.5f, -0.5f), float2(0.5f, 0.5f),  float2(-0.5f, 0.5f)};
    const float2 cr = corners[vid];
    const float2 offset = cr.x * len * dir + cr.y * wid * perp;
    const float2 posPx = centerPx + offset;
    float2 clip = posPx * gOutTexel * 2.0f - 1.0f;
    clip.y = -clip.y;

    VSOut o;
    o.pos = float4(clip, 0.0f, 1.0f);
    o.luv = cr + 0.5f;
    o.col = c;
    o.rnd = float2(Hash21(hk + 41.3f), r1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    const float2 px = i.pos.xy;            // pixel coords (SV_Position)
    const float2 suv = px * gOutTexel;     // normalized screen UV
    // Stroke coverage = the global area box OR a world-anchored censor sphere.
    // The box is full-screen when "Real brush strokes" is on (or the user's censor-
    // box rect), and EMPTY when it's off -> then only censors paint. The censor is a
    // 3D sphere test (reconstruct world pos from depth) GATED by the per-pixel
    // censored flag (forward HDR alpha bit, gInput3), so strokes land ONLY on the
    // censored object's surface - not the floor/walls inside the sphere.
    float boxCov = (suv.x >= gPostParams3.x && suv.x <= gPostParams3.z &&
                    suv.y >= gPostParams3.y && suv.y <= gPostParams3.w) ? 1.0f : 0.0f;
    float censorCov = 0.0f;
    if (gCensorCount.x > 0u) {
        const float depth = SamplePost(gInput2, suv).r;             // R32F, 1 = sky
        const float censored = step(1.5f, SamplePost(gInput3, suv).a * 3.0f); // bit1
        if (depth < 1.0f && censored > 0.5f) {
            const float3 wp = WorldFromDepth(suv, depth);
            [unroll] for (uint ci = 0u; ci < 4u; ++ci) {
                if (ci >= gCensorCount.x) break;
                const float3 center = gCensors[ci].xyz;
                const float  radius = gCensors[ci].w;
                const float  inner = radius * (1.0f - gCensorFeather[ci]);
                const float  d = distance(wp, center);
                censorCov = max(censorCov, (1.0f - smoothstep(inner, radius, d)) * gCensorStrength[ci]);
            }
        }
    }
    const float cover = max(boxCov, censorCov);
    if (cover <= 0.003f) discard;
    const float amount = gPostParams0.w;
    const float sharp = saturate(gPostParams1.x); // 0 = soft/blended, 1 = crisp/opaque
    const float bristle = saturate(gPostParams1.w);
    const float colJit = 0.12f;

    const float u = i.luv.x; // along the stroke
    const float v = i.luv.y; // across the stroke
    // Flat-brush footprint. Sharpness pushes the falloff toward the edge, so a
    // high-sharp stroke is a near-solid mark with a crisp boundary (defined),
    // while a low-sharp stroke fades softly into the underpainting (blended).
    const float wEdge = lerp(0.20f, 0.46f, sharp); // across (long sides)
    const float lEdge = lerp(0.30f, 0.47f, sharp); // along (tapered ends)
    const float across = smoothstep(0.5f, wEdge, abs(v - 0.5f));
    const float along = smoothstep(0.5f, lEdge, abs(u - 0.5f));
    float a = across * along;

    // Bristle streaks running ALONG the stroke (vary across its width).
    const float ph = i.rnd.x * 31.0f;
    const float br = VN(float2(v * 16.0f + ph, u * 3.0f));
    a *= lerp(1.0f, 0.30f + br * 0.95f, bristle);
    if (a <= 0.004f) discard;

    // Per-stroke value jitter (hand-mixed paint) + ridge darkening in the gaps.
    const float vj = (i.rnd.y - 0.5f) * 2.0f * colJit;
    float3 col = i.col * (1.0f + vj);
    col *= 0.85f + 0.15f * br;
    // Defined edge: a thin darker rim along the stroke's long sides (impasto
    // ridge), only when sharp — makes crisp strokes read as separate marks.
    const float rim = smoothstep(wEdge - 0.06f, wEdge + 0.02f, abs(v - 0.5f));
    col *= 1.0f - rim * sharp * 0.30f;

    // Crisp strokes are also more opaque, so they sit on top rather than blend.
    const float op = lerp(0.70f, 1.0f, sharp);
    // Fold in the coverage so the censor edge feathers (and the empty box culls).
    return float4(max(col, 0.0f), saturate(a) * amount * op * cover);
}

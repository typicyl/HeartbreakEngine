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
//          gInput2 = depth (R32F; 1 = sky) - silhouette-aware stroke shortening
// Params : gPostParams0 = (stroke length px, width fraction, size jitter, amount)
//          gPostParams1 = (colour jitter, edge keep, angle jitter, bristle detail)
//          gPostParams2 = (grid cols, grid rows, flow strength, layer seed)
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
    // Jitter the seed within its cell so the grid never shows through.
    const float2 jit = float2(Hash21(float2(iid + 0.5f, seed)),
                              Hash21(float2(seed * 1.7f, iid + 0.5f))) - 0.5f;
    const float2 centerPx = (float2(cx, cy) + 0.5f) * cell + jit * cell * 0.95f;
    const float2 uv = centerPx * gOutTexel;

    const float3 c = FetchHDR(gInput0, uv);

    // --- flow orientation: Sobel on luma at stroke scale -> structure tensor --
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
    // Strokes run ALONG the edge (perp to gradient). In flat regions the gradient
    // is meaningless, so blend toward a random angle (scatter) by flow strength.
    const float edgeAng = phi + 1.5707963f;
    const float randAng = (Hash21(float2(iid + 3.0f, seed)) * 2.0f - 1.0f) * kPI;
    const float oriented = saturate(gmag * flowStr * 6.0f);
    float ang = lerp(randAng, edgeAng, oriented);
    ang += (Hash21(float2(seed * 2.3f, iid + 7.0f)) - 0.5f) * angleJit * 1.4f;
    const float2 dir = float2(cos(ang), sin(ang));
    const float2 perp = float2(-dir.y, dir.x);

    // size with per-stroke jitter
    const float r1 = Hash21(float2(iid * 0.7f + 1.0f, seed * 2.1f));
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
    o.rnd = float2(Hash21(float2(iid + 11.0f, seed * 3.3f)), r1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
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
    return float4(max(col, 0.0f), saturate(a) * amount * op);
}

// Shaders/BrushField.hlsli - the procedural painterly BRUSH FIELD.
//
// ONE source of truth, compiled BOTH ways:
//   * as HLSL, by Painterly.hlsl (the real renderer), and
//   * as C++, by Source/Renderer/BrushFieldTest.cpp (--test-brushfield).
// The C++ shim below is active only under __cplusplus. This is deliberate: a
// hand-written CPU "mirror" of shader maths drifts, and then the test proves a
// property of the mirror instead of a property of the shipped shader.
//
// WHAT THIS IS. A continuous procedural field, NOT a dab/stamp renderer and NOT
// a texture. Given a stable world point + surface normal it returns the three
// quantities the painterly pass needs:
//
//     B(P, N, d) -> { warp, coverage, height }
//
//   warp     tangent-plane displacement in WORLD units - where the paint was
//            dragged from. Feeds the line-integral filter's gather, so the
//            colour masses themselves get brush-cut boundaries.
//   coverage 0..1 pigment deposition. 1 = loaded, 0 = bare ground (dry brush).
//   height   -1..1 impasto micro-relief. Its screen-space gradient lights the
//            paint against the scene's real directional light.
//
// The DOWNSTREAM pipeline depends only on this triple, never on how it was
// produced - so a future ProceduralBrushField_Gabor() can replace the fBm
// generator without touching the painterly pass. Keep that boundary clean.
//
// EVERY input is stable scene state: world position, world normal, and camera
// DISTANCE (scale-ladder selection only). Nothing here may read screen UV, a
// frame index, time, or a view matrix - that is the whole point. See
// docs/Design-PainterlyBrushField.md.
#ifndef HBE_BRUSH_FIELD_HLSLI
#define HBE_BRUSH_FIELD_HLSLI

#ifdef __cplusplus
// ---------------------------------------------------------------------------
// C++ shim: HLSL vector types + intrinsics, so the body below compiles as C++.
// ---------------------------------------------------------------------------
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace hbe::brushfield {

using uint = std::uint32_t;
using float2 = glm::vec2;
using float3 = glm::vec3;
using uint3 = glm::uvec3;
using int3 = glm::ivec3;

using std::abs;
using std::cos;
using std::exp2;
using std::floor;
using std::log2;
using std::max;
using std::min;
using std::pow;
using std::sin;
using std::sqrt;

inline float frac(float x) { return x - std::floor(x); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float2 lerp(float2 a, float2 b, float t) { return a + (b - a) * t; }
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// HLSL loop attributes have no C++ spelling; `[unroll] for` would parse as a
// lambda-introducer, so it is a macro that expands to nothing here.
#define HBE_BF_UNROLL
#else
#define HBE_BF_UNROLL [unroll]
#endif // __cplusplus

// Every function below has EXTERNAL linkage as plain C++, so two translation
// units including this header collide at link time (HLSL has no such notion).
// HBE_BF_FN makes them inline for the C++ build and vanishes for HLSL.
#ifdef __cplusplus
#define HBE_BF_FN inline
#else
#define HBE_BF_FN
#endif

// ---------------------------------------------------------------------------
// 1. Integer-lattice hash.
// ---------------------------------------------------------------------------
// The hash the old painterly shaders used - frac(p.x*p.y) after a couple of
// frac/dot rounds - COLLAPSES at world-scale coordinates. Measured over a 64x64
// lattice block in float32, distinct outputs of 4096:
//
//     origin       0 -> 2196      origin  10 000 ->   26
//     origin   1 000 ->  943      origin 100 000 ->    1  (all zero)
//
// A 5 cm lattice reaches cell index 10 000 only 500 m from the world origin, so
// on any real level that field would band and then vanish outright. This one
// hashes INTEGER cell coordinates through a PCG-style mix instead: exact integer
// arithmetic has no magnitude falloff at all.
//
// PCG3D emits THREE decorrelated words per call. That is load-bearing for cost:
// the three-channel flow noise gets its three components from ONE hash per
// lattice corner rather than three.
//
// FLOAT32 CAVEAT (not fixable here): the LATTICE COORDINATE is still a float
// before it is floored. Past ~1e6 world units the float mantissa is coarser than
// a fine lattice cell and the field quantises. The scale ladder mitigates it (a
// distant surface uses a coarse level) but a player standing 1000 km from the
// origin would see it. That is the usual float32 world-origin limit, not a
// property of the hash.
HBE_BF_FN uint3 BfPcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16u;
    v.y ^= v.y >> 16u;
    v.z ^= v.z >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// Integer cell -> three decorrelated values in [0,1). `seed` separates octaves
// and channels so one lattice can be reused without correlating them. int->uint
// conversion is modular in both languages, so negative cells hash identically.
HBE_BF_FN float3 BfHash3(int3 c, uint seed)
{
    uint3 u;
    u.x = uint(c.x) + seed * 0x9E3779B9u;
    u.y = uint(c.y) + seed * 0x85EBCA6Bu;
    u.z = uint(c.z) + seed * 0xC2B2AE35u;
    const uint3 h = BfPcg3d(u);
    return float3(float(h.x & 0xFFFFFFu), float(h.y & 0xFFFFFFu), float(h.z & 0xFFFFFFu)) *
           (1.0f / 16777216.0f);
}

HBE_BF_FN float BfHash1(int3 c, uint seed) { return BfHash3(c, seed).x; }

// ---------------------------------------------------------------------------
// 2. Value noise.
// ---------------------------------------------------------------------------
HBE_BF_FN float BfFade(float t) { return t * t * (3.0f - 2.0f * t); }

// 2D THREE-CHANNEL value noise on the z = `layer` slice of the 3D lattice. Four
// corners. The extra two channels are FREE: BfHash3 produces three decorrelated
// words per corner anyway, and a single-channel version would just discard them.
// Anywhere the field needs a smooth 2D vector (the domain warp) or two unrelated
// smooth scalars, taking them from one call costs a quarter of what separate
// calls would.
HBE_BF_FN float3 BfNoise2v(float2 p, int layer, uint seed)
{
    const float fx = floor(p.x), fy = floor(p.y);
    const int ix = int(fx), iy = int(fy);
    const float ux = BfFade(p.x - fx), uy = BfFade(p.y - fy);
    const float3 a = BfHash3(int3(ix, iy, layer), seed);
    const float3 b = BfHash3(int3(ix + 1, iy, layer), seed);
    const float3 c = BfHash3(int3(ix, iy + 1, layer), seed);
    const float3 d = BfHash3(int3(ix + 1, iy + 1, layer), seed);
    return lerp(lerp(a, b, ux), lerp(c, d, ux), uy);
}

HBE_BF_FN float BfNoise2(float2 p, int layer, uint seed) { return BfNoise2v(p, layer, seed).x; }

// 3D three-channel value noise. Eight corners, ONE hash each - all three output
// channels ride along for free, which is what makes the flow field affordable.
HBE_BF_FN float3 BfNoise3v(float3 p, uint seed)
{
    const float fx = floor(p.x), fy = floor(p.y), fz = floor(p.z);
    const int3 i = int3(int(fx), int(fy), int(fz));
    const float ux = BfFade(p.x - fx), uy = BfFade(p.y - fy), uz = BfFade(p.z - fz);

    const float3 c000 = BfHash3(i + int3(0, 0, 0), seed);
    const float3 c100 = BfHash3(i + int3(1, 0, 0), seed);
    const float3 c010 = BfHash3(i + int3(0, 1, 0), seed);
    const float3 c110 = BfHash3(i + int3(1, 1, 0), seed);
    const float3 c001 = BfHash3(i + int3(0, 0, 1), seed);
    const float3 c101 = BfHash3(i + int3(1, 0, 1), seed);
    const float3 c011 = BfHash3(i + int3(0, 1, 1), seed);
    const float3 c111 = BfHash3(i + int3(1, 1, 1), seed);

    const float3 x00 = lerp(c000, c100, ux);
    const float3 x10 = lerp(c010, c110, ux);
    const float3 x01 = lerp(c001, c101, ux);
    const float3 x11 = lerp(c011, c111, ux);
    return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

// ---------------------------------------------------------------------------
// 3. Anti-repetition: incommensurate, rotated octaves.
// ---------------------------------------------------------------------------
// Exact-doubling fBm re-aligns its octaves on the same lattice, which is what
// gives naive fBm its faintly grid-flavoured look. Ratios that are NOT exactly
// 2, plus a per-octave domain rotation, mean no two octaves ever share a lattice
// direction, so the sum has no preferred axis and no recurring structure.
static const float kBfOctRatio[4] = {1.0f, 2.017f, 4.073f, 8.191f};
static const float kBfOctCos[4] = {1.0f, 0.73923f, 0.01469f, -0.62199f};
static const float kBfOctSin[4] = {0.0f, 0.67345f, 0.99989f, 0.78302f};
static const float kBfOctAmp[4] = {1.0f, 0.55f, 0.3025f, 0.166f};

// Anisotropic fBm in brush space. `aniso` is the along/across ratio: 1 = round
// (no direction), 8 = a long dragged streak. `octaves` is the quality knob.
// Returns ~[0,1], mean ~0.5.
HBE_BF_FN float BfFbmAniso(float2 uv, float aniso, int octaves, uint seed)
{
    float acc = 0.0f, norm = 0.0f;
    HBE_BF_UNROLL for (int o = 0; o < 4; ++o)
    {
        if (o < octaves)
        {
            const float c = kBfOctCos[o], s = kBfOctSin[o], r = kBfOctRatio[o];
            // Rotate the domain, THEN squash along the stroke, so every octave
            // elongates along the same brush direction while sampling a
            // differently-oriented lattice.
            const float2 q = float2(uv.x * c - uv.y * s, uv.x * s + uv.y * c) * r;
            acc += kBfOctAmp[o] *
                   BfNoise2(float2(q.x / max(aniso, 1.0f), q.y), o, seed + uint(o) * 7919u);
            norm += kBfOctAmp[o];
        }
    }
    return norm > 0.0f ? acc / norm : 0.5f;
}

// ---------------------------------------------------------------------------
// 4. The stable brush frame (direction field).
// ---------------------------------------------------------------------------
// D must be a pure function of stable scene state. It is built from a
// low-frequency world FLOW noise projected into the surface tangent plane.
//
// THE NAIVE VERSION IS NOT GOOD ENOUGH, and --test-brushfield proves it. With a
// single flow vector,
//
//     F = BfNoise3v(P * kFlow) - 0.5
//     D = normalize(cross(N, F))
//
// cross(N,F) degenerates wherever F is parallel to N. On a large surface F
// varies across it, so that is a codimension-2 set - isolated vortex points,
// which are inherent to ANY tangent direction field (hairy ball) and are fine.
// But on an object SMALLER than the flow wavelength - a limb, a pipe, a trunk,
// a prop - F is essentially constant while N sweeps a full 360 degrees, so the
// degenerate set becomes a whole LINE running along the object. Measured on a
// 0.4 m cylinder at a 12.5 m flow wavelength: 18% of the surface below half
// confidence, 99th-percentile direction turn 41.8 deg per sample, peak 89.7 deg.
// That is a visible seam wrapping every limb - not acceptable.
//
// THE FIX: a SECOND ambient vector held strictly perpendicular to the first,
//
//     G = cross(F, kAxis)     ->  G . F == 0 always
//
// so N cannot be parallel to both. Where cross(N,F) is ill-conditioned,
// cross(N,G) is at its best, and vice versa; the two are blended by how
// well-conditioned each is. G costs one cross product and NO extra noise
// evaluation, because it is derived from F.
//
// Blending needs one subtlety, and getting it wrong is worse than not blending
// at all. This is a LINE field, not a vector field - a bristle streak reads the
// same along D and -D. Averaging the two candidates as VECTORS requires first
// sign-aligning them, and that sign flip is a hard discontinuity wherever they
// sit near 90 degrees apart: measured, it took the flat floor from a 4.4 deg
// 99th-percentile turn to 69.1 deg. Undirected directions must be averaged in
// DOUBLE-ANGLE space (the same trick the painterly structure tensor already
// uses): map angle -> 2*angle, average there, halve the result. Opposite vectors
// map to the same doubled angle, so the ambiguity disappears instead of being
// papered over with a branch.
//
// The doubled-angle average also hands us a free, principled confidence: the
// LENGTH of the averaged doubled vector is the coherence of the two candidates.
// It falls to zero exactly where they disagree by 90 degrees with equal weight -
// which is a genuine singularity of the line field, not an artifact. A closed
// surface must have some (hairy ball again); the confidence fade is how they are
// absorbed.
//
// Residual singularities (both candidates ill-conditioned at once, which needs
// N parallel to F parallel to kAxis) are handled WITHOUT an arbitrary fallback
// axis (screen-space or otherwise): `confidence` fades the brush toward
// ISOTROPIC. Where the direction is undefined the mark is round, so an undefined
// direction has nothing left to express. That is also physically honest - a
// deposit of paint with no drag direction is round.
struct BfFrame
{
    float3 dir;        // along the stroke, in the tangent plane
    float3 across;     // perpendicular to dir, in the tangent plane
    float  confidence; // 0 = direction undefined (go isotropic), 1 = well defined
    // How strong the PRIMARY flow's tangential component is, before the partner
    // vector rescues it. `confidence` deliberately stays high near a critical point
    // of the primary field, because the partner still yields a usable direction
    // there - which is right for coverage but hides the fact that the direction has
    // stopped meaning anything. Anything that needs to know "is the flow actually
    // flowing here" (long strokes do; a per-pixel field does not) must read this.
    float  primary;
    float  load;       // low-frequency paint deposition, free from the same noise
};

// An arbitrary fixed world axis for the perpendicular partner. Nothing about it
// is special; it only has to not be an axis the art tends to align to, so the
// (measure-zero) F-parallel-to-kAxis case does not sit on level geometry.
static const float3 kBfPartnerAxis = float3(0.35714f, 0.81264f, 0.46156f);

HBE_BF_FN BfFrame BfBuildFrame(float3 P, float3 N, float flowScale)
{
    BfFrame f;
    const float3 raw = BfNoise3v(P * flowScale, 0x51EDu);
    const float3 F = raw - 0.5f;
    const float3 G = cross(F, kBfPartnerAxis); // strictly perpendicular to F

    const float3 c1 = cross(N, F);
    const float3 c2 = cross(N, G);
    const float l1 = sqrt(max(dot(c1, c1), 1e-20f));
    const float l2 = sqrt(max(dot(c2, c2), 1e-20f));
    // |cross(N,X)| = |X| sin(angle), so dividing by |X| makes each weight a pure
    // ANGLE measure - a merely SHORT vector must not read as a bad direction.
    const float w1 = l1 / sqrt(max(dot(F, F), 1e-20f));
    const float w2 = l2 / sqrt(max(dot(G, G), 1e-20f));
    // The partner is a FALLBACK, not a co-equal vote, and the difference is
    // visible. Squared weights let b1 and b2 come out close over sizeable
    // stretches, and where they do, a small change of position flips which
    // candidate wins - a ~90-degree jump in the blended direction. For a
    // per-pixel field that is a speckle; for a LONG STROKE that takes its frame
    // from one point, it is a stroke pointing the wrong way, and the direction
    // debug view shows exactly that: a smooth field with scattered outliers.
    //
    // Fourth powers plus a handicap on the partner make the primary win wherever
    // it is usable at all, so the partner only takes over where the primary has
    // genuinely degenerated - which is what it was introduced for.
    const float w1b = w1 * w1, w2b = w2 * w2;
    const float b1 = w1b * w1b;
    const float b2 = w2b * w2b * 0.25f;

    // Tangent-plane reference frame for the doubled-angle average. Using c1
    // itself as the reference is free and needs no basis-from-N (so no
    // hairy-ball singularity): a rotation of the reference by delta shifts the
    // doubled angles by -2*delta and the halved result by -delta, which the
    // reconstruction below rotates straight back. The average is therefore
    // INDEPENDENT of the reference, so an arbitrary - even discontinuous - one
    // is admissible.
    const float3 e0 = c1 * (1.0f / l1);
    const float3 e1 = cross(N, e0);
    const float3 u2 = c2 * (1.0f / l2);
    const float cosP = dot(u2, e0);
    const float sinP = dot(u2, e1);

    // Doubled angles: c1 is at 0 -> (1,0); c2 is at 2*phi via the double-angle
    // identities, so no trig call is needed.
    const float vx = b1 + b2 * (cosP * cosP - sinP * sinP);
    const float vy = b2 * (2.0f * sinP * cosP);
    const float r = sqrt(max(vx * vx + vy * vy, 1e-20f));

    // Halve the angle without atan2/sincos: half-angle formulas straight off the
    // doubled vector. cos(psi) = sqrt((r+vx)/2r), sin(psi) = sign(vy)*sqrt((r-vx)/2r).
    const float inv2r = 0.5f / r;
    const float cosH = sqrt(max((r + vx) * inv2r, 0.0f));
    const float sinH = sqrt(max((r - vx) * inv2r, 0.0f)) * (vy < 0.0f ? -1.0f : 1.0f);

    f.dir = e0 * cosH + e1 * sinH;
    f.across = cross(N, f.dir);
    // Confidence. NOTE the trap here, which cost a measurement round: `r` looks
    // like an agreement score, but c1 and c2 come from DELIBERATELY perpendicular
    // sources, so they disagree almost everywhere by construction - that is the
    // whole point of having a partner. Treating low agreement as low confidence
    // fades three quarters of a flat floor to round marks (measured: 75.2% of the
    // floor below half confidence, mean confidence 0.428) for no reason.
    //
    // Disagreement only means the direction is UNDEFINED in the one case where
    // the two candidates are 90 degrees apart AND equally weighted, so the
    // doubled-angle vector cancels: b1 == b2 with phi == 90 deg, which makes
    // r == 0. That is a genuine isolated singularity of the line field (a closed
    // surface must contain some), so the coherence fade is kept but made NARROW -
    // it should catch only the singular point, not the ordinary case.
    const float coherence = r / max(b1 + b2, 1e-20f);
    f.confidence = smoothstep(0.12f, 0.45f, max(w1, w2)) * smoothstep(0.0f, 0.08f, coherence);
    f.primary = smoothstep(0.10f, 0.55f, w1);
    f.load = raw.z; // third channel of the SAME evaluation: free
    return f;
}

// ---------------------------------------------------------------------------
// 5. Parameters.
// ---------------------------------------------------------------------------
// The vocabulary is deliberately the Art Editor's BrushDef vocabulary (hardness,
// grain, bristles, scatter, size), so a painterly look is authored with the same
// words as a hand brush. What does NOT cross over is MakeBrushTip's baked 64x64
// bitmap - stamping a bitmap is exactly the tiling this system exists to avoid.
struct BfParams
{
    float brushScale; // world units across one brush width at the reference distance
    float refDist;    // distance at which brushScale applies (ladder level 0)
    float sizeBias;   // artist "brush size", in ladder octaves (+1 = twice as broad)
    float flowScale;  // cycles per world unit of the direction field (LOW)
    float aniso;      // bristle elongation along the stroke (1 = round, 8 = dragged)
    float bristles;   // 0..1 strength of the directional streak structure
    float grain;      // 0..1 high-frequency chalky breakup
    float hardness;   // 0..1 coverage contrast (0 = feathered, 1 = firm-edged)
    float scatter;    // 0..1 dry-brush hole depth
    float warpAlong;  // gather displacement along the stroke, in brush widths
    float warpAcross; // gather displacement across the stroke, in brush widths
    float heightAmp;  // 0..1 impasto relief amplitude
    int   octaves;    // 1..4 bristle fBm octaves (quality knob)
    int   levels;     // 1 or 2 scale-ladder levels (quality knob)
};

HBE_BF_FN BfParams BfDefaultParams()
{
    BfParams p;
    p.brushScale = 0.16f;
    p.refDist = 6.0f;
    p.sizeBias = 0.0f;
    p.flowScale = 0.08f;
    p.aniso = 7.0f;
    p.bristles = 0.75f;
    p.grain = 0.35f;
    p.hardness = 0.5f;
    p.scatter = 0.25f;
    p.warpAlong = 0.55f;
    p.warpAcross = 0.18f;
    p.heightAmp = 0.6f;
    p.octaves = 3;
    p.levels = 2;
    return p;
}

// ---------------------------------------------------------------------------
// 6. The field result.
// ---------------------------------------------------------------------------
// THE downstream contract. Everything after this point in the renderer consumes
// only these values, never the generator that produced them - so swapping the
// fBm generator for a Gabor / sparse-convolution one later changes this file and
// nothing else.
struct BfSample
{
    // Gather displacement in the surface TANGENT frame: .x along `dir`, .y along
    // `across`, in WORLD units. The caller projects it to screen; keeping it in
    // world units here is what stops the field from ever seeing a view matrix.
    float2 warp;
    float  coverage; // 0..1 pigment deposition (1 = loaded, 0 = bare ground)
    float  height;   // -1..1 impasto relief; its gradient lights the paint
    // EXTREMELY low-frequency deposition (the direction noise's third channel,
    // wavelength ~1/flowScale metres - tens of metres at the default). Exposed
    // separately because the substrate tone must be driven by something at THIS
    // frequency: modulating a ground by `coverage` would make it flicker with the
    // bristles, which is a texture, not a ground.
    float  load;
    float3 dir;      // brush direction (world, in the tangent plane)
    float3 across;   // perpendicular, in the tangent plane
    float  confidence; // 0 = direction undefined, brush went isotropic
};

// ---------------------------------------------------------------------------
// 7. Structure at ONE scale-ladder level.
// ---------------------------------------------------------------------------
// Brush-space coordinates. NOT a planar projection: `dir` and `across` are built
// inside the tangent plane, so the projection is always onto the surface's own
// plane and nothing stretches at any orientation.
//
// The dot(P,N) term is the parallel-surface decorrelator. Without it two
// parallel surfaces - two floors of a building, facing walls of a corridor -
// project to the same (u,v) and get an identical pattern. dot(P,N) is the plane
// offset: constant within one flat surface (so it never disturbs that surface's
// own field) and different between parallel planes. One dot product.
static const float kBfDecorr = 0.41f;

HBE_BF_FN BfSample BfStructureAtScale(float3 P, float3 N, BfFrame fr, float scale, BfParams prm, uint seed)
{
    const float inv = 1.0f / max(scale, 1e-4f);
    const float2 uv = float2((dot(P, fr.dir) + dot(P, N) * kBfDecorr) * inv,
                             dot(P, fr.across) * inv);

    // Domain warp. One three-channel evaluation buys the warp vector (.xy) AND a
    // slow along-stroke modulator (.z) for the price of one. This is what
    // decorrelates structure over large distances and gives the hand-made wobble
    // - without it the octaves stay on their own lattices and the result reads
    // mechanical however aperiodic it is.
    const float3 w = BfNoise2v(uv * 0.17f, 7, seed + 101u);
    // (Spelled out rather than `w.xy` - glm has no swizzles without
    // GLM_FORCE_SWIZZLE, and this file has to parse as C++ too.)
    const float2 quv = uv + (float2(w.x, w.y) - 0.5f) * 1.35f;

    // Where the direction is undefined (isolated vortices), the mark goes ROUND
    // rather than picking an arbitrary direction to elongate along.
    const float anisoEff = lerp(1.0f, max(prm.aniso, 1.0f), fr.confidence);

    // Bristles: anisotropic fBm - high frequency ACROSS the stroke, very low
    // frequency ALONG it. That anisotropy IS the bristle; there is no streak
    // texture anywhere.
    const float bristle = BfFbmAniso(quv, anisoEff, prm.octaves, seed);

    // Dry-brush: modulate the bristle amplitude ALONG the streak so streaks fade
    // in and out instead of running forever as ruled lines. `w.z` is already a
    // slow, smooth, decorrelated field - reuse it rather than paying for another.
    const float dry = lerp(0.55f, 1.0f, w.z);

    // Chalky breakup: high frequency, mildly anisotropic so it still reads as
    // brush rather than as static.
    const float grain = BfNoise2(quv * 3.3f, 3, seed + 55u);

    BfSample s;
    // Coverage. `load` is the low-frequency deposition already carried by the
    // direction noise (free). Hardness is a contrast/gamma on the result, so a
    // soft brush feathers and a firm one lays a defined edge.
    const float loaded = lerp(0.62f, 1.0f, fr.load);
    float cov = lerp(1.0f, bristle * dry, prm.bristles);
    cov *= lerp(1.0f, grain, prm.grain * 0.85f);
    cov *= loaded;
    // Scatter opens genuine holes rather than merely dimming, which is what
    // makes a dry brush read as dry.
    cov -= prm.scatter * 0.6f * saturate(0.45f - grain) * 2.0f;
    const float gamma = lerp(0.55f, 2.2f, prm.hardness);
    s.coverage = saturate(pow(saturate(cov * 1.35f), gamma));

    // Impasto relief. Centred on zero so the lighting perturbation is symmetric,
    // and gated by `loaded` - thin paint has no ridge, which is how real paint
    // behaves and couples the two channels the way MakeBrushTip's alpha/detail do.
    s.height = (bristle - 0.5f) * 2.0f * prm.heightAmp * loaded;

    // Gather displacement, in world units, anisotropic because paint drags mostly
    // ALONG the stroke. Reuses the warp vector already computed - free.
    s.warp = float2((w.x - 0.5f) * 2.0f * prm.warpAlong, (w.y - 0.5f) * 2.0f * prm.warpAcross) *
             scale;

    s.dir = fr.dir;
    s.across = fr.across;
    s.confidence = fr.confidence;
    s.load = fr.load;
    return s;
}

// ---------------------------------------------------------------------------
// 8. The scale ladder, and the entry point.
// ---------------------------------------------------------------------------
// A fixed WORLD frequency sticks to surfaces perfectly but makes marks
// boulder-sized up close and sub-pixel mush at range - and it is artistically
// wrong, because a painter's brush is a constant size on the CANVAS. A fixed
// SCREEN frequency gets the size right and swims. So quantise SCALE, not space:
// pick a level from distance and cross-fade the two neighbouring levels. Both
// operands are fully world-locked, so neither moves relative to the surface;
// only the blend weight moves, and it is a smooth monotone function of camera
// distance alone. Rotation and orbit do not change distance, so they do not
// change the field at all.
//
// THIS IS NOT MATHEMATICALLY CONSTANT SCREEN SIZE, and should not be sold as
// such - perspective still varies apparent size within a level. What it
// guarantees is that the active feature scale stays within ONE OCTAVE of the
// target, which is enough to avoid both the boulders and the aliasing.
//
// Each level gets its own seed so the two are not scaled copies of one pattern
// (which would read as a zoom rather than as a change of brush). The seed keys
// off the ABSOLUTE level index, not off `k`, so the level that is the upper
// operand before a boundary is bit-identical to the lower operand after it.
HBE_BF_FN uint BfLevelSeed(int level) { return 0x9E37u + uint(level + 64) * 7919u; }

HBE_BF_FN BfSample BfEval(float3 P, float3 N, float dist, BfParams prm)
{
    const BfFrame fr = BfBuildFrame(P, N, prm.flowScale);

    const float L = log2(max(dist, 0.01f) / max(prm.refDist, 0.01f)) + prm.sizeBias;
    const float kf = floor(L);
    const int k = int(kf);
    const float scale0 = max(prm.brushScale, 1e-4f) * exp2(kf);

    BfSample a = BfStructureAtScale(P, N, fr, scale0, prm, BfLevelSeed(k));
    if (prm.levels < 2) return a;

    const BfSample b = BfStructureAtScale(P, N, fr, scale0 * 2.0f, prm, BfLevelSeed(k + 1));
    // smoothstep, not a raw frac: its derivative vanishes at both ends, so the
    // hand-off at a level boundary has no first-order discontinuity to pop.
    const float t = BfFade(saturate(L - kf));
    a.warp = lerp(a.warp, b.warp, t);
    a.coverage = lerp(a.coverage, b.coverage, t);
    a.height = lerp(a.height, b.height, t);
    // `load` is scale-INDEPENDENT (it comes from the frame, not the structure), so
    // both levels already carry the same value - nothing to blend.
    return a;
}

#ifdef __cplusplus
} // namespace hbe::brushfield
#endif

#endif // HBE_BRUSH_FIELD_HLSLI

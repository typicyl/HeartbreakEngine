// Shaders/ProceduralPaint.hlsli - procedurally GENERATED PAINT, evaluated at the
// shading point.
//
// ONE source of truth, compiled BOTH ways (same trick as BrushField.hlsli):
//   * as HLSL, by MeshPBR (the runtime), and
//   * as C++, by Source/Scene/ProcPaintTest.cpp, which realizes the SAME strokes
//     through the real CPU painting system (paint::Stroke -> BakeFromStrokes ->
//     Flatten) and diffs the two. That CPU path is the ORACLE: it defines what a
//     Heartbreak brush stroke is, and this file has to agree with it.
//
// WHAT THIS IS. Not a shader effect that resembles painting. This generates the
// same THING the painting system consumes - strokes with a centreline, a width
// that tapers, a cross-section with a solid core and a feathered rim, and a
// deposition amount - and hands MeshPBR exactly the four quantities it already
// gets from a paint canvas:
//
//     ProceduralPaint(posOS, nrmOS, dist) -> { albedo, coverage, metallic,
//                                              roughness, height }
//
// Everything downstream is untouched existing code: the albedo/material lerps and
// the relief central-difference that tilts the shading normal so impasto catches
// the real scene light (MeshPBR.hlsl:588-637).
//
// WHAT THIS IS NOT. There is no noise field here. Coverage comes from a stroke
// PROFILE, not from an fBm. An earlier attempt made noise the primary structure
// and produced grain; the measured ground truth (--paint-reference) says the
// dominant visual of Heartbreak paint is the stroke BODY - marks 20-95 px across
// on a 768 canvas, with the tip's internal texture almost entirely averaged away
// by ~90% dab overlap. Bristles and grain are secondary and are deliberately
// absent from this first milestone: with them off, the result must still read as
// broad strokes of paint.
#ifndef HBE_PROCEDURAL_PAINT_HLSLI
#define HBE_PROCEDURAL_PAINT_HLSLI

#include "BrushField.hlsli" // integer hash + the validated stroke-direction frame

#ifdef __cplusplus
namespace hbe::procpaint {
using namespace hbe::brushfield;
#endif

// ---------------------------------------------------------------------------
// Parameters. Deliberately the painting system's vocabulary (PaintSystem.h's
// BrushDef), because that is what "paint" already means in this engine.
// ---------------------------------------------------------------------------
struct ProcPaintParams
{
    // Stroke geometry, in the units of the surface coordinate handed in (object
    // space). The ground truth says a stroke is a BIG feature - 3-12% of the
    // frame - so these default large on purpose.
    float halfWidth;    // half the stroke's fat-middle width
    float length;       // nominal stroke length along its centreline
    float laneSpacing;  // distance between neighbouring stroke centrelines
    float widthJitter;  // 0..1 per-stroke width variation
    float lengthJitter; // 0..1 per-stroke length variation

    // Profile, straight out of MakeBrushTip / the editor's stroke dynamics.
    float hardness;    // 0 = small core + wide feather, 1 = broad core + tight edge
    float taperStart;  // fraction of the length tapered to nothing at the start
    float taperEnd;    // ... and at the end

    // Deposition. Pressure and flow are SEPARATE from width, exactly as
    // ReplayStroke treats them (d.flow = s.flow * pressure).
    float flow;      // base deposition
    float flowJitter; // 0..1 per-stroke loaded/dry variation
    // Dab spacing as a fraction of the radius - BrushDef::spacing. Nothing is
    // stamped here; this is what sets how hard the SWEPT edge comes out, because
    // it is how many dabs overlap at a point.
    float spacing;

    // What the paint is made of.
    float3 albedo;
    float  colorVar;  // per-stroke broken colour (ApplyColorVar's concept)
    float  metallic;
    float  roughness;
    float  height;    // relief amplitude per stroke
};

HBE_BF_FN ProcPaintParams ProcPaintDefaults()
{
    ProcPaintParams p;
    // ASPECT IS THE THING. The ground-truth swatches show strokes ~24 px wide
    // running most of a 768 canvas - roughly 20:1. The first attempt used 4:1 and
    // the taper then ate the whole stroke, turning every mark into a leaf shape.
    // Taper is a FRACTION of length, so only a long stroke gets a long body with
    // tapered tips instead of two ramps meeting in the middle.
    p.halfWidth = 0.047f;   // ~24 px wide on the test patch - the Oil Flat reference
    p.length = 2.05f;       // ~20:1 aspect
    p.laneSpacing = 0.062f; // < 2*halfWidth, so neighbouring strokes OVERLAP
    p.widthJitter = 0.40f;
    p.lengthJitter = 0.50f;
    p.hardness = 0.70f;   // the "Oil Flat" reference brush
    p.taperStart = 0.35f; // BrushDef defaults
    p.taperEnd = 0.35f;
    p.flow = 0.85f;
    p.flowJitter = 0.30f;
    p.spacing = 0.06f;
    p.albedo = float3(0.62f, 0.30f, 0.22f);
    p.colorVar = 0.18f;
    p.metallic = 0.0f;
    p.roughness = 0.55f;
    p.height = 0.50f;
    return p;
}

// ---------------------------------------------------------------------------
// One stroke's explicit definition.
// ---------------------------------------------------------------------------
// THE agreement point. The GPU membership test and the CPU oracle both build
// strokes from this function, so they cannot disagree about where a stroke is,
// how long it is, or how wide - only about how it is rasterized. Any divergence
// the test finds is therefore a real difference in the paint model, not two
// generators drifting apart.
struct ProcStroke
{
    float2 centre;    // (along, across) of the stroke's midpoint, stroke-frame units
    float  halfLength;
    float  halfWidth;
    float  flow;
    float3 albedo;
    uint   id;
};

// Per-stroke broken colour: a small warm/cool + value shift, hashed per stroke.
// The same idea as paint::ApplyColorVar - a flat colour pools into variety - but
// keyed off the stroke id rather than a dab position, because a procedural stroke
// is one object, not a sequence of stamps.
HBE_BF_FN float3 ProcStrokeColor(float3 base, float amt, float3 h)
{
    if (amt <= 0.0f) return base;
    const float a1 = h.x - 0.5f; // warm <-> cool
    const float a2 = h.y - 0.5f; // value
    const float a3 = h.z - 0.5f; // saturation-ish
    float3 c;
    c.r = base.r + (a1 * 0.14f + a2 * 0.11f) * amt;
    c.g = base.g + (a3 * 0.08f + a2 * 0.11f) * amt;
    c.b = base.b + (-a1 * 0.14f + a2 * 0.11f) * amt;
    return max(c, 0.0f);
}

HBE_BF_FN ProcStroke ProcGetStroke(int lane, int seg, ProcPaintParams p)
{
    // Two independent hashes per stroke: one for the across axis, one for along.
    const float3 ha = BfHash3(int3(lane, seg, 0), 0x50A17u);
    const float3 hb = BfHash3(int3(lane, seg, 1), 0x50A17u);

    ProcStroke s;
    // Lane centre, jittered. The offset is bounded to +/-0.25 of a lane so a
    // stroke can never wander further than one neighbouring cell - which is what
    // lets the membership test below look at a FIXED 3x3 neighbourhood and still
    // be exact rather than approximate.
    const float across = (float(lane) + 0.5f + (ha.x - 0.5f) * 0.5f) * p.laneSpacing;
    // Segment centre along the lane, jittered by up to +/-0.25 of a segment. The
    // jitter is keyed on (lane, seg), so neighbouring lanes' stroke ENDS never
    // line up - staggered, the way real brushwork lands.
    const float along = (float(seg) + 0.5f + (hb.x - 0.5f) * 0.5f) * p.length;
    s.centre = float2(along, across);
    s.halfLength = 0.5f * p.length * (1.0f - p.lengthJitter * 0.5f + p.lengthJitter * hb.y);
    s.halfWidth = p.halfWidth * (1.0f - p.widthJitter * 0.5f + p.widthJitter * ha.y);
    s.flow = saturate(p.flow * (1.0f - p.flowJitter * 0.5f + p.flowJitter * hb.z));
    s.albedo = ProcStrokeColor(p.albedo, p.colorVar, float3(ha.z, hb.x, ha.x));
    // Stable id: paint order and any downstream per-stroke lookup key off this.
    s.id = uint(lane * 73856093) ^ uint(seg * 19349663);
    return s;
}

// ---------------------------------------------------------------------------
// The stroke profile - width along, coverage across.
// ---------------------------------------------------------------------------
// Taper, lifted from the editor's stroke dynamics (Editor.cpp:3949-3951): the
// width smoothsteps up from nothing at the start and back down at the end, which
// is what stops a mark reading as a stripe. `u` is 0..1 along the stroke.
HBE_BF_FN float ProcTaper(float u, float taperStart, float taperEnd)
{
    const float a0 = (taperStart > 1e-3f) ? smoothstep(0.0f, taperStart, u) : 1.0f;
    const float a1 = (taperEnd > 1e-3f) ? smoothstep(0.0f, taperEnd, 1.0f - u) : 1.0f;
    return max(a0 * a1, 0.0f);
}

// The TIP's cross-section, lifted from MakeBrushTip (PaintSystem.cpp:100-113): a
// solid core out to `inner`, then a power-law feathered rim. Evaluated
// analytically instead of sampled from the 64x64 bitmap - same curve, no texture.
HBE_BF_FN float ProcTipProfile(float r, float hardness)
{
    const float inner = hardness * 0.55f;
    const float softPow = lerp(3.6f, 1.3f, hardness);
    if (r >= 1.0f) return 0.0f;
    const float a = (r <= inner) ? 1.0f : saturate(1.0f - (r - inner) / max(1.0f - inner, 1e-3f));
    return pow(a, softPow);
}

// The SWEPT stroke's cross-section - which is NOT the tip's, and assuming it was
// is a mistake the oracle caught immediately (GPU strokes measured 24 px wide
// against the oracle's 45 px, at a nominal 56 px).
//
// A painted stroke is the tip convolved along the path and composited "over"
// dozens of times: at `spacing` 0.06 of a radius, consecutive dabs overlap ~94%.
// Repeated over-compositing drives the accumulated coverage toward 1 wherever the
// tip contributes at all, so the stroke's real edge is far harder and its real
// body far wider than one dab's.
//
// Model it directly rather than integrating. A point at normalized across-distance
// `rn` is touched by every dab whose centre lies within +/-sqrt(1-rn^2) of it
// along the path, so it receives
//     N(rn) = (2 / spacing) * sqrt(1 - rn^2)
// dabs, each depositing about `flow * tip(rn)`. Compositing N of those "over":
//     A(rn) = 1 - (1 - flow*tip(rn))^N(rn)
// One pow. It reproduces the buildup that makes the middle solid and the rim
// short, and it goes to zero at rn = 1 because both terms do.
HBE_BF_FN float ProcSweptProfile(float rn, float hardness, float flow, float spacing)
{
    if (rn >= 1.0f) return 0.0f;
    const float tip = ProcTipProfile(rn, hardness);
    if (tip <= 0.0f) return 0.0f;
    const float n = (2.0f / max(spacing, 0.01f)) * sqrt(max(1.0f - rn * rn, 0.0f));
    const float a = saturate(flow * tip);
    if (a >= 0.999f) return 1.0f;
    return saturate(1.0f - pow(1.0f - a, n));
}

// Coverage of ONE stroke at a point in the stroke frame, plus where along the
// stroke that point sits (0..1) for the pressure envelope.
//
// Returned as a struct rather than through an `out` parameter: HLSL's `out` has
// no C++ spelling, and this file has to compile both ways.
struct ProcHit
{
    float coverage; // 0 = not in this stroke
    float u;        // 0..1 along the centreline
};

HBE_BF_FN ProcHit ProcStrokeCoverage(ProcStroke s, float2 q, ProcPaintParams p)
{
    ProcHit h;
    h.coverage = 0.0f;
    h.u = 0.0f;
    const float dAlong = q.x - s.centre.x;
    if (abs(dAlong) >= s.halfLength) return h;
    h.u = dAlong / s.halfLength * 0.5f + 0.5f; // 0..1 along

    // Width at THIS point along the stroke: the taper narrows it toward both ends,
    // so the body ends by vanishing rather than by being cut off.
    const float w = s.halfWidth * ProcTaper(h.u, p.taperStart, p.taperEnd);
    if (w <= 1e-6f) return h;

    const float r = abs(q.y - s.centre.y) / w;
    h.coverage = ProcSweptProfile(r, p.hardness, s.flow, p.spacing);
    return h;
}

// ---------------------------------------------------------------------------
// The published boundary.
// ---------------------------------------------------------------------------
struct PaintSample
{
    float3 albedo;
    float  coverage; // -> MeshPBR's `pa`
    float  metallic;
    float  roughness;
    float  height;   // -> the relief channel MeshPBR central-differences
    // DEBUG / VALIDATION ONLY, not consumed by shading: which stroke dominates
    // here. On a fully painted surface coverage is ~1 everywhere, so coverage
    // cannot tell whether the paint has stroke STRUCTURE - but how far you can
    // travel before this id changes can. That is the metric a grain field fails.
    uint   dominantId;
    float  dominantCoverage;
    float3 dominantDir; // frame of the stroke seen here (direction debug view)
};

HBE_BF_FN PaintSample ProcPaintEmpty(ProcPaintParams p)
{
    PaintSample o;
    o.albedo = p.albedo;
    o.coverage = 0.0f;
    o.metallic = p.metallic;
    o.roughness = p.roughness;
    o.height = 0.0f;
    o.dominantId = 0u;
    o.dominantCoverage = 0.0f;
    o.dominantDir = float3(1.0f, 0.0f, 0.0f);
    return o;
}

// Evaluate the painting at a point given in the STROKE FRAME (q.x along, q.y
// across, both in the same units as the params). Composites every stroke whose
// cell could reach this point, in ascending (lane, seg) order - the same order
// the CPU oracle emits them, so both composite identically.
HBE_BF_FN PaintSample ProcPaintAtFrame(float2 q, ProcPaintParams p)
{
    PaintSample o = ProcPaintEmpty(p);
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float h = 0.0f;

    const int lane0 = int(floor(q.y / max(p.laneSpacing, 1e-6f)));
    const int seg0 = int(floor(q.x / max(p.length, 1e-6f)));

    // A fixed 3x3 neighbourhood is EXACT, not an approximation: ProcGetStroke
    // bounds every jitter to +/-0.25 of a cell and every half-extent to under one
    // cell, so no stroke outside this window can reach q.
    HBE_BF_UNROLL for (int dl = -1; dl <= 1; ++dl)
    {
        HBE_BF_UNROLL for (int ds = -1; ds <= 1; ++ds)
        {
            const ProcStroke st = ProcGetStroke(lane0 + dl, seg0 + ds, p);
            const ProcHit hit = ProcStrokeCoverage(st, q, p);
            if (hit.coverage > 0.0f)
            {
                // Pressure envelope: a real stroke lands, loads and lifts. It
                // scales DEPOSITION, not width - PaintSystem.cpp:884.
                // `flow` is already inside the swept profile (it is what sets how
                // fast the buildup saturates), so only pressure multiplies here.
                const float pressure = 0.55f + 0.45f * sin(hit.u * 3.14159265f);
                const float av = saturate(hit.coverage * pressure);
                // Straight-alpha "over" with accumulating coverage - the exact
                // compositing WriteDabTexel does, so paint builds up rather than
                // replacing.
                acc = acc * (accA * (1.0f - av)) + st.albedo * av;
                accA = av + accA * (1.0f - av);
                if (accA > 1e-5f) acc /= accA;
                // Relief. DELIBERATELY NOT the CPU path's `+=`: that accumulates
                // additively across ~90%-overlapping dabs and clips to solid
                // white by mid-stroke, erasing the grooves it was meant to carry
                // (see docs/Design-PainterlyStrokes.md, A2 finding 3 - a bug in
                // the existing painting system, filed separately). An over-style
                // combine builds up the same way but is bounded by construction.
                h = h + p.height * av * (1.0f - h);
                if (av > o.dominantCoverage)
                {
                    o.dominantCoverage = av;
                    o.dominantId = st.id;
                }
            }
        }
    }

    if (accA > 1e-5f) o.albedo = acc;
    o.coverage = saturate(accA);
    o.height = saturate(h);
    return o;
}

// ---------------------------------------------------------------------------
// Varying stroke direction: PER-STROKE FRAMES.
// ---------------------------------------------------------------------------
// Two earlier formulations failed, and both are worth keeping written down
// because each looks reasonable until it is rendered.
//
// PER-PIXEL frame, q = (dot(P, D(P)), dot(P, A(P))). dq/dx picks up a term
// P . dD/dx, proportional to distance from the WORLD ORIGIN. At the measured flow
// turn (0.058 rad/unit) that is 2.4 stroke half-widths of shear at 2 units out, 12
// at 10, 245 at 200. Strokes tear apart everywhere except beside the origin, and
// no smaller flowScale helps - the term scales with distance, not stroke length.
//
// PER-REGION frame (one direction per jittered cell) removes the shear but
// quantises the field: rendered, the cell boundaries are plainly visible as
// angular seams where every stroke changes angle at once.
//
// THE PRIMITIVE IS A STROKE, so the frame belongs to the stroke: sampled from the
// flow field at that stroke's own origin, and carried unchanged over its whole
// extent. Neighbouring strokes then differ smoothly (their origins differ
// slightly, and the flow is smooth) while no single stroke shears at all.
//
// The catch is finding candidates. Uniform seeding costs ~13.4 * (halfLength /
// halfWidth) candidates - 134 at a 20:1 aspect - because long thin strokes are
// expensive to locate with an isotropic search. The fix is to SPLIT the two
// concerns:
//
//   PLACEMENT uses a coarse per-CELL frame, laying strokes out in lanes across
//             the cell. A discontinuity here only changes WHERE strokes sit,
//             which is invisible - irregular placement is wanted anyway.
//   FRAME     comes from the flow at each stroke's OWN origin. This is the thing
//             that showed seams, and it is now smooth and per-stroke.
//
// That gives ~3 lanes x 3x3x3 cells of real strokes, independent of aspect, and
// the acceleration structure never touches the visual primitive.
// Surface identity: which of six face classes this normal belongs to.
//
// NOT an invented threshold. This is exactly the classification the engine's own
// box paint projection already uses (BoxPaintUV, MeshPBR.hlsl:37-48): the dominant
// axis of the object-space normal plus its sign. Reusing it means procedural paint
// and box-projected artist paint agree about what counts as one surface.
//
// It is what lets stroke ownership RESTART at a hard boundary: the two faces of a
// box edge fall in different classes, so they cannot share a stroke, and a stroke
// generated for face A can never be re-derived with face B's 90-degrees-rotated
// frame. On a smooth surface the class holds over wide stretches and changes only
// at the 45-degree axis boundaries - see the limitation note in ProcPaintStrokes.
HBE_BF_FN int ProcFaceClass(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return (n.x >= 0.0f) ? 0 : 1;
    if (a.y >= a.z) return (n.y >= 0.0f) ? 2 : 3;
    return (n.z >= 0.0f) ? 4 : 5;
}

struct ProcStroke3
{
    float3 origin;
    float3 dir;    // the stroke's OWN frame, from the flow at `origin`
    float3 across;
    float  halfLength;
    float  halfWidth;
    float  flow;
    float3 albedo;
    uint   id;
};

// Where stroke `k` of cell `cell` sits, and how big it is. Deliberately does NOT
// build the stroke's frame: the frame costs a 3D noise evaluation, and most
// candidates are rejected on distance first (see ProcPaintStrokes).
//
// NOTE what is absent: any ownership test. An earlier version rejected strokes
// whose lane offset left the placement cell, which sliced continuous strokes at
// arbitrary cell boundaries and produced notched silhouettes and gaps. A cell
// decides where a stroke STARTS; it has no say over where the stroke ends. The
// stroke ends where its own taper says it ends.
HBE_BF_FN ProcStroke3 ProcSeedStroke(int3 cell, int k, int face, float3 cellOrigin, float3 N,
                                     float cellSize, ProcPaintParams p)
{
    // The face class is part of the key, so the same lattice cell yields DIFFERENT
    // strokes on either side of a hard edge instead of one corrupted stroke.
    const float3 h = BfHash3(int3(cell.x * 3 + k, cell.y * 5 + face, cell.z * 7 + k * 13),
                             0x5B0A1u);
    const float3 h2 = BfHash3(int3(cell.x + k * 101, cell.y + face * 37, cell.z + k * 61),
                              0x71C3Fu);

    ProcStroke3 s;
    // Jitter within the cell, then flatten into the tangent plane. Projecting a
    // cube jitter rather than using a tangent basis avoids needing one at all -
    // and placement has no continuity requirement, so nothing is lost.
    float3 off = (h - 0.5f) * cellSize * 0.95f;
    off = off - N * dot(off, N);
    s.origin = cellOrigin + off;

    s.halfLength = 0.5f * p.length * (1.0f - p.lengthJitter * 0.5f + p.lengthJitter * h2.x);
    s.halfWidth = p.halfWidth * (1.0f - p.widthJitter * 0.5f + p.widthJitter * h2.y);
    s.flow = saturate(p.flow * (1.0f - p.flowJitter * 0.5f + p.flowJitter * h2.z));
    s.albedo = ProcStrokeColor(p.albedo, p.colorVar, float3(h.z, h2.x, h.x));
    s.id = (uint(cell.x * 73856093) ^ uint(cell.y * 19349663) ^ uint(cell.z * 83492791) ^
            uint(k * 2971215073)) * 6u + uint(face);
    // Frame filled in by the caller, for survivors only.
    s.dir = float3(1.0f, 0.0f, 0.0f);
    s.across = float3(0.0f, 0.0f, 1.0f);
    return s;
}

// Coverage of one placed stroke at a surface point, in THAT stroke's frame.
HBE_BF_FN ProcHit ProcStroke3Coverage(ProcStroke3 s, float3 P, float3 N, ProcPaintParams p)
{
    ProcHit h;
    h.coverage = 0.0f;
    h.u = 0.0f;
    const float3 rel = P - s.origin;
    // Off this point's tangent plane: a stroke belonging to another part of the
    // surface (the far side of a thin wall).
    if (abs(dot(rel, N)) > max(s.halfWidth, 0.5f * p.laneSpacing)) return h;

    const float dAlong = dot(rel, s.dir);
    if (abs(dAlong) >= s.halfLength) return h;
    h.u = dAlong / s.halfLength * 0.5f + 0.5f;

    // The taper narrows the stroke toward both ends, so the body ENDS BY VANISHING
    // rather than by being cut - this is the only thing that decides a silhouette.
    const float w = s.halfWidth * ProcTaper(h.u, p.taperStart, p.taperEnd);
    if (w <= 1e-6f) return h;
    const float r = abs(dot(rel, s.across)) / w;
    h.coverage = ProcSweptProfile(r, p.hardness, s.flow, p.spacing);
    return h;
}

// Paint a surface point with per-stroke frames.
//
// `cellSize` bounds how far a stroke's origin can be from the shaded point, which
// is what makes the 3x3x3 search exact rather than approximate. `strokesPerCell`
// sets density: a cell of area cellSize^2 needs about
// overlap * cellSize^2 / (2*halfWidth * 2*halfLength) strokes to cover itself.
//
// LIMITATIONS, both geometric and both deliberate rather than hidden:
//   * A straight band can only lie flat on a surface over L/R < ~0.5 rad. Longer
//     than that on a tight curve and it pinches. Curved trajectories are a later
//     milestone; for now keep the stroke short relative to the curvature radius.
//   * Surface identity is the six-way face class, so a smooth surface changes
//     class at the 45-degree axis boundaries. That is inherited from the engine's
//     own box paint projection, which has exactly the same seams.
HBE_BF_FN PaintSample ProcPaintStrokes(float3 P, float3 N, ProcPaintParams p, float flowScale,
                                       float cellSize, int strokesPerCell)
{
    PaintSample o = ProcPaintEmpty(p);
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float hgt = 0.0f;

    const int face = ProcFaceClass(N);
    const float inv = 1.0f / max(cellSize, 1e-4f);
    const int3 base = int3(int(floor(P.x * inv)), int(floor(P.y * inv)), int(floor(P.z * inv)));
    // A stroke can only reach this far from its own origin.
    const float reach = 0.5f * p.length * (1.0f + p.lengthJitter * 0.5f) + p.halfWidth;
    const float reach2 = reach * reach;

    HBE_BF_UNROLL for (int dz = -1; dz <= 1; ++dz)
    HBE_BF_UNROLL for (int dy = -1; dy <= 1; ++dy)
    HBE_BF_UNROLL for (int dx = -1; dx <= 1; ++dx)
    {
        const int3 cell = base + int3(dx, dy, dz);
        const float3 cellCentre =
            (float3(float(cell.x), float(cell.y), float(cell.z)) + 0.5f) * cellSize;
        // A 3D lattice point sits up to cellSize*sqrt(3)/2 OFF the surface, so
        // strokes placed at it fail the tangent-plane test and the surface renders
        // unpainted (measured 0% on the box). Slide the cell origin onto this
        // point's tangent plane. Half a cell, not a whole one: with a whole cell
        // two lattice layers survive and stack duplicate strokes in one place.
        const float planeOff = dot(cellCentre - P, N);
        if (abs(planeOff) <= cellSize * 0.5f)
        {
            const float3 cellOrigin = cellCentre - N * planeOff;
            HBE_BF_UNROLL for (int k = 0; k < 16; ++k)
            {
                if (k < strokesPerCell)
                {
                    ProcStroke3 st = ProcSeedStroke(cell, k, face, cellOrigin, N, cellSize, p);
                    // CHEAP REJECT before the expensive part: placement is a couple
                    // of hashes, the frame is a 3D noise evaluation. Most candidates
                    // die here, so only survivors pay for a frame.
                    const float3 d = P - st.origin;
                    if (dot(d, d) <= reach2)
                    {
                        // THE POINT OF ALL THIS: the frame comes from the flow at
                        // the stroke's OWN origin - not the pixel, not the cell.
                        const BfFrame sf = BfBuildFrame(st.origin, N, flowScale);
                        st.dir = sf.dir;
                        st.across = sf.across;
                        // VORTICES. Any tangent direction field on a surface has
                        // isolated points where it is undefined (hairy ball), about
                        // one per flow wavelength squared. They are harmless for a
                        // per-pixel field but NOT for long strokes: each stroke near
                        // one grabs a wildly different direction, so a point defect
                        // smears into a stroke-length-wide starburst that dominates
                        // the image.
                        //
                        // BfBuildFrame already reports how well-defined the direction
                        // is. Use it: where it is low the stroke shortens toward a
                        // round mark, which is both what removes the starburst and
                        // what a painter does when there is no drag direction. No
                        // arbitrary fallback axis, and nothing is hidden with noise.
                        // Shape by PRIMARY, not by confidence. Confidence stays
                        // high at a critical point of the flow because the partner
                        // vector still supplies a direction - so it cannot see the
                        // starburst. `primary` measures whether the flow is actually
                        // flowing, which is the question a long stroke has to ask.
                        // Shape by the WEAKER of the two signals. `primary` says
                        // whether the flow is actually flowing; `confidence` says
                        // whether the blended direction is trustworthy. A long
                        // stroke needs both, and either one alone misses a case.
                        const float trust = min(sf.primary, sf.confidence);
                        st.halfLength *= lerp(0.20f, 1.0f, trust);
                        st.halfWidth *= lerp(1.30f, 1.0f, trust);

                        const ProcHit hit = ProcStroke3Coverage(st, P, N, p);
                        if (hit.coverage > 0.0f)
                        {
                            const float pressure = 0.55f + 0.45f * sin(hit.u * 3.14159265f);
                            const float av = saturate(hit.coverage * pressure);
                            acc = acc * (accA * (1.0f - av)) + st.albedo * av;
                            accA = av + accA * (1.0f - av);
                            if (accA > 1e-5f) acc /= accA;
                            hgt = hgt + p.height * av * (1.0f - hgt);
                            if (av > o.dominantCoverage)
                            {
                                o.dominantCoverage = av;
                                o.dominantId = st.id;
                                o.dominantDir = st.dir;
                            }
                        }
                    }
                }
            }
        }
    }

    if (accA > 1e-5f) o.albedo = acc;
    o.coverage = saturate(accA);
    o.height = saturate(hgt);
    return o;
}

// Surface entry point. Builds the stroke frame from the validated direction field
// (BrushField.hlsli - two candidate vectors blended in double-angle space, proven
// on floor/wall/ceiling/sphere/cylinder/terrain by --test-brushfield) and
// evaluates the painting in it.
//
// OBJECT space, not world: paint must follow a skinned or deforming mesh, and it
// must not need UVs. `dist` is accepted for the scale ladder but the first
// milestone keeps a fixed scale - filtering is a deliberate, separate step, not
// something to lean on TAA for.
HBE_BF_FN PaintSample ProceduralPaint(float3 posOS, float3 nrmOS, float dist, float flowScale,
                            ProcPaintParams p)
{
    const BfFrame fr = BfBuildFrame(posOS, nrmOS, flowScale);
    // Surface-local stroke coordinates. Not a planar projection: dir and across
    // are built inside the tangent plane, so nothing stretches at any orientation.
    const float2 q = float2(dot(posOS, fr.dir), dot(posOS, fr.across));
    return ProcPaintAtFrame(q, p);
}

#ifdef __cplusplus
} // namespace hbe::procpaint
#endif

#endif // HBE_PROCEDURAL_PAINT_HLSLI

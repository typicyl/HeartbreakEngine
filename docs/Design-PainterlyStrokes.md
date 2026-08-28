# Painterly Stroke Field - Phase 2.5 re-evaluation

> **STATUS: DESIGN ONLY. No shader changes made for this document.**
> Written 2026-08-26 after the first implementation produced a fullscreen procedural
> noise filter instead of brush strokes. Supersedes the "brush field" contents of
> `Design-PainterlyBrushField.md` (its stability machinery is kept and reused; its
> *visual primitive* was wrong).

**What went wrong, stated plainly.** The delivered field was a multi-octave anisotropic
fBm whose value modulated the image. It was stable, aperiodic, world-anchored and cheap -
and it looked like grain, because **noise was the primary structure and there was no mark
at all**. A brush stroke is not a scalar field with a preferred direction; it is a *body*
with an inside, two edges, and two ends. Making the noise coarser would only have produced
larger blurry noise. The primitive itself has to change.

The one thing worth keeping from the failure is the diagnosis of the anisotropy: the
implementation *did* have high frequency across / low frequency along, which is correct.
It just had nothing for that anisotropy to live inside.

---

## A. How Heartbreak actually represents a brush stroke

Read from `Source/Scene/PaintSystem.{h,cpp}` and the authoring path in `Source/Editor/Editor.cpp`.

**A1 - A stroke is a PATH, not a mark.** `Stroke::path` is a `std::vector<StrokePoint>`
(`PaintSystem.h:181`), each point carrying `uv` / `localPos`, `radius` / `localRadius`, and
`pressure`. So the fundamental representation is a **centreline plus a per-point width plus
a per-point deposition rate**. Everything else is derived.

**A2 - Width is per-point and shaped by dynamics.** `Editor.cpp:3946-3956` builds the width
array for a 3D stroke:

```cpp
a0 = smoothstep(0, taperStart, t);        // start taper
a1 = smoothstep(0, taperEnd, 1 - t);      // end taper
w  = baseHalf * max(a0 * a1, 0.04);
w *= 1 + (nz(t*7 + 3.1) - 0.5) * sizeJitter * 1.2;
```

A stroke is therefore **fat in the middle and pointed at both ends**, with a slow width
wobble along its length. That silhouette is most of what makes a mark read as a brush
stroke rather than a stripe.

**A3 - Pressure drives DEPOSITION, not width.** `ReplayStroke` sets `d.flow = s.flow *
pressure` (`PaintSystem.cpp:884`). Width and opacity are independent axes.

**A4 - The cross-section is a solid core plus a power-law rim.** From `MakeBrushTip`
(`PaintSystem.cpp:98-113`):

```cpp
inner   = hardness * 0.55;
softPow = mix(3.6, 1.3, hardness);
a = (r <= inner) ? 1 : clamp(1 - (r - inner)/(1 - inner), 0, 1);
a = pow(a, softPow);
```

A hard brush is a near-flat plateau with a tight edge; a soft brush is a small core with a
wide feathered falloff. A flat brush additionally squashes the profile (`ey = ny / 0.34`),
i.e. a 3:1 ellipse.

**A5 - Deposition is straight-alpha "over" with accumulating coverage.** `WriteDabTexel`
(`PaintSystem.cpp:560-573`): `av = a * brushCov`, `outA = av + da*(1-av)`. Paint *builds
up*; it does not replace.

**A6 - Relief is buildup plus grooves, and the grooves dominate 4:1.**

```cpp
buildup = height * a;
grooves = (detail - 0.5) * height * 4.0 * a;
m[2] += buildup + grooves;
```

The comment is explicit that the grooves are what make the surface "rake light like real
thick oil paint". Impasto is not a smooth bump - it is a *combed* bump.

**A7 - Bristles are a separate `detail` channel, and they emerge from CONVOLUTION.** This
is the subtlest and most important finding. The tip bakes
`streak = ValueNoise(u01*22, v01*3.5)` (`PaintSystem.cpp:118`), where `u01` maps to ALONG
the stroke and `v01` ACROSS it (`Stamp` rotates into tip space at `PaintSystem.cpp:642`, so
`tu` is along). Taken alone that is *high frequency along, low across* - the opposite of a
bristle line, and it contradicts the code's own comment.

It works anyway because the tip is stamped every `radius * spacing` units with
`spacing` ~0.05-0.13, so consecutive dabs overlap by ~90%. Averaging along the path
annihilates the 22-cycle along-stroke component and leaves the 3.5-cycle across-stroke
component coherent over the whole path. **The bristle lines are a product of sweeping the
tip, not a property of the tip.** So the correct procedural form is: a handful of lanes
across the width (~3-4), slowly modulated along the length. A generator that skipped the
convolution and used the tip's raw frequencies would produce banding perpendicular to the
stroke.

Note also that bristles barely touch coverage - `a *= mix(1, 0.55 + 0.45*streak,
bAmt*0.6)` - and instead act on pigment value (+/-27%) and on relief. A loaded brush stays
opaque; dryness comes from low flow and from `grain`/`scatter`, which *do* punch holes.

**A8 - Why the painting system never degenerates into a texture.** Four reasons, all worth
copying:

1. The tip's domain is **bounded and oriented**: `[0,1]^2` mapped to one mark, rotated to
   the stroke direction, scaled to the brush radius. It has no existence outside a mark, so
   it cannot tile.
2. Marks are placed along an **authored path**, so their arrangement carries structure that
   no field has.
3. `ApplyColorVar` (`PaintSystem.cpp:592`) shifts each dab's hue/value/saturation from a
   hash of its position - "broken colour", so no two marks are the same swatch.
4. The stroke has **ends**. A tapered start and finish is what stops a mark reading as a
   stripe of pattern.

**A9 - What is already generated procedurally, and what could be.** Already procedural:
the tip profile, bristle/grain/scatter, the taper curve, size jitter, path wobble, and the
per-dab colour variation - all deterministic from `BrushDef` plus a seed. Only the *path*
is authored. So the procedural renderer's real job is precisely: **invent plausible paths
over a surface**, then reuse the rest of the model.

**A10 - Reusable concepts** (concepts, not code - the CPU path stays for the Art Editor):
centreline + width + pressure; `smoothstep` taper at both ends; the `inner`/`softPow`
cross-section; straight-alpha buildup; grooves-dominate-buildup relief; bristles as a few
lanes across; per-mark colour variation; and the `BrushDef` parameter vocabulary
(`hardness`, `grain`, `bristles`, `scatter`, `spacing`, `taperStart/End`, `sizeJitter`,
`wobble`).

---

## A2. Ground truth: what Heartbreak paint actually looks like

Reading the code cannot settle a *visual* question, so `--paint-reference <dir>`
(`Source/Scene/PaintReference.cpp`) drives the REAL painting code headless - five arced
sweeps with pressure envelopes, per brush in `DefaultBrushes()`, through
`BakeFromStrokes` -> `Flatten` - and writes the colour and relief canvases as PNGs. It is
an investigation tool, and it is also the oracle the procedural renderer gets diffed
against later.

Measured on a 768 x 768 canvas:

| brush | occupancy | run ALONG stroke | run ACROSS (= stroke width) | anisotropy |
|---|---|---|---|---|
| Soft | 8.3% | 55 px | 21 px | 2.62 |
| Hard | 29.2% | 207 px | 93 px | 2.23 |
| Bristle | 17.9% | 118 px | 46 px | 2.57 |
| Chalk | 14.7% | 84 px | 36 px | 2.34 |
| Flat | 8.7% | 57 px | 20 px | 2.89 |
| **Oil Flat** | **10.6%** | **68 px** | **24 px** | **2.89** |
| Dry Brush | 16.9% | 107 px | 44 px | 2.45 |
| Palette Knife | 12.0% | 77 px | 26 px | 2.92 |
| Colour Var | 22.5% | 158 px | 66 px | 2.40 |
| Spray | 0.8% | 6 px | 3 px | 1.68 |

Three findings, and all three contradict what the failed implementation assumed:

**1. A stroke is a BIG feature.** Across-stroke runs are **20-95 px on a 768 canvas** -
2.7% to 12% of the canvas dimension. Scaled to a 1080p frame that is a mark roughly
**30-130 px wide**. The delivered fBm had structure a few pixels across. The scale was
wrong by more than an order of magnitude, independently of the primitive being wrong.

**2. The tip's internal texture is almost entirely CONVOLVED AWAY.** At the shipped
`spacing` (0.05-0.13 x radius) consecutive dabs overlap ~90%, so the bristle/grain baked
into the tip averages out. The colour swatches for `Bristle`, `Dry Brush` and `Oil Flat`
are visually near-identical to `Soft`: clean, opaque, tapered bands. **The dominant visual
of Heartbreak paint is the BODY, not the texture inside it.** A procedural design that
leads with bristle detail would be wrong in the opposite direction from the last one.

**3. Relief saturates at default settings.** `m[2] += buildup + grooves` accumulates
additively across ~90%-overlapping dabs, so by mid-stroke the height channel clips and the
grooves that were meant to rake light are lost. Visible in every `ref_*_relief.png`: solid
white bands, no combing. This is a latent defect in the existing painting system, not
something the procedural renderer should reproduce - and it means the "grooves dominate
4:1" intent in `WriteDabTexel` does not survive to the screen at shipped brush settings.
Worth a separate ticket.

The target, as numbers the procedural system must hit: **occupancy 10-30%, stroke width
3-12% of the frame dimension, clean tapered bodies whose interior is only mildly textured.**

---

## A3. The twelve questions, answered

| # | Question | Answer |
|---|---|---|
| 1 | Stroke representation | `paint::Stroke`: type, layer id, projection mode, a snapshotted `BrushDef`, colour, metal/rough/height, flow, colorVar, and `std::vector<StrokePoint> path` |
| 2 | Centreline | The `path` itself - `StrokePoint::uv` (UV modes) or `localPos` + `localNormal` (3D projection mode) |
| 3 | Radius | Per point (`StrokePoint::radius` / `localRadius`), authored with taper x sizeJitter (`Editor.cpp:3946-3956`) |
| 4 | Pressure | Scales DEPOSITION, not width: `d.flow = s.flow * pressure` (`PaintSystem.cpp:884`) |
| 5 | Brush/surface interaction | Two modes. UV disc (`Stamp`) rasterizes the tip into canvas texels. 3D projection (`StampProjected`) rasterizes MESH TRIANGLES into UV space and tests each texel's interpolated local position against a brush SPHERE, culling faces with `dot(faceN, brushN) < 0.3`. The 3D mode crosses UV seams and never stretches |
| 6 | Paint storage | Per object: `PaintComponent` with N layers, each two RGBA8 buffers at `resolution^2` - colour (RGB albedo, A coverage) and material (R metal, G rough, **B relief**, A coverage). Plus the stroke history as the editable source of truth |
| 7 | Rendering | `Flatten` composites layers -> two bindless textures -> **MeshPBR samples them in the forward pass BEFORE lighting** (`MeshPBR.hlsl:588-637`): `albedo = lerp(albedo, paint.rgb, pa)`, same for metal/rough |
| 8 | Relief | Stored in the material canvas B channel (0.5 neutral). Rendered by central-differencing that channel at 4 taps and tilting the shading normal: `N = normalize(Ng - (Tg*grad.x + Bg*grad.y))`, `amt = heightScale * ma * 48`. So impasto is lit by the REAL scene lighting - not a baked highlight |
| 9 | Overlap | Straight-alpha "over" with accumulating coverage (`WriteDabTexel`), plus layer compositing in `Flatten`. Paint builds up; it never replaces |
| 10 | Coordinates | Three: mesh UV (`projection 0`), **box projection** (`projection 1`, `BoxPaintUV` - object-space position + face normal into a 4x4 atlas at uniform world density, so paint never stretches and **needs no UVs**), and 3D local-space proximity (`projection 2`, authoring only - it still bakes into the UV canvas) |
| 11 | GPU efficiency of existing paint | Yes, and cheaply: 2 texture samples (+4 more only where relief is active), one uniform branch, manual mip LOD with a distance bias so far strokes average into washes. `Prepare`/`UploadPrepared` already split the CPU bake off the main thread for streaming |
| 12 | Reusable directly? | **The RENDERING path, yes, completely.** The semantic hand-off at the shading point is only four quantities: `paintAlbedo.rgb`, `paintCoverage`, `paintMetalRough`, `paintHeight`. Anything that can produce those plugs into the existing lighting, relief and LOD code unchanged. The STORAGE path (baked canvases) is reusable but carries real memory and bake cost - see G |

---

## G. The hard requirement: can the existing path be the target?

**Yes - and the seam is narrower than expected.** `MeshPBR.hlsl:604-637` reduces the whole
painting system to four values at the shading point. Everything downstream (albedo lerp,
material lerp, the 4-tap relief gradient, the normal tilt, BRDF clamping) is agnostic to
where they came from.

### Option A - generate strokes, bake through the real painting system

Procedurally author `paint::Stroke` objects per mesh at load/stream time, run the existing
`BakeFromStrokes` -> `Prepare` -> `UploadPrepared`, render as artist paint.

- *Fidelity:* **perfect by construction** - it IS the painting system.
- *Temporal stability:* **absolute**. Paint is a texture on the surface, so skinning,
  animation, deformation, TAA and camera motion are all free and exact. Strictly better
  than any world-space field.
- *Compatibility:* composites with artist paint as ordinary layers; box projection covers
  meshes with poor UVs; terrain already has a canvas.
- *Memory:* two RGBA8 + mips per object - ~11 MB at 1024, ~0.7 MB at the 256 cap this
  project already sets (`maxStreamedPaintResolution = 256`). Scene-wide, tens to hundreds of MB.
- *Cost:* the CPU bake is O(strokes x dabs x texels). The repo already carries a scar from
  this - `Prepare` exists because flattening dozens of 1024^2 canvases in one finalize
  frame cost ~200 ms.
- *Complexity:* moderate; a generation + caching problem, no new rendering.

### Option B - GPU procedural paint, evaluated at shading time

Where MeshPBR samples the canvas, instead evaluate `ProceduralPaint(posOS, normalOS, ...)`
returning the same four values, feeding the identical downstream code.

- *Fidelity:* very high but not identical - profile, taper, deposition and relief maths
  port directly; what does not port is the accumulated-overlap history, which has to be
  approximated by compositing a fixed number of stroke layers.
- *Memory:* **zero**. No canvases, no streaming, no cache.
- *Cost:* per shaded pixel in the forward pass. Relief needs a gradient - 4 extra
  evaluations or an analytic derivative.
- *Resolution:* unbounded; no texel-density problem, no mip blur close up.
- *Filtering:* the one real gap. The texture path gets mips free, and the distance-based
  `lodBias` wash is a deliberate look feature. Procedurally this must be re-created by
  band-limiting - which is exactly what the scale ladder already built does.
- *Coordinates:* object space, so it needs no UVs and **follows deforming/skinned meshes
  correctly** - better than world space for characters.
- *Complexity:* touches MeshPBR and its 7 OpenPBR variants; needs a material flag so only
  opted-in surfaces pay.

### Option C - keep a separate painterly renderer (the current post-pass)

- *Fidelity:* structurally incapable of matching - no per-object surface parameterization,
  relief lighting faked in screen space, cannot layer with artist paint. Already
  demonstrated to produce the wrong primitive.
- *Only merit:* it is the code that exists. **Not a reason.**

### Comparison

| | fidelity to real painting | perf | memory | complexity | temporal stability | mesh/material compat | brush characteristics |
|---|---|---|---|---|---|---|---|
| **A bake** | **exact** | bake cost, cheap to draw | **high** | moderate | **exact** | needs UV or box | **all** |
| **B procedural** | high | per-pixel, no bake | **none** | moderate-high | high (object space) | **any mesh** | most; overlap history approximated |
| C post-pass | low | cheap | none | low | high | n/a | few |

### Recommendation: B, with A as its CPU oracle

**B is the runtime path.** It is the user's own description - "a GPU shader evaluating
whether a surface point belongs to a procedurally generated stroke" - and it is the only
option that can paint a whole world without a scene-wide bake and a scene-wide memory bill.
Crucially it reuses the *rendering semantics* wholesale: the impasto normal tilt that makes
paint catch real light is existing code that does not change at all.

**A becomes the correctness gate, not a dead end.** Generate one set of procedural strokes;
realize it BOTH ways - CPU through `BakeFromStrokes`, and GPU through the procedural
evaluator - and diff the images. That is exactly the pattern this repo already uses for
`--test-oceanfft`, where a CPU reference is "the oracle the GPU compute FFT is later diffed
against". It turns "does it look like Heartbreak painting?" from a judgement call into a
measurement.

A also stays the right answer for one case worth keeping in view: **hero/static assets**,
where an artist wants to hand-edit what the generator produced. Generating real `Stroke`
objects makes that possible - the output is editable paint, not a shader effect.

### What this changes about the interface

The Phase-2.5 `{body, deposit, relief, tint, alongAcross, confidence}` was closer but still
described a screen effect. Under Option B the interface should be **exactly the hand-off
MeshPBR already consumes**:

```
ProceduralPaint(posOS, normalOS, dist) -> {
    float3 albedo;              // pigment, per-stroke broken colour applied
    float  coverage;            // -> the existing `pa`
    float  metallic, roughness;
    float  height;              // -> the existing relief channel, 0.5 neutral
}
```

Stroke membership, stroke coordinate, profile, taper and deposition become *internals* that
produce those four values - not the published interface. That makes the boundary identical
to the painting system's own, which is the entire point of this re-evaluation.

---

## B. The visual primitive

> **A stroke is a bounded, oriented BAND on the surface: a centreline with a width that
> tapers to nothing at both ends, a cross-section with a solid core and a feathered rim,
> a deposition amount, and internal bristle/grain structure that exists only inside it.**

Formally, for a surface point the renderer must be able to answer:

```
which stroke am I in?        -> strokeId  (a stable hash seed)
where along it am I?         -> s in [0,1] from tapered start to tapered end
how far across it am I?      -> t in [-1,1], 0 = centreline
how wide is it here?         -> w(s), tapered and jittered
```

and only then:

```
body      = profile(t, hardness) * taper(s)      <- THE PRIMARY STRUCTURE
deposit   = flow(strokeId) * pressure(s) * body
bristle   = fewLanesAcross(t) * slowAlong(s)     <- lives INSIDE the body
grain     = fine breakup, gated by body
relief    = height * (buildup + 4 * grooves)
colour    = pigment shifted per strokeId (broken colour)
```

If `body` is zero the pixel is not in a stroke and nothing else applies. That single rule is
what the failed implementation lacked.

---

## C. Generating that continuously on a 3D surface

The stability machinery from the previous design is kept unchanged and is not re-litigated:
integer-lattice hashing (the old `frac(p.x*p.y)` hash dies past ~500 m - measured), the
two-vector double-angle direction field (validated on floor / wall / ceiling / sphere /
cylinder / terrain), and the distance scale ladder. What changes is what is *built* on top
of those coordinates.

**The construction.** In the flow-aligned surface frame `(u, v)` already established:

1. **Lanes across.** Decompose `v` with a **1D cellular (Worley-style) partition** whose
   cell centres and half-widths are hashed per integer cell:
   `centre_j = j + hash(j)`, `halfWidth_j = base * (0.6 + 0.8*hash2(j))`. Take the nearest
   of the three neighbouring cells. This yields a lane id and a normalized across-coordinate
   - **aperiodic by construction**, because no two lanes share a width or a spacing. A
   plain `floor(v / w)` comb would be exactly the tiling the whole exercise is avoiding.
2. **Segments along.** Decompose `u` the same way, **offset by the lane's hash** so
   neighbouring lanes' stroke ends never line up. This gives the stroke id, `s`, and the
   segment length - i.e. strokes have **ends**, staggered like real brushwork.
3. **Width profile.** `w(s) = laneHalfWidth * max(smoothstep(0,ts,s) * smoothstep(0,te,1-s),
   0.04) * (1 + jitter)` - the editor's own taper code.
4. **Cross-section.** `body = pow(clamp(1 - (|t|-inner)/(1-inner)), softPow)` with
   `inner = hardness*0.55`, `softPow = mix(3.6,1.3,hardness)` - `MakeBrushTip`'s profile,
   evaluated analytically instead of sampled from a bitmap.
5. **Bristles inside the body**: ~3-4 lanes across `t`, slowly modulated along `s` (per A7).
6. **Grain / scatter**: fine breakup multiplied by `body`, so it can never appear outside a
   stroke.
7. **Layers.** Two lane sets at different scales and slightly different directions,
   composited - a block-in pass and a detail pass, which is how the ribbon authoring path is
   already used (coarse then fine).

**Why this is not dabs:** nothing is stamped. The band is a continuous analytic function of
the surface coordinate; it has no sample points and no spacing parameter.

**Why this is not a tiled texture:** lane widths, lane offsets, segment lengths and segment
phases are all hashed per cell, so the partition never repeats; and the domain warp
displaces `v` so lanes wander rather than running dead straight.

**Why this is not fullscreen noise:** noise appears only multiplied by `body`, and only in
stroke-local coordinates.

**Why it does not swim:** `(u,v)` derives from world position and the world flow field; the
ladder takes camera *distance* only. All of this is already proven by `--test-brushfield`.

**Known limitation to design around:** the flow frame is not integrable, so a band that is
straight in `(u,v)` is only approximately a streamline in world space, and can shear where
the flow turns sharply. The constraint that follows is concrete: **stroke length must stay
short relative to the flow field's turning radius** - i.e. `segmentLength << 1/flowScale`.
That is a tuning rule, not a free parameter.

**Eligibility, using signals the renderer already has** (no arbitrary darkening):

| pixel | signal | treatment |
|---|---|---|
| sky / background | `depth == 1.0` in `gInput2` | **no strokes at all.** There is no surface, no normal, and no stable world point; the previous build painting the sky was the clearest symptom of the field being applied blindly |
| dynamic-layer object | HDR alpha mask bit 0 (`HBE_MAT_PAINTERLY_EXEMPT`, `MeshPBR.hlsl:1052`) | already restored crisp by `PainterlyComposite`; strokes must not be computed there either |
| censored object | HDR alpha mask bit 1 (`HBE_MAT_CENSORED`) | painted even while dynamic, anchored to the censor origin so it rides with the entity |
| transparent surface | writes colour only - RT1/RT2 write-masked off (`D3D12Device.cpp:2246-2247`) | its G-buffer normal belongs to whatever is *behind* it, so the stroke frame would be wrong. Exclude |
| grazing surface | `|dot(N, V)|` | foreshortening compresses bands into thin lines; the ladder must use `d / max(|N.V|, k)` so strokes stay legible edge-on |

The grazing term is the one place this trades away a proven property: it makes the ladder
mildly camera-orientation dependent, so orbiting a grazing surface becomes *smooth* rather
than *bit-identical*. It belongs at the call site, not inside the field, so `B(P,N,d)`
stays a pure function and the existing test keeps its meaning.

---

## D. Division of responsibility

**`Painterly.hlsl` keeps exactly one job: colour-mass simplification.** The directional
line integral flattens the image into coherent regions of colour and value, and stops at
silhouettes. That is genuinely useful and genuinely a filter's job. It should **stop
pretending pixels are marks** - which means the `warp` term feeding its gather is no longer
the mechanism for making brush structure; at most it stays as a mild edge-raggedness on the
region boundaries.

**The stroke system owns everything mark-shaped**: body, direction, width, taper, edge,
deposition, bristle, grain, relief. It consumes the simplified colour and lays it down as
strokes.

Stated as a pipeline:

```
lit HDR -> line integral (colour masses, silhouette-aware)
        -> stroke field: body / deposition / bristle / relief
        -> composite (dynamic layer restored crisp)
        -> TAA
```

These stay separate. The failure being corrected was precisely the first stage being asked
to do the second stage's job with a noise multiply.

---

## E. Three candidate generators

**E1 - Analytic stroke bands from an irregular cellular partition** (section C).
*Cost:* two 1D cellular lookups (~3 hashes each), one analytic profile, one bristle noise,
one grain noise - materially **cheaper** than the delivered fBm, which spent 3 octaves x 2
ladder levels on structure that is now analytic.
*For:* it is the direct procedural transcription of Heartbreak's own stroke representation
(centreline + width + profile + taper); bodies and ends are explicit rather than hoped for;
aperiodic by construction; a pure function of `(P, N, d)` so every stability property
already proven carries over untouched; trivially maskable by eligibility.
*Against:* bands are locally parallel, so it can read as combing or hatching if the flow
field is too smooth - it needs per-lane direction jitter and a domain warp to feel gestural.
Bands are straight in `(u,v)`, so real curvature comes only from the frame rotating, with
the segment-length constraint noted above.

**E2 - Flow-field integral curves (true traced trajectories).**
Seed stroke start points on the surface, advect them along the flow field, and shade by
distance to the nearest curve.
*For:* genuinely curved strokes that follow form exactly - the most convincingly "painted"
of the three.
*Against:* per-pixel "distance to nearest streamline" has no closed form; it needs either
per-pixel iterative tracing (far too expensive) or a compute prepass writing a stroke
structure. Anchoring that structure stably in world space across frames is the same problem
the old `BrushStrokes` splat solved by becoming a quad-stamping system - which is the
outcome explicitly ruled out. High cost, high risk, and it re-opens a settled question.

**E3 - Sparse-impulse line integral convolution.**
Convolve a **sparse impulse** field along the flow direction. Each impulse smears into a
streak; impulse density sets stroke density, integration length sets stroke length. With
*sparse* input this is a stroke generator, not a blur - the distinction from ordinary LIC.
*For:* strokes are automatically curved and end naturally; it could reuse the existing
13-tap line-integral loop, so the marginal cost is small; it is the classic result in this
area.
*Against:* the impulses must be sampled in a *stable surface space*, which means
reconstructing a world position per tap - 13 unprojections per pixel, expensive and fragile
at silhouettes. Stroke **width is not directly controllable** (it falls out of impulse size
and filter kernel), and width/taper are exactly the parameters section A shows to be
central. Deposition and taper are similarly indirect.

**Comparison**

| | explicit body + ends | curvature | width/taper control | cost | fits proven stability machinery |
|---|---|---|---|---|---|
| **E1 bands** | **yes, by construction** | from frame rotation only | **direct** | **lowest** | **unchanged** |
| E2 curves | yes | best | direct | highest | needs a new stable structure |
| E3 sparse LIC | emergent | good | indirect | medium | needs per-tap world reconstruction |

---

## F. Recommendation

**E1.** It is the only one of the three whose primitive *is* the thing section A shows a
Heartbreak stroke to be - a centreline with a tapered width and a core/rim profile - and it
is the only one where "body first, noise inside" is structural rather than aspirational. It
is also cheaper than what was already built and measured, and it reuses the stability
foundation without modification, so the risk is concentrated entirely in the look.

E3 is the natural upgrade if E1's bands read as too parallel after tuning; it slots behind
the same interface. E2 should not be attempted unless both fail.

**Revised interface.** `{warp, coverage, height}` was too abstract - it described *effects*
rather than a stroke. Replace it with something the renderer can reason about:

```
StrokeSample S(P, N, d) -> {
    float  body;       // 0..1 stroke coverage: profile(t) * taper(s). 0 = not in a stroke
    float  deposit;    // 0..1 pigment laid down = flow(strokeId) * pressure(s)
    float  relief;     // -1..1 impasto, buildup + 4x grooves, gated by body
    float3 tint;       // per-stroke broken-colour shift (ApplyColorVar's concept)
    float2 alongAcross;// (s, t) for anything downstream that needs stroke coordinates
    float  confidence; // direction validity; fades toward round marks at flow vortices
}
```

`warp` survives only as a small edge-raggedness input to the filter, no longer as the
mechanism for making marks.

**The acceptance test is the user's, and it is visual, not statistical:** a wall must show
identifiable strokes with a body and a direction, with bristle variation *inside* them. The
existing `--test-brushfield` gates (hash integrity, non-repetition, direction smoothness,
ladder continuity, temporal stability) remain necessary and become **preconditions rather
than evidence** - the previous build passed all of them and still looked wrong. Two new
automatable checks are worth adding, because they measure the thing that actually failed:

- **Body occupancy**: the fraction of surface with `body > 0.5` should land in a target band
  (~0.45-0.75). Near 1.0 means the strokes have merged into a wash; near 0 means sparse
  specks.
- **Run-length along vs across**: the mean run of `body > 0.5` measured along the stroke
  direction must be several times its run measured across. A grain field scores ~1.0; real
  strokes score high. This single number distinguishes the failure from the goal.

## Open questions before implementation

1. **Should the line integral's direction come from the stroke field?** Unifying them would
   make colour masses and strokes agree, at the cost of the filter no longer following
   image structure. Worth an A/B once bodies exist.
2. **How many stroke layers?** One reads thin; three costs three evaluations. Two
   (block-in + detail) matches how the ribbon authoring path is already used.
3. **Does `deposit` reveal a substrate, or the crisp render?** The substrate reading was
   right in principle but was driven at the wrong frequency last time; with a real `body`
   the question is worth re-asking, since gaps *between* strokes now have meaning.
4. **Does the sky get any painterly treatment at all**, or stay smooth? Excluding it is
   correct for strokes; whether it wants a separate very broad treatment is a look call.

# Painterly Brush Field - Design

> **STATUS: PARTLY BUILT - see the implementation log below.**
> Produced 2026-08-26. Supersedes the screen-space procedural terms in
> `Shaders/Painterly.hlsl`. Read section 1 before anything else: it contains a measured
> result that invalidates the obvious "just move it to world space" fix.

> **IMPLEMENTATION LOG (2026-08-26).** Steps A-C of section 14 are BUILT and the
> headless gate (`--test-brushfield`) is green on both backends. Four things in the
> design below turned out to be wrong or incomplete when measured; each is corrected
> in place and flagged **[CORRECTED]**. Read those before trusting the surrounding
> prose. Step D (visual A/B) and Step E (GPU timing) need a real project and are
> not done.

**Verdict up front:** a **continuous procedural brush field** evaluated per pixel in a
**surface-local frame on a distance-driven scale ladder**, producing three outputs
(`warp`, `coverage`, `height`) that are fed *into* the painterly filter's own operators
rather than multiplied over its result. No stamps, no lattice `frac()`, no bitmaps, no
screen-locked coordinates. Estimated **0.6-1.0 ms at 1080p** on low-end discrete, with
quality knobs down to ~0.4 ms; partly self-funding because it lets the global
`BrushStrokes` splat (~2 M vertex-shader texture fetches/frame) be turned off.

---

## 1. The measured constraint that shapes everything

The existing hash - used identically in `Painterly.hlsl:39-43` and `BrushStrokes.hlsl:64-68` -

```hlsl
float Hash21(float2 p) {
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}
```

**collapses at world-scale coordinates.** Measured over a 64x64 lattice block in float32,
counting distinct outputs out of 4096:

| lattice origin | distinct values | mean | std |
|---|---|---|---|
| 0 | 2196 / 4096 | 0.503 | 0.285 |
| 100 | 1074 / 4096 | 0.502 | 0.289 |
| 1 000 | 943 / 4096 | 0.501 | 0.286 |
| 10 000 | **26 / 4096** | 0.455 | 0.285 |
| 100 000 | **1 / 4096** (all zero) | 0.000 | 0.000 |

A 5 cm brush lattice reaches cell index 10 000 at **500 m from the world origin** - well
inside any real level. Past that the field flattens to a constant and the paint texture
simply vanishes. Even at index 1 000 only 23% of values are distinct, which reads as
banding. The code already half-knows this: `D3D12Device.cpp:3536` wraps the boil phase to
256 with the comment *"the growing time can't blow up Hash21 (precision -> stripes)"*.

An integer-lattice hash (PCG3D-style mix on `int3`) holds at every magnitude tested - 3996-4029
distinct of 4096 from origin 0 through 10 000 000, axis cross-correlation -0.009.

**Consequence:** every world-space design below is predicated on replacing the hash. This is
not a nicety; it is the difference between "works" and "the effect disappears at 500 m".

---

## 2. What a "brush field" is, mathematically

A pure function of stable scene state, with no history, no screen coordinate and no time:

```
B : (P, N, d) -> { warp : float2, coverage : float, height : float }

  P        world position of the shaded surface point (unprojected from depth)
  N        world surface normal (G-buffer, OctDecode)
  d        camera distance (selects the scale ladder level only)

  warp     tangent-plane displacement, in world units - where the paint was DRAGGED FROM
  coverage 0..1 pigment deposition - 1 = loaded, 0 = bare ground (dry brush)
  height   -1..1 impasto micro-relief - its gradient lights the paint
```

`warp` / `coverage` / `height` are deliberately the same triple the Art Editor already
speaks: `paint::BrushTip` carries `alpha` (coverage) and `detail` (micro-relief), and
`Stamp()` applies them at a stroke direction. The field is that vocabulary made continuous.

The composition is multiplicative-in-structure, not multiplicative-in-colour:

```
coverage = load(P) * bristle(u,v) * breakup(u,v)
height   = ridge(u,v) * load(P)
warp     = W * (bristle-gradient rotated into the tangent plane)
```

where `load` is low-frequency paint deposition, `bristle` is the directional streak
structure, `breakup` is high-frequency grain/dry-brush, and `(u,v)` are brush-space
coordinates (section 4).

---

## 3. Brush direction - options and the choice

The direction field `D` defines the brush-space basis, so it is the single most
load-bearing decision. `D` must be a function of stable scene state only.

| Source | Stable? | Coherent? | Cost | Verdict |
|---|---|---|---|---|
| **Image structure tensor** (current, `Painterly.hlsl:103-113`, `BrushStrokes.hlsl:242-256`) | **No** - a function of shading; re-derived every frame; rotates as the camera orbits | yes | 9 fetches | **Reject as the basis.** This is the documented cause of the "speed-up" the censor path was patched around. |
| **Global axis projected into the tangent plane** (`D = norm(up - N*(N.up))`) | yes | too coherent - every wall vertical, every floor one way; mechanical | ~6 ALU | Reject as sole source; useful as a *fallback* in the degenerate branch. |
| **Tangent basis built from N, rotated by a scalar noise** | yes | yes | cheap | Reject: the hairy-ball theorem forces a singularity in any `T(N)`, and the natural placements (+Y or -Y) land on floors or ceilings - exactly the surfaces that are everywhere. |
| **Principal curvature from normal derivatives** | yes | genuinely follows form | 4+ extra G-buffer fetches, undefined on flat surfaces, noisy | Reject: cost and degeneracy on the flat surfaces that dominate a level. |
| **Low-frequency world flow noise, projected into the tangent plane** | **yes** - pure function of P | yes, and varies like a painter's hand rather than snapping to world axes | 1 three-channel 3D noise (section 12) | **Chosen.** |
| **Silhouette-aligned term** (perpendicular to the depth gradient) | yes given geometry+camera; view-dependent *by nature* - a silhouette is | very - painters do run strokes along contours | 2 depth fetches (already sampled) | **Adopt later, at low weight, confined to a thin band.** Not in the vertical slice. |

**Construction. [CORRECTED - the single-vector form below FAILED measurement.]**

The design proposed one flow vector:

```
F = flow3(P * kFlow);  D = normalize(cross(N, F));  A = cross(N, D)
```

`--test-brushfield` rejected it. On a surface LARGER than the flow wavelength the
degenerate set (F parallel to N) is codimension-2, i.e. isolated vortex points, which is
fine. But on an object SMALLER than the flow wavelength - a limb, a pipe, a trunk, a prop -
F is essentially constant while N sweeps 360 degrees, so the degenerate set becomes a whole
LINE running along the object. Measured on a 0.4 m cylinder at the 12.5 m default flow
wavelength: **18.0% of the surface below half confidence, 99th-percentile direction turn
41.8 deg per sample, peak 89.7 deg** - a visible seam wrapping every limb.

The fix is a SECOND ambient vector held strictly perpendicular to the first, so N cannot be
parallel to both, blended by how well-conditioned each is:

```
F = flow3(P * kFlow) - 0.5
G = cross(F, kAxis)          // G . F == 0 always
D = blend( cross(N,F), cross(N,G) )   weighted by sin(angle to N), squared
A = cross(N, D)
```

G costs one cross product and NO extra noise evaluation. **[CORRECTED again]** the blend
must happen in DOUBLE-ANGLE space, not as a vector average: this is a LINE field (a streak
reads the same along D and -D), and sign-aligning two vectors before averaging introduces a
hard flip wherever they sit near 90 degrees apart. That mistake took the flat floor from a
4.4 deg 99th-percentile turn to **69.1 deg**. Mapping angle -> 2*angle, averaging, and
halving (the same trick the existing painterly structure tensor uses) removes the ambiguity
instead of branching around it, and needs no atan2/sincos - half-angle formulas read
straight off the doubled vector.

Measured after both corrections, at a 12.5 m flow wavelength, 128x128 samples per surface:

| surface | turn p50 | turn p99 | >20 deg | low-confidence | mean confidence |
|---|---|---|---|---|---|
| flat floor | 0.31 deg | 1.99 deg | 0.00% | 0.0% | 1.000 |
| vertical wall | 0.08 deg | 1.94 deg | 0.00% | 0.0% | 1.000 |
| ceiling | 0.22 deg | 1.28 deg | 0.00% | 0.0% | 1.000 |
| sphere | 1.35 deg | 8.21 deg | 0.16% | 5.2% | 0.947 |
| cylinder (limb) | 0.06 deg | 1.77 deg | 0.00% | 0.0% | 1.000 |
| curved terrain | 1.31 deg | 28.10 deg | 1.96% | 8.5% | 0.918 |

The residual turns on the sphere and terrain are ISOLATED VORTICES, not noise, and the test
proves it by refinement: doubling the sampling density takes the >20 deg fraction to **x0.25
and x0.23** respectively (a noisy field would hold at x1.0). Such singularities are
unavoidable - the hairy-ball theorem guarantees them on a closed surface - and they are
absorbed by `confidence` fading the brush toward ISOTROPIC, so where the direction is
undefined the mark is simply round. No screen-space fallback is used anywhere.

**[CORRECTED] confidence must NOT include an agreement term.** `r`, the length of the
averaged doubled vector, looks like a coherence score, but the two candidates come from
deliberately perpendicular sources and so disagree almost everywhere by design. Treating
that as low confidence faded **75.2% of a flat floor** to round marks for no reason. The
agreement term is kept but narrowed to catch only the genuine singularity (both candidates
90 deg apart AND equally weighted, so the doubled vector cancels).

`cross(N, F)` degenerates only where `F` is parallel to `N`. Because `F` varies through space,
that set is a thin surface in the world rather than being pinned to a particular normal
direction, so it sweeps across geometry instead of parking on every floor. It is handled by
blending toward the global-axis fallback with `smoothstep(0.05, 0.20, length(cross(N,F)))` -
one smoothstep, no branch. The brush field is symmetric under `D -> -D` (a bristle streak
reads the same either way), which halves the visible severity of any residual instability.

**Why not the image gradient at all?** It is not merely unstable - it is *shading*, and
shading changes when a light moves or an object's albedo changes, so the paint texture would
re-lay itself when nothing about the surface moved. The user-visible contract we want is
"the same surface location produces the same brush structure unless the surface itself
changes"; a shading-derived direction violates it by definition.

---

## 4. Coordinate space - surface-local, not planar, not screen

```
u = dot(P, D) + dot(P, N) * kDecorr
v = dot(P, A)
```

Three things to note, because two of them are the difference between this working and not:

**4.1 - It is not a planar projection.** The stated risk of `u = dot(P,D), v = dot(P,A)` is
stretching on surfaces parallel to the projection direction. That cannot occur here because
`D` and `A` are constructed **inside the tangent plane** (`D` perpendicular to `N`,
`A = N x D`): the projection is always onto the surface's own plane, so the metric is
isometric to first order and there is no stretch anywhere. Curvature makes `(u,v)`
non-integrable - the frame shears slowly over a curved surface - but noise is scale-free, so
shear reads as variation rather than as an artifact.

**4.2 - The `dot(P, N)` term is the anti-correlation trick.** Without it, two parallel
surfaces (two floors of a building, opposite sides of a corridor) project to the same
`(u,v)` and receive an identical brush pattern. `dot(P, N)` is the plane offset: constant
within one flat surface (so it does not disturb that surface's own field) and different
between parallel planes (so their fields decorrelate). **Cost: one dot product.** It is the
cheapest possible substitute for going fully 3D.

**4.3 - Sky.** `depth == 1` has no `P`. Use the view-ray direction as the field coordinate,
`dir = normalize(WorldFromDepthPost(uv, 0.999) - gCameraPosWS)`, evaluated on the unit
sphere. Stable under camera rotation *and* translation, because the sky is at infinity.
`N` is set to `-dir` so the tangent frame is well defined.

---

## 5. The scale ladder - the crux of the whole design

There is a genuine conflict here and it has to be named:

- **Fixed world-space frequency** -> perfect adhesion to surfaces (zero swim), but apparent
  mark size scales with 1/distance: boulder-sized marks up close, sub-pixel aliasing mush at
  range. It is also artistically wrong - a painter's brush is a constant size *on the canvas*,
  not in the scene.
- **Fixed screen-space frequency** -> correct constant mark size, and it swims. This is the
  current system.

**[CORRECTED] This is stability, not magic constant size.** A world-space scale ladder is
NOT mathematically constant screen-space brush size - perspective still varies apparent size
*within* a level, and the ladder only keeps the active feature scale within about one octave
of the target. What it does guarantee is world anchoring, no screen-space swimming, graceful
transitions, and no aliasing blow-up at distance. It should be described that way.

**[CORRECTED] Distance alone is not enough: FOV had to be folded in.** Apparent size goes as
1/(d * tan(fovY/2)), so a LENS zoom changes it without changing distance and marks would
balloon when zoomed. The backends therefore scale the ladder's reference distance by
`kRefTanHalfFov / tan(fovY/2)` (taken from `proj[1][1]`), so the level tracks dolly and zoom
alike. No shader change - it rides in on `refDist`.

**Resolution: quantise scale, not space.** Pick a level from distance,

```
L      = log2(d / dRef) + bias
k      = floor(L)
t      = frac(L)                      // smooth, monotone in d
field  = lerp( Field(P, 2^k), Field(P, 2^(k+1)), smoothstep(t) )
```

Each level is fully world-locked, hence individually stable; the blend weight `t` is a smooth
monotone function of camera distance only. So at any instant the image is a blend of two
world-locked fields, apparent mark size stays within one octave of constant, and **nothing
swims** - because neither operand moves relative to the surface and the weight changes only
when the camera dollies. This is the standard "infinite zoom" ladder from stylized NPR; it
is a ladder in *scale*, not a tiling in *space*, so it introduces no periodicity.

Two useful side effects:

- **It self-compensates for depth precision.** The projection is `glm::perspectiveRH_ZO`
  (`Source/Renderer/Camera.h:39`) - plain `[0,1]` depth, **not** reversed-Z, so world position
  reconstructed from depth gets noisier with distance. But the lattice cell at level `k` is
  itself proportional to distance, so the *error expressed in field coordinates* stays roughly
  constant instead of growing.
- Rotation and zoom are handled for free: rotation does not change `d`, so the level does not
  change; zoom changes `d` smoothly, so the blend moves smoothly.

`bias` is the artist control that means "brush size", and it is the natural home for
`BrushDef::size`.

---

## 6. How repetition is prevented

Four independent mechanisms, and an automated acceptance test.

1. **Aperiodic lattice noise with an integer hash.** Gradient/value noise over a hashed
   integer lattice has *no* period - unlike `sin`/`frac`, which is the whole objection to the
   current terms. Requires section 1's hash replacement to actually be true in the far field.
2. **Incommensurate octave ratios + per-octave domain rotation.** Ratios `1.0, 2.017, 4.073`
   (not exactly 2) with rotations `0, 0.739, 1.556 rad`, so octaves never re-align into the
   grid-flavoured look that exact-doubling fBm produces.
3. **Domain warp.** The low-frequency flow noise `F` already computed for the direction is
   reused to displace the brush coordinates before the bristle octaves - no extra evaluation.
   This is what decorrelates structure over large distances and supplies the hand-made wobble.
4. **The `dot(P, N)` decorrelation term** of 4.2, killing parallel-surface correlation.

**Acceptance test (automatable, headless, CPU mirror of the shader).** Sample the field along
a 1D world line and require its autocorrelation to decay and stay decayed. Prototype result:

| field | autocorrelation past the smoothness shoulder |
|---|---|
| candidate world brush field (3 octaves, incommensurate + rotated) | +0.18 at lag 8, then **-0.05 / -0.00 / +0.01 / +0.10** at lags 16/32/64/128 - decays and never recovers |
| current stroke-lattice term `sin(2*pi*frac(x/cell))` | **+1.000, recurring at every period** |

Proposed gate: `abs(rho(lag)) < 0.25` for every `lag > 4x` the correlation length. The current
term fails at 1.000; the candidate passes.

---

## 7. Bristles, paint density, edge breakup

**Bristles** are anisotropy, not a texture: high frequency *across* the stroke, very low
frequency *along* it. In brush space that is simply an anisotropic sample of the same fBm:

```
bristle = fbm2( u * 0.12, v * 1.0 )        // ~8:1 elongation along D
```

Three things stop it reading as ruled lines: (a) amplitude modulated by an independent
low-frequency term along `u`, so streaks fade in and out instead of running forever
(dry-brush drop-out); (b) the domain warp of 6.3, so streaks wander; (c) hash-varied lateral
phase, so streak spacing is irregular rather than a comb.

**Paint density (`load`)** is a low-frequency scalar over `P` - "here the brush was loaded,
here it was running dry". It reuses a channel of `F`, so it costs nothing extra. It gates
both `coverage` and `height`, which is what couples "thin paint" to "no impasto" the way real
paint does.

**Edge breakup** has two meanings and both are served by `warp`, not by a multiply:

- *Region edges* - where one colour mass meets another. `warp` displaces the painterly
  filter's gather origin, so the boundary between masses becomes ragged in a brush-shaped way.
  The boundary is then genuinely brush-cut, not a straight edge with noise painted over it.
- *Silhouettes* - already handled by the existing `edgeFrac` fallback
  (`Painterly.hlsl:346-353`), which stays as-is.

---

## 8. Where the field enters the pipeline

This is the part that distinguishes the proposal from the thing being replaced. The current
code ends with `col *= 1 + ValueNoise(screenUV)*k`. The field instead drives the painterly
*operators*:

| Output | Enters at | Effect |
|---|---|---|
| `warp` | the line-integral's sample origin and tangent (`Painterly.hlsl:188-215`) - replacing the `frac`/`sin` lattice pull outright | the smoothing operator itself becomes brush-shaped; colour masses acquire dragged, ragged boundaries |
| `coverage` | lerp between the painted mass and a ground/underpainting colour | dry-brush drop-out reveals what is beneath - a lerp, not a darkening |
| `height` | `N' = perturb(N, grad(height))`, then `dot(N', gLightDirWS) * gLightColor` | impasto ridges that **catch the scene's actual light** and change as the sun moves |

`grad(height)` is free: `float2(ddx(h), ddy(h))` - hardware derivatives of a world-stable
scalar are themselves stable, at the cost of 2x2-quad granularity, which is standard and
invisible at these frequencies. `gLightDirWS` / `gLightColor` / `gCameraPosWS` / `gInvViewProj`
are all already visible to post passes via `Common.hlsli` (b0).

The `height`-lights-the-paint path is the direct descendant of `MakeBrushTip`'s `detail`
channel, whose own comment says it exists so "a mark reads as real oil paint under PBR
lighting, not a flat decal" (`Source/Scene/PaintSystem.h:154-159`).

---

## 9. Fate of the existing three passes

**`Painterly.hlsl` - keep, minus four terms.** The directional line integral is worth keeping:
13 taps, O(R), and it is what produces coherent colour/value masses - the "simplify the image"
job that a brush field cannot do for itself. Deleted: the `frac`/`sin` stroke lattice
(:188-191), the bristle sawtooth (:227), the marks overlay (:361-375), the canvas weave
(:377-381). Their *function* is replaced (warp, bristle, breakup, grain respectively), not
merely removed - the image must not end up smooth and empty.

The image-gradient structure tensor (:96-115) stays for now: it steers the line integral's
gather, which is a *filter* decision where instability is far less visible than in the brush
structure. Whether to also drive it from the stable flow field is a follow-up, measured
separately.

**`BrushStrokes.hlsl` - keep, narrowed.** It stops being the global look (that is the field's
job now) and remains the **Painterly Censor** implementation - a shipped feature with an ECS
component, editor UI (`Source/Editor/Editor.cpp:10406-10422`) and a world-anchored sphere test.
Turning the global path off reclaims most of its ~2 M VS texture fetches per frame, which
part-funds the field. Longer term the censor can be re-expressed as a masked region of the
field pass and the splat retired; that is a later, separately reviewable step, not part of
this one.

**`PainterlyComposite.hlsl` - unchanged.** The static/dynamic split and the censor sphere test
are orthogonal to how paint texture is generated.

**`StrokeSurface.hlsl` / `StrokeGen.cpp` - unchanged and still dead.** `painterly3D` is pinned
off at `Source/RHI/RHI.h:404`. Out of scope; flagged for a later cleanup decision.

---

## 10. What `MakeBrushTip` and `BrushDef` become

Not stamped. `MakeBrushTip` stays exactly where it is, doing exactly what it does, for the Art
Editor's CPU stamping path - that is a genuine dab renderer and correctly so, because hand
painting *is* stamping.

What crosses over is the **parameter vocabulary and the two-channel model**:

| `BrushDef` field | Field parameter |
|---|---|
| `hardness` | breakup contrast / coverage gamma |
| `bristles` | bristle octave amplitude + anisotropy ratio |
| `grain` | high-frequency breakup amplitude |
| `scatter` | coverage hole threshold |
| `size` | scale-ladder `bias` |
| `BrushTip::alpha` (concept) | `coverage` |
| `BrushTip::detail` (concept) | `height` |

So a painterly look is authored with the same words as an Art Editor brush, and the existing
`DefaultBrushes()` presets ("Bristle", "Chalk", "Oil Flat", "Spray") map onto field presets
directly. That is reuse of the *description* of a brush, which is the part worth reusing;
the 64x64 bitmap it bakes is the part that must not be tiled.

---

## 11. Temporal stability - the argument, term by term

| Term | Depends on | Stable under camera move / rotate / zoom? | Under object motion? |
|---|---|---|---|
| `P` | depth + `gInvViewProj` | yes for static geometry | **no** - see below |
| `N` | G-buffer | yes | rotates with the object, correctly |
| `F`, `D`, `A` | `P` only | yes | follows `P` |
| `(u,v)` | `P`, `N`, `D`, `A` | yes | follows `P` |
| ladder level `k`, blend `t` | distance only | yes - smooth, monotone, no per-frame term | n/a |
| `grad(height)` | `ddx/ddy` of a stable scalar | yes | yes |

No term reads screen UV, frame index, time, or `gViewProj`. Nothing is accumulated between
frames.

**The one honest gap: moving objects.** World-anchored paint slides across a surface that is
itself moving through the world. The existing architecture already answers this and the answer
is kept: dynamic-layer objects are composited back **crisp** by `PainterlyComposite`, and the
one case that must stay painted while moving - a Painterly Censor - carries an explicit world
origin that rides with the entity, so the field can be evaluated in that object's frame
(`P - censorOrigin`). Anything outside those two cases would need an object ID, and there is
no room: the G-buffer is full (`rg` = octahedral normal, `b` = roughness, `a` = metallic,
`Shaders/MeshPBR.hlsl:1074`) and the HDR alpha already carries both mask bits. Adding a fourth
render target for object IDs is a real cost and a separate decision - out of scope here, and
named so it is not discovered later as a surprise.

**TAA becomes an ally.** It runs *after* painterly (`Source/RHI/D3D12/D3D12Device.cpp:3711`,
10% current / 90% reprojected history). A screen-locked pattern gets dragged along geometry
velocity and re-blended against itself every frame - that is a large part of what currently
reads as scrolling and smearing. A world-anchored field reprojects *correctly*, so TAA
denoises the brush structure instead of fighting it.

---

## 12. Cost

> **[CORRECTED - MEASURED.]** The per-pixel ALU estimate in the table below was a
> hypothesis and it was optimistic. Measured statically with DXC by pinning the path at
> compile time (`-D HBE_PAINTERLY_FORCE_MODE=0|1`), since a runtime branch compiles both
> paths into one shader and hides the delta:
>
> | Painterly PS configuration | DXIL instructions | texture samples |
> |---|---|---|
> | legacy screen-space terms only | 928 | 37 |
> | **brush field, full quality (2 ladder levels, 4 octaves compiled)** | **3110** | **38** |
> | brush field, ladder pinned to 1 level | 2332 | 38 |
> | brush field, 1 level + 2 bristle octaves | 2085 | 38 |
> | brush field, 1 level + 1 bristle octave | 1974 | 38 |
>
> So the field adds **~2180 static instructions and exactly ONE texture fetch** (the
> G-buffer normal, as designed). The ladder is ~25% of the field's cost, not the 40% the
> estimate below claimed. DXIL instruction count is not ALU-op count - it is SSA form and
> the driver optimises further - and these are STATIC counts including untaken runtime
> branches, so they bound the shape of the cost, not its wall-clock. **Real timing needs a
> real scene (`--benchmark`) on the target hardware and has NOT been done.** Do not quote
> the sub-millisecond figures below as fact until it has.
>
> The SPIR-V unroll check the repo requires for this pass passes: zero
> `OpVariable %_ptr_Function` in `Painterly.ps.spv`, so the octave loop's indexed constant
> arrays stay in registers on Vulkan rather than spilling to scratch.

### Original estimate (hypothesis, kept for comparison)

Per pixel, for the recommended configuration (2 ladder levels; 3 bristle octaves; 1 grain
octave; PCG3D hash ~12 ALU, emitting three channels per evaluation so the direction field's
three components share one hash per lattice corner):

| Stage | Texture | ALU (approx) |
|---|---|---|
| G-buffer fetch + `OctDecode` | 1 | 12 |
| `WorldFromDepthPost` (depth already sampled) | 0 | 20 |
| ladder select (`log2`/`floor`/`frac`) | 0 | 6 |
| `F` - 3D 3-channel value noise, 8 corners | 0 | 132 |
| brush coords `(u,v)` incl. decorrelation | 0 | 9 |
| bristle fBm, 3 octaves x 2 ladder levels | 0 | 220 |
| grain, 1 octave x 2 levels | 0 | 110 |
| `load`, reusing `F` + 1 octave | 0 | 55 |
| `grad(height)` via `ddx/ddy`, normal perturb, light | 0 | ~40 |
| **total** | **1 extra** | **~600** |

~1.25 G ALU-ops at 1080p. Rough landing: **~0.7-1.0 ms** on low-end discrete (GTX 1650 class),
**~1.5-2.0 ms** on integrated (Iris Xe class) - the latter needs the knobs.

Quality knobs, in the order they should be spent:

| Knob | Saving | Cost to the look |
|---|---|---|
| ladder -> 1 level | -40% | mark size drifts within an octave; visible only on long dollies |
| bristle 3 -> 2 octaves | -18% | slightly coarser streaks |
| `F` from a **half-res prepass** (it is low-frequency by construction, so half-res is near-lossless) | -20% | needs one extra target + bindless slot; bleeds slightly across silhouettes |

Combined: ~0.4 ms on integrated. The half-res `F` prepass is deliberately **not** in the first
slice - *profile before introducing complexity*.

**Offsetting reclaim:** turning the global `BrushStrokes` splat off removes ~20 000 instances
x 6 vertices x ~16 texture fetches = **~2 M VS fetches/frame**. Net frame cost may be near zero
or negative. Both numbers must be measured on a real scene before this claim is repeated as
fact - the repo contains no project, so the measurement in section 14 step 6 needs the user's.

---

## 13. Three approaches compared

**A - Volumetric warped fBm.** Evaluate 3D noise directly at a world position stretched along
the flow direction, `q = P + D*dot(P,D)*(s-1)`, single world scale.
*For:* trivially correct - genuinely 3D, so zero planar correlation and zero seams on curved
or intersecting geometry; simplest to reason about. *Against:* 3D noise is 8 corners/octave
against 2D's 4, so ~2x the ALU for the same octave count; and it has **no answer for apparent
mark size** - marks are boulder-sized near and alias to mush far. That second problem is
fatal on its own.

**B - Surface-frame anisotropic fBm on a distance scale ladder.** Sections 3-5.
*For:* constant apparent mark size *and* zero swim, which is the pair nothing else delivers;
2D noise in the tangent plane is half the cost of 3D; the ladder self-compensates for the
non-reversed-Z depth precision; the `dot(P,N)` term buys back the decorrelation that going 2D
gave up, for one dot product. *Against:* `(u,v)` is non-integrable on curved surfaces (shear -
benign for noise); needs the degeneracy blend in `cross(N,F)`; two ladder levels mean the
structure layers are evaluated twice.

**C - Anisotropic Gabor / sparse convolution noise.** Sum randomly-placed, randomly-phased
anisotropic Gabor kernels over the neighbouring cells of a Poisson-ish lattice.
*For:* the principled answer - it is literally the mathematics of "a field of overlapping
brush marks" with no stamping, provably aperiodic, and **band-limited**, so it can be
antialiased exactly by narrowing the kernel bandwidth with distance (no ladder needed, no
mip guessing). Orientation is a per-kernel parameter, so a per-pixel direction field drops
straight in. *Against:* ~32-64 kernel evaluations per pixel for a smooth result - roughly
5-10x approach B - which is out of budget for the stated low-end target. Also the highest
implementation risk of the three (kernel density vs. variance tuning is fiddly).

**Recommendation: B.** A is disqualified by apparent mark size; C is disqualified by cost on
the stated hardware target, not by quality. B's weaknesses are all bounded and each has a
cheap named mitigation. If the look later needs C's antialiasing quality on hero shots, the
band-limited kernel can be added as a high-quality mode *behind the same field interface* -
B's three-output contract does not have to change.

---

## 13b. What was actually built (2026-08-26)

| Item | Where | State |
|---|---|---|
| Integer PCG3D hash, 2D/3D value noise, incommensurate rotated fBm | `Shaders/BrushField.hlsli` | done |
| Two-vector direction field + double-angle blend + confidence | same | done, corrected twice |
| Scale ladder + `BfEval` entry point + `BfParams` / `BfSample` | same | done |
| Compiled as C++ AND as HLSL from one file | same (`__cplusplus` shim) | done |
| Headless gate: hash, non-repetition, direction on 6 surfaces, ladder, temporal, determinism | `Source/Renderer/BrushFieldTest.cpp`, `--test-brushfield` | green |
| Four screen-space terms removed from the global path | `Shaders/Painterly.hlsl` | done |
| `warp` -> line-integral gather; `coverage` -> ground; `height` -> impasto light | same | done |
| A/B switch (`painterlyBrushMode`), legacy path retained | `PostSettings` + shader branch | done |
| Compile-time path pin for cost measurement / eventual dead-code removal | `HBE_PAINTERLY_FORCE_MODE` | done |
| Constants + settings + serialization + editor panel, both backends in lockstep | D3D12 + Vulkan + `Editor.cpp` | done |
| FOV compensation in the ladder | both backends | done |

Two things the design got wrong that the code now reflects:

- **The ground tone must be driven by the field's LOW-frequency channel, not by
  `coverage`.** Coverage carries the bristle structure, so modulating a substrate by it
  makes the ground flicker along with the bristles - which is a texture, not a ground.
  `BfSample::load` (wavelength ~1/flowScale, tens of metres) was added and exposed for
  exactly this.
- **`BfNoise2` was discarding two thirds of its own hash.** `BfHash3` yields three
  decorrelated words per lattice corner; taking only `.x` meant the domain warp would have
  cost a second full evaluation. `BfNoise2v` returns all three, so the warp vector and the
  dry-brush modulator are free.

Not done, and needing a real project: **Step D** (visual A/B over static camera /
translation / rotation / orbit / zoom / object motion / animation / TAA on-off / multiple
resolutions) and **Step E** (GPU timing on low-end discrete and integrated). **Step F**
(retiring the legacy path and the global `BrushStrokes` splat) is deliberately blocked on
those.

---

## 14. Proposed implementation order (Phase 3), not yet started

Each step builds, runs, and is independently reviewable.

1. **`Shaders/BrushField.hlsli`** - integer PCG3D hash, 2D/3D value noise, fBm with
   incommensurate rotated octaves, the `BrushField()` entry returning the three outputs. No
   call sites yet. Ship with a CPU mirror in `Source/Renderer/BrushField.h` used only by the
   test.
2. **`--test-brushfield`** - headless: hash uniformity at world magnitudes (section 1 table as
   the gate), autocorrelation decay (section 6 gate), determinism, and the ladder blend's
   continuity across a level boundary. No GPU.
3. **Vertical slice in `Painterly.hlsl`** - one brush "type": delete the four screen-space
   terms; wire `warp` into the line integral; `coverage` into a lerp to the underpainting;
   `height` into the light. One new `float4` appended to `PostCB`/`PostUBO`/`PostCommon.hlsli`
   (the established append convention), plus a `painterlyBrush*` group in `PostSettings`,
   serialization, and the editor panel.
4. **Both backends in lockstep**, D3D12 and Vulkan, in the same commit.
5. **Turn the global `BrushStrokes` splat off by default**; keep it for censors.
6. **Measure** on the user's real scene (`--benchmark`), then decide on the half-res `F`
   prepass and octave counts.
7. Only then generalize: the `BrushDef` -> field-parameter mapping of section 10, and the
   silhouette direction term of section 3.

## 15. Open questions for review

- **Underpainting colour for `coverage`.** Lerp toward the crisp original (keeps detail,
  reads as "thin paint"), or toward a low-frequency ground tone (reads as primed canvas
  showing through)? The second is more painterly and needs one extra low-frequency term.
- **Should the line integral's orientation also come from the stable flow field?** It would
  unify direction across filter and brush at the cost of losing "strokes follow the image's
  forms". Recommend measuring both, not deciding up front.
- **Retire `BrushStrokes.hlsl` entirely** once censors are folded into the field pass - yes or
  keep both paths?
- **Object IDs.** Accept "dynamic objects are crisp unless censored" permanently, or budget a
  4th render target later so the field can be evaluated in object space everywhere?

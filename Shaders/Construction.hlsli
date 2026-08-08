// Construction.hlsli - procedural surface for construction materials.
//
// WHY THIS EXISTS. The procedural construction system generates real geometry - individual bricks,
// studs, boards, shingles - but every piece was shaded with one flat base colour, so a brick wall
// read as a pink slab with faint seams and an oak beam read as a brown box. The geometry was
// right and the SURFACE was missing.
//
// This is a real procedural shader, not a texture lookup: brick faces vary in tone, mortar sits in
// its own joints, wood has grain and knots, concrete has aggregate and staining. It costs no
// texture memory, no UV unwrap and no artist time, and it tiles at any wall size because it is
// driven by WORLD POSITION rather than by mesh UVs.
//
// COSTS NOTHING IN THE DRAW BUDGET. The material id is packed into the spare high bits of the
// existing `flags` word rather than added as a new ObjectCB field. ObjectCB is 528 bytes, aligned
// to 768, against a 4 MB per-frame arena - roughly 5,400 draws - so growing it would DIRECTLY cut
// the number of things this renderer can draw. See docs/Design-ProceduralConstruction.md SS2.2.
#ifndef HBE_CONSTRUCTION_HLSLI
#define HBE_CONSTRUCTION_HLSLI

// Packed into gMaterialFlags bits 16-23. 0 = not a construction surface.
#define HBE_PROC_SHIFT 16u
#define HBE_PROC_MASK  0xFFu

#define HBE_PROC_NONE      0u
#define HBE_PROC_BRICK     1u
#define HBE_PROC_STONE     2u
#define HBE_PROC_CONCRETE  3u
#define HBE_PROC_WOOD      4u
#define HBE_PROC_PLASTER   5u
#define HBE_PROC_METAL     6u
#define HBE_PROC_SHINGLE   7u
#define HBE_PROC_MORTAR    8u

// --- Noise ------------------------------------------------------------------
// Value noise on a hash. Cheap, stable, and - critically - a pure function of world position, so
// two walls meeting at a corner agree on their grain instead of showing a seam.
float HbHash1(float2 p) {
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}
float HbHash1(float3 p) {
    return frac(sin(dot(p, float3(127.1f, 311.7f, 74.7f))) * 43758.5453f);
}

float HbValueNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0f - 2.0f * f); // smoothstep: no visible lattice
    float a = HbHash1(i), b = HbHash1(i + float2(1, 0));
    float c = HbHash1(i + float2(0, 1)), d = HbHash1(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float HbFbm(float2 p, int octaves) {
    float sum = 0.0f, amp = 0.5f;
    [unroll(4)]
    for (int i = 0; i < octaves; ++i) {
        sum += HbValueNoise(p) * amp;
        p *= 2.03f; // deliberately not exactly 2, which would align the octaves' lattices
        amp *= 0.5f;
    }
    return sum;
}

// THE PER-PIECE KEY. This is what makes the difference between "a noise function on a wall" and
// "every brick is its own brick".
//
// The generator emits each unit as its own box and writes WORLD-SCALE UVs measured in metres from
// that face's origin corner. So the face's origin can be recovered exactly:
//
//     origin = posWS - (u * tangent + v * bitangent)
//
// That value is constant across one brick's face and different for every other brick, which is a
// perfect per-piece hash key - derived from geometry the generator already produces, needing no
// extra vertex channel and no ObjectCB space (there is room for neither).
//
// Guessing a cell grid instead - which is what this did first - produces tone bands that do not
// line up with the actual units, because the shader has no idea what course height or unit length
// the generator chose. That is exactly what the banding in a brick wall was.
float3 HbPieceKey(float3 posWS, float3 n, float3 tangentWS, float2 uv) {
    const float3 T = normalize(tangentWS);
    const float3 B = normalize(cross(normalize(n), T));
    return posWS - (uv.x * T + uv.y * B);
}
float HbPieceRand(float3 key, float salt) {
    return HbHash1(floor(key * 97.0f) + salt);
}

// Projects world position onto the plane most facing the surface normal, so a pattern runs
// correctly across a wall, a floor and a soffit without a per-face UV set.
float2 HbTriplanarUV(float3 posWS, float3 n) {
    float3 a = abs(n);
    if (a.y > a.x && a.y > a.z) return posWS.xz; // floor / ceiling
    if (a.x > a.z) return float3(posWS.zy, 0).xy; // wall facing X
    return posWS.xy;                              // wall facing Z
}

// --- Materials --------------------------------------------------------------

// Per-brick tonal variation. The GEOMETRY already provides the courses and joints, so this must
// NOT redraw them - it varies each unit's face and roughens it, which is what a real brick wall
// does and what a single flat colour cannot.
// FULL BRICK PATTERN ON A FLAT SURFACE - courses, joints, bond offset and all.
//
// THIS IS THE TLOU-STYLE APPROACH, and it is the right default. Naughty Dog do not model
// individual bricks: a wall is a simple modular panel carrying a tiling brick MATERIAL, with
// uniqueness added by decals and vertex paint on top. The geometry stays trivial and every bit of
// the detail lives in the shader.
//
// The difference is not cosmetic, it is three orders of magnitude. An 8x3 m wall as real brick
// geometry is ~1,480 boxes and ~35,000 vertices; the same wall as one box with this material is 24
// vertices and ONE draw. On a renderer capped near 5,400 draw items a frame, that is the
// difference between a street and a single building.
//
// Per-brick geometry still exists and still has a use - it is what lets a wall actually LOSE a
// brick - so it stays available as a detail mode. It is just no longer the default.
float3 HbBrickPattern(float3 base, float3 posWS, float3 n, inout float rough, out float relief) {
    // Standard metric brick: 215 x 65 with a 10 mm joint, so a course is 75 mm and the stretcher
    // stride is 225 mm.
    const float courseH = 0.075f;
    const float strideL = 0.225f;
    const float jointW = 0.010f;

    const float2 uv = HbTriplanarUV(posWS, n);
    const float row = floor(uv.y / courseH);
    // RUNNING BOND: every other course offset by half a brick. Without this the wall reads as a
    // grid of tiles rather than as masonry, which is the single most obvious tell.
    const float offset = (fmod(abs(row), 2.0f) < 0.5f) ? 0.0f : strideL * 0.5f;
    const float col = floor((uv.x + offset) / strideL);

    // Position within this brick, in metres.
    const float inY = uv.y - row * courseH;
    const float inX = (uv.x + offset) - col * strideL;

    // The joints. Softened by one pixel of derivative so they antialias instead of shimmering into
    // a moire pattern at distance - a hard step here is what makes tiled brick crawl when the
    // camera moves.
    const float2 fw = max(fwidth(uv), 1e-5f);
    const float bedTop = smoothstep(0.0f, fw.y * 1.5f, inY - jointW);
    const float bedBot = smoothstep(0.0f, fw.y * 1.5f, (courseH - inY) - jointW * 0.0f);
    const float perpL = smoothstep(0.0f, fw.x * 1.5f, inX - jointW);
    const float perpR = smoothstep(0.0f, fw.x * 1.5f, (strideL - inX));
    const float face = saturate(min(min(bedTop, bedBot), min(perpL, perpR)));

    // One tone per brick, from its own (row, col) - stable, and it never bleeds across a joint.
    const float2 cell = float2(col, row);
    const float tone = HbHash1(cell);
    float3 brick = base;
    brick *= 0.78f + tone * 0.46f;
    brick.r *= 0.94f + HbHash1(cell + 17.0f) * 0.16f;
    brick.b *= 0.88f + HbHash1(cell + 41.0f) * 0.24f;

    // Surface grain, offset per brick so it does not run continuously across the wall.
    const float grain = HbFbm((uv + cell * 3.7f) * 120.0f, 3);
    brick *= 0.90f + grain * 0.16f;
    // A worn margin at each brick's arris.
    brick = lerp(brick * 0.78f, brick, saturate(face * 3.0f));

    // Mortar: pale, chalky, and dirtier than the brick.
    const float mgrain = HbFbm(uv * 150.0f, 2);
    float3 mortar = float3(0.74f, 0.72f, 0.68f) * (0.84f + mgrain * 0.26f);
    mortar *= 0.82f + HbFbm(uv * 8.0f, 3) * 0.30f;

    relief = face; // 1 on a brick face, 0 in a joint - the caller uses it to fake the recess
    rough = saturate(lerp(0.94f, 0.82f + grain * 0.14f, face));
    return lerp(mortar, brick, face);
}

float3 HbBrick(float3 base, float3 posWS, float3 n, float3 tangentWS, float2 meshUV,
               inout float rough) {
    // ONE TONE PER ACTUAL BRICK, from the piece key - not from a guessed grid.
    const float3 key = HbPieceKey(posWS, n, tangentWS, meshUV);
    const float tone = HbPieceRand(key, 0.0f);

    // Fired clay varies a lot brick to brick: some run dark and purple, some pale and sandy.
    float3 c = base;
    c *= 0.80f + tone * 0.42f;
    c.r *= 0.95f + HbPieceRand(key, 17.0f) * 0.14f;
    c.b *= 0.90f + HbPieceRand(key, 41.0f) * 0.22f;

    // Fine surface grain, keyed to the piece so it does not swim across the joints.
    const float2 local = meshUV;
    const float grain = HbFbm((local + key.xz) * 110.0f, 3);
    c *= 0.90f + grain * 0.16f;

    // A darker, worn margin around each face. Real brick is never uniform right to its arris, and
    // this is what stops a wall of flat rectangles reading as printed-on.
    const float2 fw = fwidth(local) * 2.0f;
    const float edge = saturate(min(min(local.x / max(fw.x, 1e-4f), local.y / max(fw.y, 1e-4f)),
                                    4.0f) * 0.25f);
    c = lerp(c * 0.72f, c, edge);

    rough = saturate(0.82f + grain * 0.16f);
    return c;
}

// Lime mortar: pale, chalky, and slightly different in every joint.
float3 HbMortar(float3 base, float3 posWS, float3 n, inout float rough) {
    const float2 uv = HbTriplanarUV(posWS, n);
    const float grain = HbFbm(uv * 130.0f, 3);
    const float dirt = HbFbm(uv * 9.0f, 3);
    float3 c = base * (0.86f + grain * 0.24f);
    c = lerp(c, c * 0.66f, dirt * 0.5f); // grime collects in the joints first
    rough = saturate(0.90f + grain * 0.10f);
    return c;
}

// Rubble stone: large irregular cells, strong tone variation, heavy grain.
float3 HbStone(float3 base, float3 posWS, float3 n, inout float rough) {
    const float2 uv = HbTriplanarUV(posWS, n);
    const float2 cell = floor(uv / 0.42f);
    const float tone = HbHash1(cell);
    float3 c = base * (0.66f + tone * 0.68f);
    const float grain = HbFbm(uv * 55.0f, 4);
    c *= 0.84f + grain * 0.26f;
    // Cool the shadowed pits slightly - limestone reads blue-grey where it is damp.
    c.b *= 1.0f + grain * 0.10f;
    rough = saturate(0.88f + grain * 0.12f);
    return c;
}

// Poured concrete: form marks, aggregate speckle, and streaking where water has run.
float3 HbConcrete(float3 base, float3 posWS, float3 n, inout float rough) {
    const float2 uv = HbTriplanarUV(posWS, n);
    const float agg = HbFbm(uv * 160.0f, 3);       // exposed aggregate
    const float blotch = HbFbm(uv * 5.0f, 4);      // pour variation
    const float streak = HbFbm(float2(uv.x * 30.0f, uv.y * 1.2f), 3); // vertical run-off

    float3 c = base * (0.88f + blotch * 0.22f);
    c *= 0.94f + agg * 0.12f;
    c = lerp(c, c * 0.72f, saturate(streak - 0.45f) * 0.9f);
    rough = saturate(0.80f + agg * 0.18f);
    return c;
}

// Timber. The grain runs along the member's LONGEST world axis, which is what makes a stud read as
// a stud and a floorboard as a floorboard - grain across the length is the classic tell of a
// procedural wood that was written without thinking about it.
float3 HbWood(float3 base, float3 posWS, float3 n, float3 tangentWS, float2 meshUV,
              inout float rough) {
    // Each board its own colour - milled from a different tree, weathered differently.
    base *= 0.84f + HbPieceRand(HbPieceKey(posWS, n, tangentWS, meshUV), 7.0f) * 0.32f;
    // THE GRAIN RUNS ALONG THE TANGENT, NOT ALONG AN AXIS PICKED FROM THE NORMAL.
    //
    // This was wrong and it showed: a horizontal siding board and a vertical stud have the SAME
    // normal (both face out of the wall), so a normal-derived axis gave both the same grain
    // direction - and horizontal boards came out with vertical grain. The tangent is the only
    // thing that knows which way a board actually runs, and the generator already sets it to the
    // face's u-axis, which for every emitted box is its long edge.
    const float3 T = normalize(tangentWS);
    const float3 B = normalize(cross(normalize(n), T));
    const float along = dot(posWS, T);   // down the length of the board
    const float across = dot(posWS, B);  // across it - where the rings show

    // Growth rings: a ramp distorted by low-frequency noise so they wander like real grain.
    const float wobble = HbFbm(float2(along * 2.2f, across * 8.0f), 3);
    const float rings = frac((across * 26.0f) + wobble * 3.4f);
    const float ring = smoothstep(0.0f, 0.42f, rings) * smoothstep(1.0f, 0.58f, rings);

    float3 c = base * (0.80f + ring * 0.34f);

    // Knots: rare, dark, and they pull the surrounding grain around them.
    const float2 kcell = floor(float2(along * 1.6f, across * 1.6f));
    const float kseed = HbHash1(kcell);
    if (kseed > 0.86f) {
        const float2 kc = (kcell + 0.5f) / 1.6f;
        const float kd = length(float2(along, across) - kc);
        const float knot = 1.0f - smoothstep(0.0f, 0.085f, kd);
        c = lerp(c, base * 0.34f, knot * 0.85f);
    }

    // Long fine fibre, stretched hard along the board.
    const float fibre = HbFbm(float2(along * 220.0f, across * 12.0f), 2);
    c *= 0.90f + fibre * 0.18f;

    rough = saturate(0.74f + fibre * 0.20f);
    return c;
}

// Split shingles: each course tinted separately, weather-darkened toward its exposed lower edge.
float3 HbShingle(float3 base, float3 posWS, float3 n, float3 tangentWS, float2 meshUV,
                 inout float rough) {
    const float3 key = HbPieceKey(posWS, n, tangentWS, meshUV);
    base *= 0.72f + HbPieceRand(key, 3.0f) * 0.56f; // one tone per SHINGLE, not per noise cell
    // NO CELL GRID. A hashed cell lattice at a fixed size has nothing to do with where the actual
    // shingle geometry is, so it tiled as visible squares - a roof that read as BLOCKS rather than
    // as shingles. The GEOMETRY already provides the courses and their overlaps; the surface only
    // has to provide grain and weather streaking, and both of those are continuous.
    const float2 uv = HbTriplanarUV(posWS, n);
    const float grain = HbFbm(float2(uv.x * 6.0f, uv.y * 150.0f), 3); // stretched down the slope
    const float weather = HbFbm(uv * 3.5f, 4);                        // broad patchy fading
    float3 c = base * (0.80f + grain * 0.26f);
    c = lerp(c, c * 0.70f, saturate(weather - 0.40f) * 1.1f);
    rough = saturate(0.86f + grain * 0.14f);
    return c;
}

// Plaster / drywall: nearly flat, which is the point - a faint trowel texture is all that stops it
// looking like untextured geometry.
float3 HbPlaster(float3 base, float3 posWS, float3 n, inout float rough) {
    const float2 uv = HbTriplanarUV(posWS, n);
    const float trowel = HbFbm(uv * 14.0f, 3);
    const float fine = HbFbm(uv * 190.0f, 2);
    float3 c = base * (0.95f + trowel * 0.10f);
    c *= 0.97f + fine * 0.06f;
    rough = saturate(0.86f + trowel * 0.10f);
    return c;
}

// Sheet metal: streaked, and it RUSTS - which is the one weathering effect that changes hue rather
// than just value, so it has to come from the material and not from a global colour shift.
float3 HbMetal(float3 base, float3 posWS, float3 n, inout float rough, inout float metal) {
    const float2 uv = HbTriplanarUV(posWS, n);
    const float streak = HbFbm(float2(uv.x * 40.0f, uv.y * 1.6f), 3);
    const float patch = HbFbm(uv * 7.0f, 4);
    float3 c = base * (0.90f + streak * 0.18f);

    const float rust = saturate((patch - 0.52f) * 2.6f);
    const float3 rustCol = float3(0.42f, 0.17f, 0.07f);
    c = lerp(c, rustCol, rust);
    // Rust is not metallic and it is rough - blending both is what stops it reading as painted-on.
    metal = lerp(metal, 0.0f, rust);
    rough = saturate(lerp(0.32f + streak * 0.18f, 0.94f, rust));
    return c;
}

// Dispatch. Returns the shaded albedo and adjusts roughness/metalness in place.
float3 HbConstructionSurface(uint kind, float3 base, float3 posWS, float3 n, float3 tangentWS,
                             float2 meshUV, inout float rough, inout float metal) {
    switch (kind) {
        case HBE_PROC_BRICK:    return HbBrick(base, posWS, n, tangentWS, meshUV, rough);
        case HBE_PROC_MORTAR:   return HbMortar(base, posWS, n, rough);
        case HBE_PROC_STONE:    return HbStone(base, posWS, n, rough);
        case HBE_PROC_CONCRETE: return HbConcrete(base, posWS, n, rough);
        case HBE_PROC_WOOD:     return HbWood(base, posWS, n, tangentWS, meshUV, rough);
        case HBE_PROC_SHINGLE:  return HbShingle(base, posWS, n, tangentWS, meshUV, rough);
        case HBE_PROC_PLASTER:  return HbPlaster(base, posWS, n, rough);
        case HBE_PROC_METAL:    return HbMetal(base, posWS, n, rough, metal);
        default:                return base;
    }
}

#endif // HBE_CONSTRUCTION_HLSLI

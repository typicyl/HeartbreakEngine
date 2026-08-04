#include "Human/BodyField.h"

#include "Core/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hbe::human {

namespace {

// Polynomial smooth minimum (quadratic). C1 continuous, no transcendentals, and stable in
// float - which matters when it is evaluated a couple of million times per human. `k` is the
// blend radius in metres, so it is a real distance an artist can reason about.
// How softly two solids from DIFFERENT body groups merge. Small and deliberate: enough that
// the body is one closed surface, far too small to bridge an armpit or a crotch.
// 1 cm. Tight enough that an armpit stays a crease rather than filling in, and still large
// enough to RESOLVE: a feature finer than the extraction cell size cannot be represented, and
// asking for one produces pinched, non-manifold geometry instead of a crisp crease.
constexpr f32 kCrossGroupBlend = 0.012f;

inline f32 SmoothMin(f32 a, f32 b, f32 k) {
    if (k <= 1e-6f) return std::min(a, b);
    const f32 h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b * (1.0f - h) + a * h - k * h * (1.0f - h);
}

// Distance to a tapered, elliptical, optionally bellied swept solid.
//
// The ellipse comes first: scaling Z before measuring turns "distance to a round tube" into
// "distance to a flat one", which is how a ribcage gets to be wider than it is deep and a
// lat gets to lie flat against the back. It is an approximation of a true elliptical
// distance - exact only for circular sections - and it under-estimates slightly on the
// flattened axis. At the scales here that is well under a millimetre, and it costs one
// multiply instead of an iterative solve.
inline f32 SolidDistance(const FieldSolid& s, const glm::vec3& p) {
    glm::vec3 q = p, a = s.a, b = s.b;
    if (s.flatten != 1.0f) {
        const f32 inv = 1.0f / std::max(0.05f, s.flatten);
        q.z *= inv; a.z *= inv; b.z *= inv;
    }
    const glm::vec3 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    f32 t = (len2 > 1e-12f) ? glm::dot(q - a, ab) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const glm::vec3 closest = a + ab * t;

    f32 r = s.ra + (s.rb - s.ra) * t;
    if (s.bulge > 1.0f) {
        // The belly profile: a smooth bump peaking at `bias`, tapering toward the tendons at
        // either end but never to zero - a tendon has thickness, and a radius of exactly
        // zero produces a degenerate surface the extractor cannot resolve.
        const f32 bias = std::clamp(s.bias, 0.05f, 0.95f);
        const f32 u = (t < bias) ? (t / bias) * 0.5f : 0.5f + (t - bias) / (1.0f - bias) * 0.5f;
        const f32 profile = std::sqrt(std::max(0.0f, 4.0f * u * (1.0f - u)));
        r *= 1.0f + (s.bulge - 1.0f) * (0.25f + 0.75f * profile);
    }
    return glm::length(q - closest) - r;
}

} // namespace

void BodyField::Build(const Anatomy& a) {
    solids_.clear();
    solids_.reserve(a.bones.size() + a.muscles.size());
    fat_ = a.fat;
    dermis_ = a.dermis;

    for (const BoneSolid& b : a.bones) {
        FieldSolid s;
        s.a = b.head;
        s.b = b.tail;
        s.ra = b.radiusHead;
        s.rb = b.radiusTail;
        s.flatten = b.flatten;
        s.region = b.region;
        s.group = b.group;
        // Bone blends tightly: the point of a skeleton layer is that it reads as structure
        // under the flesh, not as another soft lump.
        s.blend = 0.008f;
        solids_.push_back(s);
    }

    for (const Muscle& m : a.muscles) {
        FieldSolid s;
        s.a = a.ToModel(m.originJoint, m.originLocal);
        s.b = a.ToModel(m.insertJoint, m.insertLocal);
        // PCSA is an AREA, so the belly radius is sqrt(A/pi) - the actual geometric
        // relationship, which is why doubling a muscle's cross-section widens it by ~1.41x
        // rather than 2x. Then the contraction law scales it further.
        const f32 rBelly = std::sqrt(std::max(m.pcsa, 1e-6f) / 3.14159265f) * m.RadiusScale();
        // Tendon ends are a fraction of the belly, not zero.
        s.ra = rBelly * 0.30f;
        s.rb = rBelly * 0.30f;
        s.bulge = (s.ra > 1e-6f) ? (rBelly / s.ra) : 1.0f;
        s.bias = m.bellyBias;
        s.flatten = m.flatten;
        s.blend = m.blendRadius;
        s.region = m.region;
        s.group = m.group;
        solids_.push_back(s);
    }

    for (FieldSolid& s : solids_) {
        s.centre = (s.a + s.b) * 0.5f;
        const f32 rMax = std::max(s.ra, s.rb) * std::max(1.0f, s.bulge);
        // The bound has to cover the ellipse's long axis, the blend radius AND the thickest
        // fat that could be subtracted here - a bound that is too tight silently clips
        // geometry, which reads as holes in the body.
        f32 fatMax = 0.0f;
        for (const FatDeposit& f : fat_) fatMax = std::max(fatMax, f.thickness);
        s.bound = glm::length(s.b - s.a) * 0.5f + rMax / std::min(1.0f, s.flatten) +
                  s.blend + fatMax + dermis_ + 0.01f;
    }

    lo_ = glm::vec3(1e9f);
    hi_ = glm::vec3(-1e9f);
    for (const FieldSolid& s : solids_) {
        lo_ = glm::min(lo_, s.centre - glm::vec3(s.bound));
        hi_ = glm::max(hi_, s.centre + glm::vec3(s.bound));
    }
    if (solids_.empty()) { lo_ = glm::vec3(-0.5f); hi_ = glm::vec3(0.5f); }
}

f32 BodyField::EvalTissue(const glm::vec3& p) const { return SampleTissue(p).tissue; }

BodyField::Sample BodyField::SampleTissue(const glm::vec3& p) const {
    Sample out;
    f32 d = 1e9f;
    // The group of whatever the running minimum currently belongs to. It has to be THIS and
    // not "the previously iterated solid": otherwise the blend radius depends on array order
    // rather than on geometry.
    u8 nearGroup = 0xFF;
    for (const FieldSolid& s : solids_) {
        // Bounding-sphere reject. Most samples are near a handful of primitives, so this is
        // what keeps a two-million-sample grid affordable without an acceleration structure.
        const glm::vec3 v = p - s.centre;
        const f32 far = glm::dot(v, v);
        const f32 reach = s.bound + (d < 1e8f ? d : 0.0f);
        if (far > reach * reach) continue;
        const f32 raw = SolidDistance(s, p);
        if (s.group < kGroups) out.groupDist[s.group] = std::min(out.groupDist[s.group], raw);
        // ACROSS GROUPS, BLEND TIGHTLY. Within a limb a generous blend is what makes muscle
        // read as flesh over bone. Between a limb and the trunk it is a catastrophe: an arm
        // hanging four centimetres from the ribs merges into them and the armpit fills in
        // solid. A body IS continuous, but the join at an armpit is a crease, not a fillet.
        const f32 k = (nearGroup == 0xFF || nearGroup == s.group) ? s.blend : kCrossGroupBlend;
        d = SmoothMin(d, raw, k);
        if (raw < d + 1e-6f) nearGroup = s.group;
    }
    out.tissue = d;
    return out;
}

f32 BodyField::FatAt(const glm::vec3& p, const Sample& sample) const {
    // Deposits ADD rather than replace: the abdomen and the hips overlap, and a body that
    // took only the nearest deposit would show a seam where they meet.
    f32 total = 0.0f;
    for (const FatDeposit& f : fat_) {
        if (f.thickness <= 0.0f) continue;
        const f32 d = glm::length(p - f.centre);
        const f32 edge = f.radius + f.falloff;
        if (d >= edge) continue;
        // Smoothstep falloff so a deposit feathers out instead of ending at a hard rim.
        const f32 t = (d <= f.radius) ? 1.0f : 1.0f - (d - f.radius) / std::max(1e-6f, f.falloff);
        f32 w = t * t * (3.0f - 2.0f * t);

        // ATTACHED TO ITS OWN BODY PART, SMOOTHLY. Subcutaneous fat lies on the part it
        // belongs to: an ungated deposit is a sphere that thickens everything inside it,
        // including the gap between an arm and the ribs, which welds them together.
        //
        // The gate MUST be continuous. A hard "is the nearest solid in this group" test makes
        // the field jump by centimetres across a group boundary, and a distance field with a
        // step in it breaks both the isosurface extraction and the Newton projection onto it
        // - which is exactly what a first attempt at this did. Fading over the distance to
        // that group's own tissue keeps it smooth everywhere.
        if (f.group < kGroups) {
            // RELATIVE, not absolute. Gating on the absolute distance to this group's tissue
            // is self-limiting: the fat pushes the skin outward, which increases that
            // distance, which shrinks the gate, which shrinks the fat. Comparing this group's
            // distance to the NEAREST group's instead asks the right question - "is this
            // point on my body part, or on a different one?" - and both distances grow
            // together as the surface moves out, so the depth is not capped.
            f32 gmin = 1e9f;
            for (u32 gI = 0; gI < kGroups; ++gI) gmin = std::min(gmin, sample.groupDist[gI]);
            const f32 excess = sample.groupDist[f.group] - gmin;
            constexpr f32 kBand = 0.05f; // metres of "how much nearer another part must be"
            const f32 u = std::clamp(1.0f - excess / kBand, 0.0f, 1.0f);
            w *= u * u * (3.0f - 2.0f * u);
        }
        total += f.thickness * w;
    }
    return total;
}

f32 BodyField::Eval(const glm::vec3& p) const {
    // Subtracting an amount from a distance field INFLATES the surface by that amount.
    // Fat and skin are therefore additive thickness over the muscle-and-bone body, which is
    // exactly what subcutaneous tissue is.
    const Sample s = SampleTissue(p);
    return s.tissue - FatAt(p, s) - dermis_;
}

glm::vec3 BodyField::Gradient(const glm::vec3& p, f32 h) const {
    // Tetrahedral 4-tap central difference: 4 evaluations instead of 6, and symmetric enough
    // that the resulting normals do not favour an axis.
    const glm::vec2 k(1.0f, -1.0f);
    const glm::vec3 g =
        glm::vec3(k.x, k.y, k.y) * Eval(p + glm::vec3(k.x, k.y, k.y) * h) +
        glm::vec3(k.y, k.y, k.x) * Eval(p + glm::vec3(k.y, k.y, k.x) * h) +
        glm::vec3(k.y, k.x, k.y) * Eval(p + glm::vec3(k.y, k.x, k.y) * h) +
        glm::vec3(k.x, k.x, k.x) * Eval(p + glm::vec3(k.x, k.x, k.x) * h);
    const f32 len = glm::length(g);
    return (len > 1e-12f) ? g / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

Region BodyField::RegionAt(const glm::vec3& p) const {
    // Nearest primitive by RAW distance, deliberately not by the blended field: the blend is
    // what makes the surface continuous, but a region label must be a crisp decision or the
    // boundaries between regions would smear and no longer partition the body.
    f32 best = 1e9f;
    Region r = Region::Pelvis;
    for (const FieldSolid& s : solids_) {
        const glm::vec3 v = p - s.centre;
        if (glm::dot(v, v) > (s.bound + best) * (s.bound + best)) continue;
        const f32 d = SolidDistance(s, p);
        if (d < best) { best = d; r = s.region; }
    }
    return r;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("bodyfield FAIL: %s\n", what); ++g_fails; }
}
// Walk outward from a point until the field turns positive: the surface radius along a ray.
f32 SurfaceAlong(const BodyField& f, glm::vec3 origin, glm::vec3 dir, f32 maxT = 1.0f) {
    f32 prev = f.Eval(origin);
    for (f32 t = 0.0f; t < maxT; t += 0.001f) {
        const f32 d = f.Eval(origin + dir * t);
        if (prev < 0.0f && d >= 0.0f) return t;
        prev = d;
    }
    return -1.0f;
}
} // namespace

bool BodyFieldSelfTest() {
    g_fails = 0;
    HumanParameters p;
    BodyField f;
    f.Build(Resolve(p));

    Check(!f.Solids().empty(), "the field must resolve some anatomy");

    // Inside must be negative, far outside positive - the sign convention everything else
    // depends on.
    const Anatomy a = Resolve(p);
    const glm::vec3 chest = a.joints[static_cast<usize>(a.FindJoint("Spine2"))].position;
    Check(f.Eval(chest) < 0.0f, "a point inside the chest must be INSIDE the field");
    Check(f.Eval(chest + glm::vec3(0, 0, 5.0f)) > 0.0f, "a point far away must be outside");

    // The surface must sit where a body's surface sits, not at an arbitrary distance.
    const f32 chestFront = SurfaceAlong(f, chest, {0, 0, 1});
    Check(chestFront > 0.03f && chestFront < 0.35f,
          "the chest surface must be a plausible distance in front of the spine");

    // THE CENTRAL CLAIM: growing a muscle moves the SURFACE over that muscle.
    {
        HumanParameters big = p;
        big.muscle.arms = 2.5f;
        BodyField fb;
        fb.Build(Resolve(big));
        // Midway along the humerus, wherever the A-pose put it - probing a fixed offset
        // below the shoulder joint misses the arm entirely once it swings out.
        const glm::vec3 arm =
            (a.joints[static_cast<usize>(a.FindJoint("LeftArm"))].position +
             a.joints[static_cast<usize>(a.FindJoint("LeftForeArm"))].position) * 0.5f;
        const f32 r0 = SurfaceAlong(f, arm, {0, 0, 1});
        const f32 r1 = SurfaceAlong(fb, arm, {0, 0, 1});
        Check(r0 > 0.0f && r1 > 0.0f, "the arm surface must be findable in both");
        Check(r1 > r0 + 0.004f,
              "A BIGGER BICEPS MUST PUSH THE SKIN OUT - this is the whole premise: the "
              "surface is derived from the anatomy, not from a morph");
        // ...and it must be LOCAL.
        const glm::vec3 calf = (a.joints[static_cast<usize>(a.FindJoint("LeftLeg"))].position +
                                a.joints[static_cast<usize>(a.FindJoint("LeftFoot"))].position) * 0.5f;
        const f32 c0 = SurfaceAlong(f, calf, {0, 0, -1});
        const f32 c1 = SurfaceAlong(fb, calf, {0, 0, -1});
        Check(std::abs(c1 - c0) < 0.002f, "growing the arms must not move the calves");
    }

    // Fat must inflate the surface where it is deposited.
    {
        HumanParameters heavy = p;
        heavy.weight = 120.0f;
        BodyField fh;
        fh.Build(Resolve(heavy));
        const glm::vec3 belly = a.joints[static_cast<usize>(a.FindJoint("Spine"))].position;
        const f32 r0 = SurfaceAlong(f, belly, {0, 0, 1});
        const f32 r1 = SurfaceAlong(fh, belly, {0, 0, 1});
        Check(r1 > r0 + 0.01f, "a heavier human must have a deeper belly");
    }

    // Contraction must bulge the surface without any parameter change at all.
    {
        Anatomy flexed = Resolve(p);
        for (Muscle& m : flexed.muscles)
            if (m.name.find("Biceps") != std::string::npos) m.activation = 1.0f;
        BodyField ff;
        ff.Build(flexed);
        const glm::vec3 arm =
            (a.joints[static_cast<usize>(a.FindJoint("LeftArm"))].position +
             a.joints[static_cast<usize>(a.FindJoint("LeftForeArm"))].position) * 0.5f;
        const f32 r0 = SurfaceAlong(f, arm, {0, 0, 1});
        const f32 r1 = SurfaceAlong(ff, arm, {0, 0, 1});
        Check(r1 > r0, "FLEXING must bulge the surface - volume preservation, not a blendshape");
    }

    // Gradient must be a unit normal pointing outward.
    {
        const glm::vec3 pOut = chest + glm::vec3(0, 0, chestFront + 0.02f);
        const glm::vec3 g = f.Gradient(pOut);
        Check(std::abs(glm::length(g) - 1.0f) < 1e-3f, "the gradient must be normalised");
        Check(g.z > 0.3f, "the gradient in front of the chest must point forward, out of the body");
    }

    // Region tagging must be crisp and correct.
    Check(f.RegionAt(chest) == Region::Chest || f.RegionAt(chest) == Region::UpperBack,
          "the chest interior must be tagged as torso");
    {
        const glm::vec3 thigh = a.joints[static_cast<usize>(a.FindJoint("LeftUpLeg"))].position +
                                glm::vec3(0, -0.12f, 0);
        Check(f.RegionAt(thigh) == Region::ThighL, "the left thigh must be tagged as the left thigh");
    }

    // Determinism.
    {
        BodyField f2;
        f2.Build(Resolve(p));
        bool same = true;
        for (int i = 0; i < 500; ++i) {
            Rng r(u64(i) + 7);
            const glm::vec3 q(r.Range(-0.6f, 0.6f), r.Range(0.0f, 1.9f), r.Range(-0.4f, 0.4f));
            if (f.Eval(q) != f2.Eval(q)) { same = false; break; }
        }
        Check(same, "the same anatomy must evaluate BIT-IDENTICALLY - the bake key depends on it");
    }

    if (g_fails == 0)
        std::printf("bodyfield: %d primitives composed by smooth union; a bigger muscle pushes "
                    "the skin out LOCALLY, fat deepens the belly, flexing bulges the arm, and "
                    "regions stay crisply labelled\n",
                    static_cast<int>(f.Solids().size()));
    return g_fails == 0;
}

} // namespace hbe::human

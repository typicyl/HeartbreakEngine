// Human/BodyField.h - the anatomy, composed into one continuous body.
//
// Every anatomical structure is a signed distance function. The body is their union, blended
// smoothly, minus the fat and skin laid on top. That single idea is what makes the surface
// DERIVED rather than authored:
//
//     phi_body(p) = smoothUnion(bones, muscles) - fat(p) - dermis
//
// A negative value is inside the body, positive outside, and the skin is where it crosses
// zero. Grow a muscle's cross-section and the zero crossing moves outward over that muscle -
// which is a bulge, obtained by arithmetic rather than by a sculpted shape.
//
// WHY SMOOTH UNION AND NOT PLAIN MIN. A hard `min` gives a visible crease everywhere two
// structures meet - a biceps would look glued onto the humerus. The polynomial smooth
// minimum blends them over a small radius, which is both what flesh actually does and what
// keeps the extracted surface free of the sharp creases that wreck normals and tangents.
//
// EVALUATION COST is the honest constraint here: this is called once per grid sample, and a
// 128^3 grid is two million samples. Every primitive is closed-form, allocation-free and
// branch-light, the whole thing is const after Build() so it is safe to evaluate from many
// threads at once, and a bounding-sphere reject skips most primitives per sample.
#pragma once

#include "Core/Types.h"
#include "Human/Anatomy.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::human {

// A resolved anatomical primitive in model space: everything needed to evaluate it, with
// nothing left to look up. Bones and muscles both reduce to this - a tapered, elliptical
// swept solid - which is why the composition is one loop rather than two.
struct FieldSolid {
    glm::vec3 a{0.0f}, b{0.0f}; // segment endpoints, model space
    f32 ra = 0.02f, rb = 0.02f; // radius at each end
    f32 bulge = 1.0f;           // extra radius at the belly (muscles), 1 = no belly
    f32 bias = 0.5f;            // where along the span the belly sits
    f32 flatten = 1.0f;         // Z (front-back) scale of the cross-section ellipse
    f32 blend = 0.02f;          // smooth-union radius against everything else
    Region region = Region::Pelvis;
    // Which limb/trunk this solid belongs to. Solids in DIFFERENT groups blend far more
    // tightly than solids within one, which is what stops an arm hanging 4 cm from the ribs
    // from smooth-unioning into it. A body is continuous, but an armpit is a crease, not a
    // fillet - without this the whole torso and both arms fuse into one slab.
    u8 group = 0;
    // Bounding sphere, so a sample far from this solid costs one dot product.
    glm::vec3 centre{0.0f};
    f32 bound = 0.0f;
};

class BodyField {
public:
    // Resolve the anatomy into evaluable primitives. Call once per generated human.
    void Build(const Anatomy& a);

    // Signed distance to the skin. Negative inside. THE function the surface comes from.
    f32 Eval(const glm::vec3& p) const;

    // Central-difference gradient - the surface normal, before any mesh exists. Used to
    // project extracted vertices onto the true surface rather than leaving them on the grid.
    glm::vec3 Gradient(const glm::vec3& p, f32 h = 0.002f) const;

    // Which anatomical region owns this point, by nearest primitive. This is what tags the
    // generated surface so that regions survive into editing, weighting and export.
    Region RegionAt(const glm::vec3& p) const;

    // Distance to the MUSCLE-AND-BONE surface, ignoring fat and skin. The difference between
    // this and Eval is the local soft-tissue depth, which is what a later subsurface or
    // jiggle layer needs and what makes "how deep is the fat here" answerable.
    f32 EvalTissue(const glm::vec3& p) const;

    // One traversal produces both the blended tissue distance AND the raw distance to each
    // body group. The per-group distances are what let fat be attached to the part it
    // belongs to CONTINUOUSLY - see FatAt.
    static constexpr u32 kGroups = 6;
    struct Sample {
        f32 tissue = 1e9f;
        f32 groupDist[kGroups] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
    };
    Sample SampleTissue(const glm::vec3& p) const;

    const std::vector<FieldSolid>& Solids() const { return solids_; }
    void Bounds(glm::vec3& lo, glm::vec3& hi) const { lo = lo_; hi = hi_; }

private:
    f32 FatAt(const glm::vec3& p, const Sample& s) const;

    std::vector<FieldSolid> solids_;
    std::vector<FatDeposit> fat_;
    f32 dermis_ = 0.002f;
    glm::vec3 lo_{0.0f}, hi_{0.0f};
};

bool BodyFieldSelfTest();

} // namespace hbe::human

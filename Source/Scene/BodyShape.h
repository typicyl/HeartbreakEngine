// Scene/BodyShape.h - sliders that reshape a character, with no art at all.
//
// THE IDEA. A character creator normally costs an artist a sculpted blendshape per slider
// per body part. This one costs nothing: the sliders drive the SKELETON, not the mesh.
// Import a rig with recognisable joint names and ~15 working sliders appear immediately,
// on every part, including parts imported tomorrow that nobody has ever seen.
//
// WHY THE SKELETON AND NOT BLENDSHAPES - this is forced, not preferred. Characters here are
// modular parts welded together at build time, and weld::WeldSeams guarantees a closed seam
// by stamping ONE rest position and ONE {joints,weights} binding into every part that meets
// there. Two welded vertices then evaluate the same skinning sum over the same bytes and
// land in the same place, for ANY palette - which weld::SelfTest already proves with a
// deliberately non-rigid (scaled + sheared) palette. A body slider only ever changes the
// palette, so the seam cannot open: not "within tolerance", bit-exactly.
//
// The GPU morph path CANNOT be used for this. It adds deltas per part, indexed by that
// part's own vertex id in that part's own atlas, AFTER the weld - so two welded vertices
// get different deltas by construction and the seam opens by exactly the delta. Morphs stay
// where they are correct: facial animation, which never crosses a seam.
//
// WHY SCALING A JOINT IS ENOUGH. Composition multiplies a child's local transform by its
// parent's global, so scaling one joint moves everything below it. Scaling `thigh` lengthens
// the thigh AND carries the knee, ankle and foot - meshes included. That is why "wider
// shoulders" does not leave the arms behind: it scales the clavicles, and the arm chains
// hang off them. (Writing into the composed globals instead would scale one bone's flesh and
// strand every descendant - a torn limb. The injection point matters.)
//
// BONE AXIS IS DERIVED, NOT AUTHORED. "Longer" and "thicker" are different axes, and which
// axis a bone runs along differs per rig. A joint's child sits at the far end of the bone, so
// the child's bind translation IS the bone direction in that joint's local frame. That makes
// length-vs-thickness rig-agnostic with nothing to configure.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {
struct Skeleton;
}

namespace hbe::bodyshape {

// One slider as the user sees it. `present` is false when this rig has none of the joints
// the slider drives - the UI hides it rather than offering a control that does nothing.
struct SliderDesc {
    std::string name;
    std::string tip;
    bool present = false;
};

// A resolved per-joint change. Baked once when a value changes, then read-only for the rest
// of the frame: the pose pass runs inside a parallel job and must not be resolving names.
struct JointShape {
    u16 joint = 0;
    glm::vec3 scale{1.0f};
};

// Every slider this skeleton can offer, in display order. Costs one pass over the joints.
std::vector<SliderDesc> Resolve(const Skeleton& sk);

// Turn authored slider values into the flat per-joint list the pose pass applies.
// Unknown slider names and unmatched joints are ignored rather than treated as errors, the
// same tolerance the joint remap and the variant lookup already use - a rig that is missing
// a bone should lose one slider, not fail to load.
void Bake(const Skeleton& sk, const std::unordered_map<std::string, f32>& values,
          std::vector<JointShape>& out);

// Values are authored in [-1, 1] and default to 0 = the rig exactly as imported.
inline constexpr f32 kMin = -1.0f;
inline constexpr f32 kMax = 1.0f;

bool SelfTest(); // --test-bodyshape

} // namespace hbe::bodyshape

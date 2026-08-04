// Human/Anatomy.h - the body itself. THIS is the source of truth; the mesh is a view of it.
//
// The layering the whole tool is built on:
//
//     PARAMETERS -> SKELETON -> MUSCLES -> FAT -> composed FIELD -> SURFACE
//
// Every arrow is one-way. Nothing downstream feeds back, which is what makes generation
// deterministic and what makes "change the weight and the belly changes" true by
// construction rather than by a morph somebody sculpted.
//
// WHY SIGNED DISTANCE FIELDS RATHER THAN MESHES FOR THE ANATOMY. A muscle is a volume that
// bulges, slides over bone, and merges smoothly into its neighbours. As a mesh, every one of
// those is a hard problem (self-intersection, boolean unions, remeshing). As a field they
// are arithmetic: union is min, smooth union is a polynomial blend, "muscle sits on bone" is
// max against the bone field. The surface is extracted ONCE at the end, so the extraction
// cost is paid once per generated human rather than once per operation.
//
// SCALE AND UNITS: metres throughout, model space, origin at the floor between the feet,
// +Y up, +Z forward, +X to the character's LEFT (so that a "left" limb has positive X).
#pragma once

#include "Core/Types.h"
#include "Human/HumanParameters.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe::human {

// ---------------------------------------------------------------------------
// Layer 1: SKELETON
// ---------------------------------------------------------------------------

// The articulated joint. Deliberately kept to a conventional, engine-compatible set and
// naming so that the exported skeleton retargets against existing animation without a
// translation table - the engine matches clips to joints BY NAME.
struct Joint {
    std::string name;
    i32 parent = -1;         // parents always precede children
    glm::vec3 position{0.0f}; // MODEL space at rest (not parent-relative; see LocalOf)
    Region region = Region::Pelvis;
};

// The shape of a bone, which is a different thing from the joint that moves it. Ribs and
// vertebrae need shape but not articulation, so they get solids and no joint - conflating
// the two is what silently blows a 256-joint palette budget.
struct BoneSolid {
    i32 jointA = -1;          // the joint this bone runs FROM
    i32 jointB = -1;          // ...and TO. -1 = a free solid positioned by `head`/`tail`.
    glm::vec3 head{0.0f}, tail{0.0f}; // model space, filled from the joints when they exist
    f32 radiusHead = 0.02f, radiusTail = 0.02f;
    // Cross-section is an ellipse, not a circle: a tibia is not round, and a ribcage is much
    // wider than it is deep. `flatten` scales the Z (front-back) axis of the section.
    f32 flatten = 1.0f;
    Region region = Region::Pelvis;
    u8 group = 0; // 0 trunk, 1 head/neck, 2 armL, 3 armR, 4 legL, 5 legR
};

// ---------------------------------------------------------------------------
// Layer 2: MUSCLES
// ---------------------------------------------------------------------------

// A muscle-tendon unit as an actual anatomical structure: it spans from an ORIGIN on one
// bone to an INSERTION on another, has a real volume, and bulges when it shortens.
//
// The contraction law is a physical invariant, not a curve somebody tuned: muscle tissue is
// very nearly incompressible, so if a belly of length L and cross-section A shortens by a
// factor k, the section must grow by 1/k to hold volume. The radius therefore scales by
// 1/sqrt(k). That single line is why "flex the arm" produces a bulge without a blendshape.
struct Muscle {
    std::string name;
    Region region = Region::UpperArmL;

    // Attachments are stored as (joint, offset in that joint's frame) so they FOLLOW the
    // skeleton for free: change the humerus length and the insertion moves with it.
    i32 originJoint = -1;
    glm::vec3 originLocal{0.0f};
    i32 insertJoint = -1;
    glm::vec3 insertLocal{0.0f};
    // Optional waypoint that pushes the belly off the bone, standing in for the wrapping
    // surfaces a full anatomical solver would use. Explicitly an approximation.
    bool hasVia = false;
    glm::vec3 viaLocal{0.0f};
    i32 viaJoint = -1;

    f32 pcsa = 0.0f;        // physiological cross-sectional area, m^2 - the "how big" number
    f32 restLength = 0.0f;  // solved from the attachments, never authored
    f32 bellyBias = 0.5f;   // where along the span the thickest point sits (0..1)
    f32 flatten = 1.0f;     // section ellipse: a lat is flat, a biceps is round
    f32 blendRadius = 0.02f;// how softly it merges with neighbours and with bone

    // Runtime state of the generator, not of the game: an authoring pose can flex a muscle
    // to inspect the bulge. 0 = rest.
    f32 activation = 0.0f;
    f32 maxShortening = 0.30f; // fraction of rest length at full activation
    u8 group = 0;

    // Volume-preserving contraction. Returns the radius multiplier for the current
    // activation - the whole of "shortens and thickens", in one place.
    f32 RadiusScale() const;
    f32 CurrentLength() const;
};

// ---------------------------------------------------------------------------
// Layer 3: FAT / SOFT TISSUE
// ---------------------------------------------------------------------------

// Subcutaneous fat as a thickness laid over a region, which is exactly how skinfold
// anthropometry describes it - so the numbers here map onto real reference data instead of
// being invented.
struct FatDeposit {
    Region region = Region::Abdomen;
    f32 thickness = 0.005f;   // metres at the centre of the deposit
    f32 falloff = 0.25f;      // how far it feathers into neighbouring regions
    // Where within the region the deposit is centred and how far it reaches, both in model
    // space, so a belly can sit forward of the spine rather than uniformly around it.
    glm::vec3 centre{0.0f};
    f32 radius = 0.15f;
    // Which body group this deposit belongs to. Fat is subcutaneous: it sits on the part it
    // belongs to and nowhere else. Without this gate the abdominal deposit - a 22 cm sphere
    // centred in the waist - inflates the upper arms and the air between them, welding the
    // whole torso and both arms into one mass.
    u8 group = 0;
};

// ---------------------------------------------------------------------------
// The resolved body
// ---------------------------------------------------------------------------

// Everything the surface is generated FROM. Pure data, no GPU, no engine types: it can be
// built, hashed and tested headlessly, which is what keeps determinism checkable.
struct Anatomy {
    std::vector<Joint> joints;
    std::vector<BoneSolid> bones;
    std::vector<Muscle> muscles;
    std::vector<FatDeposit> fat;

    // Overall dermis thickness, added everywhere. Real skin is ~1-3 mm; it matters because
    // it is what closes the small gaps between adjacent muscle bellies.
    f32 dermis = 0.002f;

    u64 paramHash = 0;
    u32 generatorVersion = kGeneratorVersion;

    i32 FindJoint(const std::string& name) const;
    // Model-space position of a point expressed in a joint's frame. Stage 1 skeletons are
    // built in a rest pose with axis-aligned frames, so this is a translation - the function
    // exists so that adding real joint orientations later does not change every call site.
    glm::vec3 ToModel(i32 joint, const glm::vec3& local) const;

    // Bounding box of every solid, used to size the extraction grid.
    void Bounds(glm::vec3& lo, glm::vec3& hi) const;
};

// PARAMETERS -> ANATOMY. Deterministic, sub-millisecond, no allocation beyond the vectors.
// This is the function that must stay honest: everything visible about a human has to be
// traceable to something this produced.
Anatomy Resolve(const HumanParameters& p);

bool AnatomySelfTest();

} // namespace hbe::human

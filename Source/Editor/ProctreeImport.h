// Editor/ProctreeImport.h - parametric ("proctree-style") tree importer, EDITOR-ONLY.
//
// A compact, self-contained parametric tree authoring model - the sanctioned "port the math,
// not the library" resolution of the proctree owner decision. It vendors NO external library
// (so it adds no FetchContent / network dependency and no runtime link): the trunk grows upward
// and recursively forks into thinner, shorter, angled child branches, twigs sprouting leaf
// clusters, all deterministic from `seed` via Core/Rng. It emits the engine's crown-jewel
// `veg::PlantSkeleton`, so the result flows through the exact same pipeline as the built-in
// generators: BuildTreeMesh -> uaf::WriteMesh -> an `External` species (authoredMesh) that the
// paint brush / scatter replay. Lives in hbe_editor only (HBE_EDITOR_SOURCES); the shipped
// runtime never links it. A future binding to the actual proctree BSD library would slot in
// behind this same ImportProctreeSkeleton interface without changing any caller.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Core/Types.h"

namespace hbe::editor {

// Parameters mirroring the proctree/Weber-Penn parametric family. Defaults produce a plausible
// broadleaf; all are exposed as editor sliders.
struct ProctreeParams {
    u32 seed = 1337;
    u32 levels = 4;            // branch orders (recursion depth); order 0 = trunk, levels-1 = twig
    u32 segments = 6;          // nodes grown per branch
    u32 childCount = 3;        // child branches spawned per branch
    f32 trunkHeight = 6.0f;    // trunk length (m)
    f32 trunkRadius = 0.24f;   // trunk base radius (m)
    f32 taper = 0.66f;         // radius multiplier per level
    f32 lengthFalloff = 0.72f; // length multiplier per level
    f32 branchAngle = 44.0f;   // child spread from the parent axis (degrees)
    f32 twist = 34.0f;         // azimuth between sibling children (degrees)
    f32 droop = 0.12f;         // gravity bias, stronger at higher orders
    f32 wobble = 0.14f;        // per-segment direction jitter
    f32 leafSize = 0.5f;       // leaf-cluster card radius (m)
};

// Builds a PlantSkeleton from `params`. Pre-order emission keeps the load-bearing
// parent < node-index invariant every mesher/generator relies on. Returns false only for
// degenerate params (0 levels/segments or non-positive trunk height).
bool ImportProctreeSkeleton(const ProctreeParams& params, veg::PlantSkeleton& out);

// Headless self-test (--test-proctree): deterministic (same seed -> byte-identical skeleton),
// valid (parent invariant), bounded (finite positions), meshable (woody + leaf nodes both
// present), and seed-sensitive (a different seed yields a different tree).
bool ProctreeSelfTest();

} // namespace hbe::editor

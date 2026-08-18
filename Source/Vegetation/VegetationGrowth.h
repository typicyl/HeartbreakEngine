// Vegetation/VegetationGrowth.h - incremental structural growth (P10).
//
// GrowSkeleton advances an EXISTING skeleton from age a0 to a1 by ADDING structure (new
// twig/leaf nodes at the growing tips) and thickening the wood - it never regenerates from
// scratch, so a plant's earlier form is preserved and grows out of. Deterministic from the
// skeleton's sourceSeed, and job-safe (pure CPU, no shared state). This is the difference
// the design draws between real growth and merely scaling a fixed mesh.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Vegetation/Species.h"

namespace hbe::veg {

// Grows `skel` in place from age a0 to a1 (both in [0,1] of the plant's life). A no-op when
// a1 <= a0. Adds tip extensions + leaf clusters proportional to the age gained and thickens
// every node's radius toward its mature value.
void GrowSkeleton(PlantSkeleton& skel, const Species& sp, f32 a0, f32 a1);

} // namespace hbe::veg

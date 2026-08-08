// Construction/ConstructionMasonry.h - Phase 3: courses, bonds and units.
//
// THE ARCHITECTURAL POINT OF THIS FILE. Layout is computed ONCE, by one function, and both the
// geometry generator and the capacity estimator consume its output. They cannot disagree, because
// there is no second implementation to drift.
//
// That is not tidiness. The RHI cannot free a mesh and `UpdateMesh` NEVER GROWS: if the estimate
// were computed by separate arithmetic and came out one unit short, the update would be refused,
// the GPU would keep the PREVIOUS geometry while the caller's bounds and colliders described the
// new wall, and there would be no way to free the mesh and try again. A capacity that disagrees
// with generation is not a rounding error in this engine, it is a permanent corruption.
//
// WHY REAL GEOMETRY AND NOT A TEXTURE. The brief is explicit ("Do not rely exclusively on
// textures"), and it is right for a destruction-oriented system: a normal map cannot lose a
// brick. The cost is real and worth stating plainly - see the note on unit counts below.
#pragma once

#include "Construction/ConstructionDef.h"

#include <glm/glm.hpp>

namespace hbe::construction {

// One masonry unit, in the component's local space.
struct BrickPlacement {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f}; // half-extents, already jittered and already clipped at wall ends
    f32 yaw = 0.0f;         // radians about local Y
    f32 roll = 0.0f;        // radians about local Z
    u32 course = 0;         // 0 at the bottom
    u32 indexInCourse = 0;

    // STABLE ACROSS REGENERATION AND ACROSS DAMAGE. Derived from (course, indexInCourse), never
    // from emission order - a damage record naming "the 400th brick emitted" would name a
    // different brick the moment an earlier one was destroyed.
    u32 ElementId() const { return (course << 16) | (indexInCourse & 0xFFFFu); }
};

// Reasons a layout can come back short or empty, so a caller can say something useful instead of
// silently generating nothing.
enum class MasonryResult : u8 {
    Ok = 0,
    DegenerateExtent,  // the component has no volume to lay units in
    DegenerateUnit,    // unit dimensions are zero or negative
    UnitCapReached,    // hit MasonryParams::maxUnits; output is truncated, not wrong
};

// Lays out the units for one component. Deterministic: identical input always yields an identical
// vector, because every jitter draws from a stream keyed on (component id, course, indexInCourse)
// rather than on a running counter.
//
// A wall of 8 x 3 m in standard brick is about 1,400 units - roughly 34k vertices. That is the
// real price of construction geometry, it is per WALL, and this engine has NO LOD SYSTEM to fall
// back on at distance (see docs/Design-ProceduralConstruction.md SS2.1). Full unit geometry is a
// hero-surface tool; a whole city of it is not viable today.
MasonryResult LayoutMasonry(const ConstructionDef& def, const ConstructionComponent& c,
                            std::vector<BrickPlacement>& out);

// The unit count LayoutMasonry would produce, without building the vector. Used by the capacity
// estimator; shares the same course/bond arithmetic so the two cannot drift.
u32 MasonryUnitCount(const ConstructionDef& def, const ConstructionComponent& c);

} // namespace hbe::construction

// Construction/ConstructionGeometry.h - Phase 2: definition -> MeshData.
//
// WHAT THIS PHASE IS. The massing: walls, floors, ceilings, roofs, foundations, beams and
// columns as solid geometry. No openings (Phase 5 needs a clipper that is still file-private
// inside Fracture.cpp), no brick courses (Phase 3), no damage (Phase 7).
//
// THREE DECISIONS FORCED BY THE RENDERER, all measured, see docs/Design-ProceduralConstruction.md:
//
//   1. A SECTION MERGES INTO ONE SUBMESH PER MATERIAL. The per-frame constant arena caps the
//      whole frame at roughly 5,400 draw items - not the 16k the code comments claim - and
//      Vulkan silently TRUNCATES past its cap while D3D12 drops shadow casters. One entity per
//      stud is therefore not an option; a section is one draw per material it uses.
//   2. SUBMESH ORDER IS BY MaterialKind, ASCENDING. Scenes reference submeshes positionally as
//      "uaf:<path>#<index>", so renumbering silently repoints existing references at different
//      geometry. Ordering by an append-only enum means adding a material later can only ever
//      APPEND - the same discipline MeshFaceSelect already enforces for splits.
//   3. EVERY COMPONENT'S TRIANGLES STAY ADDRESSABLE. `MeshRange` records which slice of which
//      submesh each component owns. That is what lets a later phase drop a destroyed component
//      without re-deriving the whole building, and it is the hand-off a destruction pass needs.
//
// GEOMETRY CONVENTION matches Assets/MeshGenerator.cpp exactly, deliberately: a face is
// (origin, du, dv), normal = cross(du, dv), tangent = du with w = +1, corners wound
// (o, o+du, o+du+dv, o+dv) and indexed (0,1,2),(0,2,3). Diverging would put this system's
// geometry at a different handedness from every primitive in the engine.
#pragma once

#include "Construction/ConstructionDef.h"
#include "Assets/Mesh.h"

#include <glm/mat4x4.hpp>

namespace hbe::construction {

// The slice of generated geometry belonging to ONE component.
struct MeshRange {
    ComponentId id = kInvalidComponent;
    u32 submesh = 0;     // index into SectionMesh::model
    u32 firstIndex = 0;  // into that submesh's index buffer
    u32 indexCount = 0;
    u32 firstVertex = 0;
    u32 vertexCount = 0;
};

// The slice belonging to ONE sub-element of a component - a single brick in a wall.
//
// Present only for components that generate discrete units. Massing blocks have no elements; the
// component IS the element. This is what brief SS5's "individual bricks should remain addressable"
// and SS13's Wall Chunk -> Brick Group -> Individual Debris hierarchy both need.
struct ElementRange {
    ComponentId component = kInvalidComponent;
    u32 element = 0; // BrickPlacement::ElementId - stable across regeneration and damage
    u32 submesh = 0;
    u32 firstIndex = 0;
    u32 indexCount = 0;
};

struct SectionMesh {
    Model model;                   // one MeshData per MaterialKind present, ascending
    std::vector<MeshRange> ranges; // sorted by ComponentId
    std::vector<ElementRange> elements; // per-unit slices (masonry); empty for pure massing
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    const MeshRange* RangeFor(ComponentId id) const;
    // All elements belonging to one component, in emission order.
    std::vector<const ElementRange*> ElementsOf(ComponentId id) const;
    u32 TotalIndices() const;
    u32 TotalVertices() const;
};

// Worst-case sizes for a reservation.
//
// WHY THIS EXISTS AT ALL. The RHI cannot free a mesh - `DestroyGpuBuffer` (compute buffers) is
// the only destroy in the whole interface - and `UpdateMesh` NEVER GROWS: a larger upload is
// refused on every backend, and the refusal leaves the OLD geometry on the GPU while the caller's
// bounds and colliders describe the new one. The single leak-free path is to reserve once with
// headroom and then update in place forever, checking the bool. So a caller must be able to ask
// "how big can this ever get" BEFORE it uploads anything.
struct MeshCapacity {
    u32 vertices = 0;
    u32 indices = 0;
};

// EXACT for Phase 2 shapes - every kind here emits a fixed vertex/index count, so this is a
// count, not a guess. Phases 3+ (brick courses, damage) introduce parameter-dependent counts and
// must extend this with a real worst-case bound rather than letting callers discover the limit
// through a refused UpdateMesh.
//
// `headroom` multiplies the result (1.0 = exact). Reserve above 1.0 when the caller intends to
// keep editing the definition in place.
MeshCapacity EstimateCapacity(const ConstructionDef& def, ComponentId root, f32 headroom = 1.0f);

// Accumulated parent-relative transform. Walks the containment chain, so a stud positioned inside
// its wall lands in the building's space.
glm::mat4 WorldMatrix(const ConstructionDef& def, ComponentId id);

// Generates the subtree rooted at `root` (kInvalidComponent = the whole definition).
//
// Deterministic: the same (definition, seed, damage) always produces byte-identical output,
// because components are emitted in ascending id order and every vertex is a pure function of the
// definition. Destroyed, hidden and geometry-less components contribute nothing.
void BuildSection(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                  SectionMesh& out);

// True when this kind emits geometry at all. `Building` is a container and `Opening` is a hole -
// both are real components with real relationships that draw nothing.
bool EmitsGeometry(ComponentKind kind);

// The procedural shader surface a construction material should be drawn with. Returned as the
// packed MaterialFlags bits, ready to OR into MeshInstance::materialFlags.
u32 ProceduralSurfaceFlagsFor(MaterialKind m);

} // namespace hbe::construction

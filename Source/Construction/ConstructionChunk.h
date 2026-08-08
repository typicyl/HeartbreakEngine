// Construction/ConstructionChunk.h - logical construction units, and their spatial grouping.
//
// THE DISTINCTION THIS FILE EXISTS TO MAKE (brief SS8/SS25). These are three different things and
// conflating any two of them is how a procedural building system dies:
//
//     LOGICAL CONSTRUCTION UNIT   one brick, one stud, one shingle. Thousands of them.
//     RENDER OBJECT               one mesh, one draw call. Tens of them.
//     PHYSICS OBJECT              one collider. Fewer still.
//
// A 20,000-unit building is fine. 20,000 draw calls is not - this renderer caps the whole FRAME at
// roughly 5,400 draw items and silently truncates past that. So generation produces a PIECE LIST
// (the logical units), and the meshes are derived from it by bucketing pieces into spatial chunks
// and merging each chunk's pieces by material.
//
// WHY A PIECE LIST RATHER THAN EMITTING STRAIGHT INTO MESHES. Two reasons, and the second is the
// one that made it worth restructuring:
//
//   1. The logical construction survives as data, which is what a future destruction pass needs to
//      answer "which units are in this region" without re-deriving anything.
//   2. Chunked and unchunked generation become the SAME CODE with a different bucketing rule. The
//      alternative - a second dispatch that walks components and decides masonry-vs-wood-vs-solid
//      all over again - is precisely the kind of duplicated traversal that has to be kept in
//      lockstep by hand, and this system has already been bitten by a count that disagreed with a
//      generator once.
#pragma once

#include "Construction/ConstructionGeometry.h"

namespace hbe::construction {

// One generated box or prism, placed in world space. This IS a logical construction unit: a brick,
// a stud, a shingle, a mortar backing slab, or a whole massing block.
struct PlacedPiece {
    ComponentId component = kInvalidComponent;
    // The sub-element id within that component (BrickPlacement/BoardPlacement ElementId), or
    // kWholeComponent when the piece IS the component - a massing block, a mortar backing, a
    // window frame member.
    u32 element = 0;
    MaterialKind material = MaterialKind::Unknown;
    glm::mat4 xform{1.0f}; // world transform of the piece's centre
    glm::vec3 extent{0.0f};
    bool gable = false; // false = box
    // Carried BY VALUE, not as a pointer into the definition: pieces outlive the call that made
    // them and a dangling parameter block would be a use-after-free the first time a definition
    // was edited between generation and emission.
    WeatheringParams weathering{};

    glm::vec3 Center() const { return glm::vec3(xform[3]); }
};

inline constexpr u32 kWholeComponent = 0xFFFFFFFFu;

// Every logical unit the definition produces, in deterministic order (ascending component id, then
// generation order within the component). This is the construction, before any decision about how
// to render it.
void CollectPieces(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                   std::vector<PlacedPiece>& out);

// One spatial chunk of a generated structure.
//
// The unit of: culling, streaming, cache invalidation, and "which chunk became structurally
// compromised". Chunk coordinates are integer cell indices so a chunk's identity is stable under
// regeneration - it does not shift when a wall is resized.
struct ConstructionChunk {
    i32 cx = 0, cy = 0, cz = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    Model model;                        // one MeshData per material present IN THIS CHUNK
    // PARALLEL TO `model`: which MaterialKind each submesh is. MeshData carries only a display
    // NAME, and reverse-looking-up a kind from a string would break silently the first time a
    // name was reworded. Consumers need the kind to pick the procedural shader surface.
    std::vector<MaterialKind> materials;
    std::vector<MeshRange> ranges;      // (component, submesh) slices within this chunk
    std::vector<ElementRange> elements; // per-unit slices within this chunk
    u32 pieceCount = 0;
};

struct ChunkedSection {
    std::vector<ConstructionChunk> chunks; // sorted by (cx, cy, cz) - deterministic order
    f32 chunkSize = 4.0f;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    u32 TotalPieces() const;
    u32 TotalIndices() const;
    u32 DrawCount() const; // submeshes across all chunks == draw items this structure will cost

    // "Which construction units are in this region?" (brief SS13). Returns chunks whose bounds
    // overlap the query box - the coarse half of an impact query, before per-unit tests.
    std::vector<const ConstructionChunk*> ChunksInBounds(const glm::vec3& mn,
                                                         const glm::vec3& mx) const;
    const ConstructionChunk* ChunkAt(i32 cx, i32 cy, i32 cz) const;
};

// Groups the structure into spatial chunks of `chunkSize` metres and merges each chunk's pieces by
// material.
//
// A piece belongs to the chunk containing its CENTRE - it is never split across chunks. A brick
// straddling a boundary lands wholly in one of them, which keeps every unit addressable exactly
// once. Splitting geometry at chunk borders would produce units that are half in two places, and
// no destruction query could then answer "is this brick present" with a single answer.
void BuildChunked(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                  f32 chunkSize, ChunkedSection& out);

bool ChunkSelfTest();

} // namespace hbe::construction

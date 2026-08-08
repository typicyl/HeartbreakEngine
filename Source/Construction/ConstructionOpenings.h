// Construction/ConstructionOpenings.h - Phase 5: openings, cutters, doors and windows.
//
// NON-DESTRUCTIVE BY CONSTRUCTION, not by discipline. A cutter is a COMPONENT in the definition,
// and geometry is derived from the definition every time. So moving, resizing, disabling or
// deleting a cutter cannot damage the base construction - there is no baked mesh to damage. The
// brief asks for this in SS8; the architecture gives it away for free because the definition was
// always the source of truth.
//
// WHY NOT ClipPoly. The design doc originally said Phase 5 should promote the Sutherland-Hodgman
// clipper out of Fracture.cpp and cut openings with it. That was wrong, and the correction is
// worth keeping here where the code is: ClipPoly is a HALF-SPACE clipper - it keeps one side of a
// plane, producing a convex INTERSECTION. A window opening is convex, but A WALL WITH A HOLE IN IT
// IS CONCAVE, and no sequence of half-space clips will produce it. The premise confused "the
// cutter is convex" with "the result is convex".
//
// What this actually needs is exact convex DECOMPOSITION: subtract the cutter and emit the boxes
// that remain. That is exact rather than approximate, cheaper than clipping, produces clean quads
// with correct UVs, and - the part that matters for a construction system - the pieces it produces
// ARE the real construction: the course under the sill, the course over the head, and the two
// jambs beside the opening.
//
// THE LIMIT THIS LEAVES, stated rather than hidden: cutters are axis-aligned in the HOST's local
// frame. The host may be rotated arbitrarily in the world; the cutter may not be rotated relative
// to the host, and may not be non-rectangular. Every real door and window is square to its wall,
// so this covers architecture; a diagonal slash or a circular porthole needs a real CSG path that
// does not exist in this engine yet.
#pragma once

#include "Construction/ConstructionDef.h"

#include <glm/glm.hpp>

namespace hbe::construction {

// One axis-aligned box remaining after subtraction, in the host's local space.
struct BoxPiece {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f};
};

// Collects every cutter affecting `host`, expressed in HOST-LOCAL space.
//
// A cutter is any Opening component parented to the host. Openings that are hidden (the artist's
// disable) or destroyed contribute nothing - which is exactly what makes "disable this window and
// the wall goes solid again" work with no special case.
// `includeDisabled` is for the CAPACITY path and only that. Generation must respect hidden and
// destroyed openings; a RESERVATION must not, because hidden is a toggle the artist flips and
// destroyed is a state a repair pass undoes. Sizing a reservation while a cutter happens to be
// disabled would refuse the very update that re-enables it - and there is no mesh destroy to
// recover with. Same rule as counting hidden and destroyed COMPONENTS in EstimateCapacity.
void GatherCutters(const ConstructionDef& def, const ConstructionComponent& host,
                   const DamageState* damage, std::vector<BoxPiece>& out,
                   bool includeDisabled = false);

// Subtracts axis-aligned cutters from an axis-aligned box, exactly.
//
// One cutter yields at most 6 pieces (two along each axis); they are non-overlapping and their
// union is exactly the original box minus the cutter. Multiple cutters are applied iteratively,
// so the result stays exact for any number of them.
//
// `out` is cleared. A box entirely consumed by cutters yields nothing, which is correct: a wall
// that is all doorway is not a wall.
void SubtractCutters(const glm::vec3& center, const glm::vec3& extent,
                     const std::vector<BoxPiece>& cutters, std::vector<BoxPiece>& out);

// True when the box is entirely inside some cutter (so it should be dropped whole) - the test a
// masonry unit or a framing member takes before it is emitted.
bool FullyCut(const glm::vec3& center, const glm::vec3& extent,
              const std::vector<BoxPiece>& cutters);

// Shrinks a box to the part of it outside the cutters, when that part is still a single box.
// Returns false when the box is untouched (nothing to do) or when the remainder is NOT a single
// box - in which case the caller should fall back to SubtractCutters.
bool ClipToSingleBox(glm::vec3& center, glm::vec3& extent, const std::vector<BoxPiece>& cutters);

// The framing a timber wall grows AROUND an opening (brief SS6 "headers"). Real construction does
// not simply omit the studs that cross a window: the load they carried has to go somewhere.
//
//   header      - spans the opening, carrying what the cut studs used to carry
//   jack studs  - short studs each side, holding the header up
//   sill        - under a window (absent for a door, which reaches the floor)
//
// Emitted into the same member list as the rest of the framing, so they are addressable and
// damageable like any other member.
struct OpeningFraming {
    bool header = true;
    bool jackStuds = true;
    bool sill = true; // suppressed automatically when the opening reaches the bottom plate
};

// Geometry for a Door or Window component that OCCUPIES an opening: frame, glazing, panel.
// Returns the pieces in the component's own local space.
void BuildFillingGeometry(const ConstructionDef& def, const ConstructionComponent& c,
                          std::vector<BoxPiece>& frame, std::vector<BoxPiece>& panel);

} // namespace hbe::construction

// Construction/ConstructionWood.h - Phase 4: timber framing, boards, siding, shingles.
//
// HOW THIS DIFFERS FROM MASONRY, and why it needed its own file. Masonry is one repeating unit on
// a grid. Wood is a SUB-STRUCTURE of differently-shaped members that each do a different job: a
// stud is not a plate, a joist is not a deck board, a batten covers the seam between two boards.
// So the layout produces members that carry a ROLE, and the role is what a later damage or
// destruction pass will reason about ("a broken stud" is structurally different from "a missing
// siding board").
//
// WHAT STAYS A SOLID BOX, deliberately: beams, columns, headers and posts. A beam IS one piece of
// lumber. Generating it as a stack of boards would be the same "textures pretending to be
// construction" error the brief rejects, only in the opposite direction.
//
// WHY GENERATED MEMBERS ARE NOT GRAPH NODES. A studded wall holds 15-20 studs; a house holds
// hundreds. Promoting each to a ConstructionComponent would put thousands of nodes in the
// structural graph - and `ConstructionDef::Find` is a linear scan, so the graph would go
// quadratic. Members are ELEMENTS, addressable and damageable like bricks, while the wall is the
// graph node. That matches the brief's own rule for destruction granularity: "Only increase
// destruction granularity when necessary." An artist who genuinely wants one joist to be a
// first-class structural member can still author it as a Joist COMPONENT - both paths exist.
//
// The parameter blocks (TimberParams, PlankParams, ShingleParams) live in ConstructionDef.h
// beside MasonryParams, because a component owns them and the definition header must be
// self-contained for serialization.
#pragma once

#include "Construction/ConstructionDef.h"
#include "Construction/ConstructionOpenings.h"

#include <glm/glm.hpp>

namespace hbe::construction {

struct BoardPlacement {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f};
    f32 yaw = 0.0f;   // about local Y
    f32 pitch = 0.0f; // about local X - the slope of a clapboard course or a shingle
    f32 roll = 0.0f;  // about local Z
    MemberRole role = MemberRole::Board;
    u32 index = 0; // position within its role's run

    // STABLE across regeneration. Keyed on (role, index), never emission order, so a damage record
    // naming "stud 7" still means stud 7 after a board is added somewhere else.
    u32 ElementId() const { return (static_cast<u32>(role) << 24) | (index & 0x00FFFFFFu); }
};

enum class WoodResult : u8 {
    Ok = 0,
    DegenerateExtent,
    DegenerateMember,
    MemberCapReached,
    NotWood, // the component is not a wood construction at all
};

// Lays out every generated member for one component. Which members appear depends on the
// component's KIND and MATERIAL together:
//
//   Wall  + TimberFrame            -> plates + studs
//   Floor/Ceiling + TimberFrame    -> joists + a subfloor deck
//   Wall/Siding/Surface + plank    -> boards (+ battens for board-and-batten)
//   Roof/RoofSurface + WoodShingle -> shingle courses laid up both gable slopes
//
// Deterministic: every jitter draws from a stream keyed on (component id, role, index).
// `cutters` (host-local, may be null/empty) are openings punched through the component. Framing
// does NOT simply omit the studs an opening crosses: the load they carried has to go somewhere,
// so a header spans the opening and jack studs carry it down. That is the difference between a
// wall with a hole in it and a wall with a window in it.
WoodResult LayoutWood(const ConstructionDef& def, const ConstructionComponent& c,
                      std::vector<BoardPlacement>& out,
                      const std::vector<BoxPiece>* cutters = nullptr);

// True when LayoutWood would generate anything for this component. Beams, columns, headers and
// posts answer false - they are single pieces of lumber and stay solid boxes.
bool IsWoodConstruction(const ConstructionComponent& c);

} // namespace hbe::construction

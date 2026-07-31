// Scene/StrokeZone.h - WHERE A 3D PAINT STROKE LIVES.
//
// "3D paint strokes" here means the REAL ONES: ordinary lit mesh entities the
// paint tool creates - a quad for a tap, a generated ribbon `.uaf` for a drag.
// They are not StrokeSurface.hlsl (the painterly-3D material feature) and not
// BrushStrokes.hlsl (the 2D post pass); three unrelated systems share the word.
//
// THE RULE, stated once:
//
//   A stroke belongs to the STREAMING ZONE of the surface it was painted on. It
//   is parented under that zone's stroke-group node, and the GROUP NODE ITSELF
//   carries the zone's Tag.
//
// WHY THE GROUP NODE CARRIES THE TAG, AND NOT JUST THE STROKES. A subtree is one
// streaming atom (Scene/TagShard.h). tagshard::Bake resolves ONE tag per atom, and
// an untagged root with several tags below it is a hard ERROR - "it stays RESIDENT
// and none of those tags take effect". So the obvious shape, one global group node
// with per-zone tags on the leaves, does not merely look untidy: it bakes an error
// and changes nothing at runtime. One group node per zone, tagged, is the only
// shape that streams. (The strokes are tagged too, with the SAME id, which the bake
// reads as agreement rather than conflict, and which keeps a stroke's zone legible
// if it is ever unparented.)
//
// WHY A GROUP NODE IS NOT AT THE ORIGIN. The bake gives a meshless row a minimum
// box at its own world position and unions the atom's boxes, so a group parked at
// (0,0,0) would stretch its shard's AABB from the origin to the zone and the
// distance streamer would load that zone from across the map. EnsureGroup anchors a
// new node at the first stroke that needs it, and Attach converts the stroke's
// transform into group-local space so nothing moves on screen.
//
// THE UNTAGGED CASE IS DELIBERATE, AND IT IS A DEAD END. Paint on terrain or on any
// untagged geometry resolves to kTagUntagged, lands in the plain "Paint Strokes"
// group, and is PERMANENTLY RESIDENT: tags::Normalize force-pins index 0
// (`alwaysLoaded = true`, "cannot be un-pinned"), so the untagged bucket cannot be
// streamed or sharded at all. That is correct for a floor that is itself always
// loaded, and it means paint on terrain has no streaming story. The remedy is the
// authoring rule that already applies - TAG BY PLACE, NOT BY TYPE - not code here.
//
// A STROKE IS NEVER NAV GEOMETRY. Strokes carry Transform + MeshRef, which is
// exactly the view GridNav collects, so before this file a stroke painted on a wall
// was a wall and a stroke on the floor was a step. GridNav::NavFilterAccepts calls
// IsStroke and rejects them, in every filter mode. It does NOT stamp NavmeshInput
// on a stroke to achieve that: a scene whose only NavmeshInput components sat on
// strokes would flip ChooseNavFilter to the Input filter and empty the navmesh for
// the whole level.
//
// ONE GROUP PER (ZONE, SCENE FILE, SHARD CELL). The zone is the identity; the other
// two exist because a single group node would otherwise be WRONG rather than untidy:
//
//   * SCENE FILE. Groups are found by component, across the whole registry, and an
//     additively streamed `.hbscene`'s entities carry a SceneSource. Without keying on
//     it, painting in scene A while scene B is streamed in parents the new stroke
//     under B's group - and Ctrl+S then writes A without the group, so EntityToJson
//     drops the parent link and the stroke reloads as a root at its GROUP-LOCAL
//     transform. A silent teleport, the same one Editor::Reparent guards with
//     MoveToScene.
//   * SHARD CELL, and only for a tag with autoShard ON. A root is one atom, and
//     tagshard::Bake unions every grid cell an atom's box touches into one component.
//     One group per zone spans the whole zone the moment two strokes are painted at
//     opposite ends, which collapses the tag's auto-shards into a SINGLE shard and
//     silently un-does the per-shard streaming for every prop in it. Per-cell group
//     nodes keep each atom inside one cell; the bake's own 8-neighbour merge then
//     rejoins whatever genuinely belongs together.
//
// THE ZONE IS THE NODE'S OWN `Tag`, NOT A COPY OF IT. `StrokeGroup` is a bare MARKER.
// It used to duplicate the TagId, and nothing kept the copy in sync: tags::RemoveTag
// remaps `Tag` and tags::AssignSubtree rewrites `Tag`, so deleting an unrelated tag
// or re-tagging the node from the Inspector left a group whose marker said "Mill" and
// whose Tag said something else - after which a terrain stroke could be parented into
// a streaming atom and despawn with it. Deriving removes the invariant instead of
// adding a third site that has to be kept in lockstep.
//
// WHAT THIS DOES NOT DO, stated plainly:
//   * A stroke drawn across two zones picks the zone of its FIRST hit and pops with
//     it. One ribbon is one mesh is one entity is one tag; there is no splitting.
//   * A stroke does not re-home itself when the surface's tag changes later. Its
//     zone is baked at paint time. Rehome() is the artist-triggered remedy.
//   * A stroke does not follow the object it was painted on. The overwhelmingly
//     common surface is static level geometry, and EntityUnderPixel resolves to the
//     nearest AABB - which is another STROKE when you paint over one - so default
//     parenting would build accidental chains.
//   * Attach converts POSITION only. Correct at creation (a group is minted with an
//     identity basis) but a group an artist has ROTATED or SCALED will turn its
//     strokes with it when Rehome moves one into it - which is what a group is for,
//     and is stated here rather than silently true.
#pragma once

#include "Scene/Components.h" // TagId, kTagUntagged, StrokeGroup

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe {

class Scene;

namespace strokezone {

// The name of the pre-zone global group node, and still the name of the untagged
// one. Kept as a constant because a node with this name that carries NO StrokeGroup,
// no Tag and no mesh is recognised as a LEGACY group - which is how a scene authored
// before this file keeps collecting its untagged strokes in the node the artist
// already sees, and how its strokes are excluded from the navmesh before anybody has
// painted anything.
inline constexpr const char* kUntaggedGroupName = "Paint Strokes";

// How far outside a zone's combined box a stroke may sit and still count as inside
// it. Every stroke is deliberately created OFF its surface: a tap is lifted
// 0.02 + up to 0.012 m along the hit normal and a ribbon up to 0.012 m along the draw
// plane's. Paint on the outer face of the wall that DEFINES a zone's box is therefore
// a few centimetres outside it, and a pure containment test would answer "no zone".
inline constexpr f32 kStrokeLiftSlack = 0.05f;
// The stricter band a stroke must clear before Rehome will DEMOTE it out of a real
// zone into the always-resident untagged group. Demotion is the one re-home that
// loses streaming, and it is unrecoverable without re-tagging, so it takes hysteresis
// rather than the same threshold that admits.
inline constexpr f32 kStrokeDemoteSlack = 0.5f;

// Display name of a zone's group node. Cosmetic only: the group is found by its
// StrokeGroup component, never by name, so renaming it in the Hierarchy no longer
// forks a second group the way the old name scan did.
std::string GroupName(TagId zone);

// --- Resolution ---------------------------------------------------------------
// The zone of the surface a stroke was started on. Walks UP the Parent links, so a
// hit on a child mesh of a tagged prop resolves to the prop's tag. Untagged (and
// kTagUntagged) for an invalid handle.
TagId ZoneOfSurface(const entt::registry& reg, entt::entity hit);

// The zone of a world POSITION, resolved against each tag's COMBINED world bounding
// box - containment only, smallest containing box wins, no nearest-match fallback
// (a fallback would drag a terrain stroke into whichever zone happened to be
// closest). Stroke groups and their strokes are excluded from the boxes, so the
// answer cannot be defined by the thing being re-homed. This is for Rehome, not for
// authoring: at paint time the surface hit is the better answer and it already
// exists. `slack` expands every box (see kStrokeLiftSlack - a stroke sits OFF its
// surface, so exact containment answers "no zone" for paint on a zone's own boundary).
TagId ZoneOfPosition(const Scene& scene, const glm::vec3& worldPos, f32 slack = 0.0f);

// --- The group nodes ----------------------------------------------------------
// The zone a group node collects: its own Tag, or kTagUntagged when it has none.
// (The StrokeGroup component is a bare marker - see the header comment.)
TagId GroupZone(const entt::registry& reg, entt::entity group);

// True for a node that collects strokes: one carrying StrokeGroup, or a LEGACY node
// (named "Paint Strokes", no mesh, no StrokeGroup, no Tag) from a scene authored
// before this file. Recognising the legacy shape without mutating anything is what
// makes the navmesh exclusion and the re-home menu work on a scene that has never
// been painted in since the upgrade.
bool IsGroupNode(const entt::registry& reg, entt::entity e);

// The existing group node for `zone`, or entt::null. First match in any scene file
// and any shard cell - for callers that just want "a group of this zone" (tests,
// diagnostics). Authoring goes through EnsureGroup, which keys on all three.
entt::entity FindGroup(const entt::registry& reg, TagId zone);

// Find-or-create the group for (`zone`, `sceneSrc`, the shard cell `anchor` falls
// in). A created node gets an identity-basis Transform at `anchor` (see the header
// comment on why not the origin), the StrokeGroup marker, the requesting content's
// SceneSource, and - for a real zone - the zone's Tag. For kTagUntagged it first
// adopts a legacy name-matched node in the same scene.
entt::entity EnsureGroup(Scene& scene, TagId zone, const glm::vec3& anchor,
                         const std::string& sceneSrc = std::string());

// Parents `stroke` under the group for `zone` and tags it. `stroke` must be a ROOT
// carrying a Transform (which is how both creation sites leave it): its transform
// is converted into the group's local space, so the stroke does not move. The group
// is chosen for the stroke's OWN scene file, so this never creates a cross-file
// parent link. Returns false only when `stroke` is not usable.
bool Attach(Scene& scene, entt::entity stroke, TagId zone);

// True when `e` is a stroke - i.e. a child of a group node. There is no per-stroke
// marker component: membership IS the parent link, which is also what makes a
// despawn drag the strokes along with their group.
bool IsStroke(const entt::registry& reg, entt::entity e);

// Every stroke in the scene, grouped-node children flattened, in hierarchy order.
std::vector<entt::entity> AllStrokes(const entt::registry& reg);

// True when the scene holds any group node at all, legacy ones included. The Scene
// menu's enable predicate: gating on the StrokeGroup pool alone greyed the re-home
// item out on exactly the legacy scenes it is the migration path for.
bool HasAnyGroup(const entt::registry& reg);

// Stamps the StrokeGroup marker on every legacy group node. Idempotent; returns how
// many were adopted. Called by Rehome so the migration path works on a scene where
// nobody has painted since the upgrade. NOT called at load: stamping a component on
// load would bake derived state into the next save.
usize AdoptLegacyGroups(Scene& scene);

// RE-HOME: re-resolve every stroke by POSITION and move it into the right zone's
// group. The remedy for "the surface's tag changed after the paint was done", and
// the (artist-triggered, undoable) migration path for strokes authored before
// zones existed. Also re-splits a group that has grown across shard cells. Returns
// how many strokes moved. Never deletes anything - an emptied group node is left in
// place, because it may be the one the artist named.
usize Rehome(Scene& scene);

// --test-strokezones: the headless proof of everything above. Surface-hit and
// position resolution, per-zone grouping and legacy adoption, the untagged
// fallback staying resident, a save/load round trip that is a fixed point, a shard
// bake with ZERO errors (the cross-shard-parent case this design exists to avoid),
// a full despawn/respawn cycle that restores a stroke's mesh, material, bounds,
// transform, parent and tag intact, and the navmesh exclusion. No GPU, no window,
// no project.
bool SelfTest();

} // namespace strokezone
} // namespace hbe

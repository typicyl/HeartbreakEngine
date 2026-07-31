// Scene/TagTable.h - the process-wide STREAMING TAG table.
//
// A tag is authored as a NAME and lives in the ECS as a 2-byte interned id
// (Components.h `struct Tag`). This file owns the one mapping between the two,
// plus the small set of rules that keep the mapping honest:
//
//   * Index 0 is ALWAYS "Untagged" - always resident, never streamed, and the
//     meaning of an entity with no Tag component at all.
//   * A tag's index in ProjectSettings::tags IS its TagId. SeedFromProject
//     installs the authored order, so ids are stable for a given `.hbproj`.
//   * The table is RESET AND RESEEDED on project open. The `.hbproj` parser
//     already documents why (Project.cpp: the same `settings_` member is reused
//     across in-process project switches, which is why it clears inputActions);
//     a stale tag table across a switch would silently mis-map every tag id.
//   * A name a scene references but the project does not list is AUTO-INTERNED,
//     never silently folded into Untagged - that would move content into the
//     always-resident set without telling anyone. The Tags panel surfaces those.
//
// THREADING: the table is a plain vector + map with NO synchronisation. Intern
// mutates it, so Intern/Reset/SeedFromProject/RemoveTag are MAIN THREAD ONLY.
// scene::Instantiate is main-thread by contract and is the only loader site that
// touches this; StageAssets runs on job workers and must never call in here.
#pragma once

#include "Project/Project.h" // TagDef (the authored per-tag streaming config)
#include "Scene/Components.h" // TagId, kTagUntagged, Tag, UIDocMember

#include <entt/entt.hpp>

#include <string>
#include <vector>

namespace hbe::tags {

// The reserved name of tag index 0.
inline constexpr const char* kUntaggedName = "Untagged";

// --- The table ---------------------------------------------------------------
// Registers `name` on a miss, returns the existing id on a hit. An empty name is
// Untagged. Main thread only.
TagId Intern(const std::string& name);
// "" for an id no table entry exists for (never for a live Tag).
const std::string& Name(TagId id);
// kTagUntagged when `name` is not interned - which is also the id of "Untagged"
// itself. Use for lookups where "absent" and "untagged" mean the same thing.
TagId Find(const std::string& name);
// Back to exactly {"Untagged"}. Main thread only.
void Reset();
// Reset + install the authored order, so TagId == index into `defs`. Call on
// project open (and after any edit to the project's tag list). Main thread only.
void SeedFromProject(const std::vector<TagDef>& defs);
// Every interned name, indexed by TagId. Never empty (index 0 is "Untagged").
const std::vector<std::string>& All();

// --- The authored list -------------------------------------------------------
// Enforces every invariant on a parsed/edited tag list, in place:
//   * index 0 is "Untagged" with alwaysLoaded forced on (an authored row of that
//     name is adopted for its other fields, but cannot move or be un-pinned),
//   * nameless rows and duplicate names are dropped (first row of a name wins),
//     because a duplicate would collapse two ids onto one under SeedFromProject,
//   * loadRadius is clamped non-negative and the hysteresis band is enforced via
//     salvage::EnforceHysteresis - corrected unconditionally, with a warning,
//     exactly as the `.hbworld` manifest parser did.
void Normalize(std::vector<TagDef>& defs);

// Makes the authored list agree with the LIVE table: after this, defs[i].name ==
// All()[i] for every interned tag. Appends a default-configured row for each tag
// that was auto-interned from a scene file but never listed in the project, and
// interns any listed-but-unknown row.
//
// WHY IT IS MANDATORY BEFORE ANY LIST EDIT. Ids are indices, so an edit that
// re-seeds the table from a list SHORTER than the table silently drops the
// auto-interned tags - and an entity left holding a dropped id has no name, so
// EntityToJson writes no "tag" for it and the object quietly leaves its streaming
// group on the next save. Reconciling first makes the re-seed lossless. Only
// appends, so every existing id keeps its value.
void ReconcileWithTable(std::vector<TagDef>& defs);

// Removes tag `index` from `defs` AND remaps every live entity so the ids still
// line up with the new authored order: entities on the removed tag become
// Untagged (their Tag component is dropped), and every id above `index` shifts
// down by one. Re-seeds the table. Returns false - changing nothing - for index
// 0 (Untagged is undeletable) or an out-of-range index.
//
// Without the remap, deleting a row would silently repoint every entity above it
// at its neighbour's tag, which is the one failure an index-based id invites.
bool RemoveTag(entt::registry& reg, std::vector<TagDef>& defs, usize index);

// --- Assignment (the enforced mutation site) ---------------------------------
// False for a `.hbui` DOCUMENT entity (and for an invalid handle). UI is asset
// content: it is outside the streamed world entirely, it is excluded from every
// scene write, and both Replace sweeps spare it - a Tag on one would describe a
// streaming group that can never spawn or despawn.
bool Taggable(const entt::registry& reg, entt::entity e);

// THE ONE MUTATION SITE for Tag. Refuses (false, nothing written) when
// !Taggable. Assigning kTagUntagged REMOVES the component rather than storing
// id 0, so "no Tag" and "Tag{0}" can never disagree.
bool Assign(entt::registry& reg, entt::entity e, TagId id, i32 shard = -1);

// Assigns `root` and every descendant (depth-first over the Parent links).
// Returns how many entities were tagged.
//
// This is NOT a convenience. A shard owns whole subtrees: the scene file stores
// `parent` as an index into its own entity array, so a partial write leaves a
// child whose parent did not survive, and the salvaged remap turns that into a
// ROOT - i.e. the child renders at its LOCAL transform in world space, a silent
// teleport. Tagging subtree-wide is what makes that case not arise.
usize AssignSubtree(entt::registry& reg, entt::entity root, TagId id, i32 shard = -1);

// Headless proof of everything above (--test-tagtable): interning round-trips,
// Untagged is index 0 and undeletable, the hysteresis band is enforced, a
// project tag list round-trips through `.hbproj` (including present-but-empty
// and a repeated parse into the same settings), a per-entity tag survives
// save/parse/save byte-identically, subtree assignment propagates, RemoveTag
// remaps live entities, and a `.hbui` document entity cannot be tagged. No GPU,
// no window.
bool SelfTest();

} // namespace hbe::tags

// Scene/EntityGuid.h - stable per-entity identity (see `struct Guid` in Components.h).
//
// WHY: entity identity used to be the Name string, which is neither unique nor
// stable, and entt handles, which are recycled and meaningless across a load.
// Anything that persists per-entity state across a save/load - world:: area
// state, checkpoint saves, and (next) tag streaming's despawn/respawn - needs a
// key that means "this exact object". That key is a 64-bit guid.
//
// DESIGN, in four rules:
//
//  1. MINTED IN ONE PLACE. Scene::CreateEntity emplaces a Guid on every entity
//     it creates. It is the only wrapper around registry_.create() in the tree,
//     so every authored / loaded / spawned entity is covered. (The three raw
//     reg.create() calls in Engine.cpp are transient runtime UI - dialogue
//     choice buttons, the interact prompt and its icon - which the serializer
//     already refuses to write; they carry no guid and need none.)
//
//  2. ADOPTED ON LOAD. scene::Instantiate overwrites the freshly minted guid
//     with the one parsed from the file, when there is one.
//
//  3. UNIQUE AMONG LIVE ENTITIES. Adoption goes through a Claim, seeded from
//     the guids already live in the registry. A parsed guid that is already
//     taken is NOT adopted - the entity keeps a fresh mint instead. This is what
//     makes a spawner bursting twenty copies of one prefab, or the same scene
//     file loaded additively twice, produce twenty (or two) distinct objects
//     rather than twenty aliases of one.
//
//  4. FRESH ON DUPLICATION. The editor's copy/paste, duplicate, prefab create
//     and prefab instantiate all funnel through SaveSubtreeToString ->
//     BuildSubtreeJson, which deliberately does NOT write the guid key (same
//     precedent as `sceneSrc`). Nothing to adopt, so rule 1's mint stands.
//     Undo/redo, play-mode snapshots and `.hbsave` go through BuildSceneJson
//     instead, which DOES write it - those are restores of the same objects,
//     not copies.
//       EXCEPTION: prefab REVERT re-pastes from the same guid-less fragment but
//     is a restore, not a copy - the instance keeps its position, parent and
//     prefab link. Editor::RevertPrefabInstance therefore captures the root's
//     guid before destroying it and stamps it back onto the fresh root.
//       BuildSceneJson also refuses `Persistent` entities (the resident UI layer
//     loaded once at boot). They are exactly the set LoadMode::Replace spares,
//     so serializing them would restore a second, freshly-minted copy of the
//     whole UI on top of the surviving one.
//
// PRE-GUID FILES (the migration question). Every `.hbscene` on disk today
// predates this field. A random mint on load would rebind every saved row to a
// different object on the next launch, so the fill must be DETERMINISTIC:
// ParseSceneFile derives a guid for each guid-less entity from
// hash(file identity) mixed with the entity's INDEX in the file's `entities`
// array. Same file, same bytes, same order -> same guids, every load, on every
// machine. The moment the editor saves that scene the derived guids are written
// out as real ones and the derivation never runs on it again.
//   * The file identity is the path RELATIVE TO the project's Assets dir when it
//     is under it (so moving or copying the project preserves guids), falling
//     back to the filename. Lowercased, forward slashes - Windows paths reach
//     this from both the editor and the VFS with different casing/separators.
//   * The derivation is only applied to `.hbscene`. A `.hbprefab` is a TEMPLATE
//     that gets instantiated repeatedly; its entities must always mint fresh.
//   * The caveat, stated plainly: inserting or reordering entities in a pre-guid
//     file shifts the indices after the insertion point, so their derived guids
//     shift too. That is unavoidable without a field in the file, it only
//     affects files that have never been saved by a guid-aware editor, and it is
//     exactly why the fill is a migration and not a permanent mechanism.
#pragma once

#include "Scene/Components.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

namespace hbe::guid {

// A fresh, random, process-unique guid. Never returns 0.
u64 Mint();

// Deterministic per-file seed. `path` is normalized (relative to the active
// project's Assets dir when possible, lowercased, forward slashes) before
// hashing, so the same asset in the same project always seeds the same.
u64 SeedFromPath(const std::filesystem::path& path);

// Deterministic guid for entity `index` of the file identified by `seed`.
// Never returns 0.
u64 Derive(u64 seed, u32 index);

// 16-char lowercase hex. A string, not a JSON number: 64-bit integers are the
// one thing JSON tooling reliably mangles, and this key must round-trip exactly.
std::string ToHex(u64 g);
// Parses ToHex output. Returns 0 (= unset) on anything malformed.
u64 FromHex(std::string_view s);

// Enforces rule 3. Seed from the registry, then Resolve() each parsed guid.
class Claim {
public:
    Claim() = default;
    // Seeds from every guid currently live in `reg`.
    explicit Claim(const entt::registry& reg);

    // Returns `parsed` when it is non-zero and not already taken (and marks it
    // taken); otherwise mints a fresh guid. The result is always non-zero and
    // always unique within this Claim.
    u64 Resolve(u64 parsed);

    usize Size() const { return used_.size(); }

private:
    std::unordered_set<u64> used_;
};

// Stamps the guid an instantiated entity should carry (rules 2 + 3). This is the
// whole of scene::Instantiate's guid handling; the self-test drives this exact
// function so what it proves is the shipping path, not a copy of it.
void Apply(entt::registry& reg, entt::entity e, u64 parsed, Claim& claim);

// Headless proof of the whole contract (--test-entityguid). No GPU, no window.
bool SelfTest();

} // namespace hbe::guid

// Scene/TagShard.h - the SAVE-TIME SPATIAL SHARD BAKE for streaming tags.
//
// THE PROBLEM THIS SOLVES. An author's unit is the TAG: a semantic label like
// "Props" or "Enemies". A streamer's unit has to be SPATIAL. A tag applied to 400
// crates spread over a whole level has a level-sized bounding box, so a literal
// "one tag = one streaming group" either always loads it or never does - the
// feature does nothing, and that scattered case is the COMMON one, because tags are
// semantic while streaming is geometric.
//
// So: the author tags. The bake splits each tag into N spatially-coherent SHARDS at
// save time, and the shard is what the runtime measures distance to. Authoring
// never sees it except as a diagnostic. One tag may become several shards; a tag
// that really is one place stays one shard.
//
// THE BAKE RESULT *IS* THE FILE. Every entity's shard index is stamped into its
// `Tag` component and written to the scene file ("shard"), and each shard's world
// AABB goes in the file header ("tagShards" -> scene::ShardDesc). The runtime never
// re-clusters. An editor clustering and a runtime clustering that could disagree is
// a whole class of bug, and this removes it by construction.
//
// A SUBTREE IS ONE ATOM. Only ROOTS are clustered, and a root's shard is stamped on
// its entire subtree. That is not a shortcut: `EntityData::parent` is an index into
// the file's own entity array, so a shard that owned only part of a subtree would
// load a child whose parent row is outside the slice - which the loader deliberately
// turns into a ROOT (SceneSerializer.h, "CROSS-SLICE PARENT"), i.e. the child
// renders at its LOCAL transform in world space. A silent teleport. Whole-subtree
// ownership is what makes that case not arise, and tags::AssignSubtree is the
// authoring half of the same rule.
//
// DETERMINISM IS A REQUIREMENT, not a nicety: the bake output is content that gets
// diffed and committed. There is no RNG and no iteration over a hash container
// anywhere in Bake - every grouping step goes through explicitly sorted keys, so
// the same scene always bakes to the same bytes regardless of entity creation
// order, registry layout or allocator addresses.
#pragma once

#include "Project/Project.h"  // TagDef (per-tag streaming config: radii, autoShard, cell)
#include "Scene/Components.h" // TagId, kTagUntagged
#include "Scene/SceneSerializer.h" // scene::ShardDesc / scene::SceneData

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace hbe {

class Scene;

namespace tagshard {

// Padding added around every shard AABB, to absorb small authoring drift (and, at
// runtime, the fact that a physics-driven object inside a shard can move after the
// bake without the AABB following it).
inline constexpr f32 kBoundsPad = 1.0f;
// Half-extent given to an entity that declares no bounds at all (no AABB, no
// volume): a bare marker becomes a 2 m box rather than a zero-volume point, so it
// cannot fall exactly on a cell boundary and cannot produce a degenerate shard.
inline constexpr f32 kMinHalfExtent = 1.0f;
// Floor on the clustering cell, for a tag whose loadRadius is tiny or zero. Without
// it a 0 m radius would produce one shard per object.
inline constexpr f32 kMinShardCell = 8.0f;
// A tag applied to tens of thousands of instances must not become tens of thousands
// of streaming units: past this, shards are merged smallest-first (with a warning).
inline constexpr u32 kMaxShardsPerTag = 256;
// Bound on the cells one root may occupy (a root far larger than the cell size).
// Past it the range is strided rather than enumerated - clustering quality only;
// such a root's tag is flagged as spread anyway.
inline constexpr u32 kMaxCellsPerRoot = 4096;
// A tag that bakes to ONE shard whose diagonal exceeds this many load radii is
// effectively always-loaded. The author has to be told, because they believe they
// enabled streaming.
inline constexpr f32 kSpreadRadiiWarn = 4.0f;
// Two shards of one tag whose intersection exceeds this fraction of the smaller
// one's volume are not really two streaming units - crossing either boundary pulls
// in both.
inline constexpr f32 kOverlapWarnFraction = 0.5f;

// --- Diagnostics --------------------------------------------------------------
// Errors are REPORTED, NOT SAVE-BLOCKING, and that is deliberate. The bake cannot
// emit an inconsistent file: it stamps whole subtrees, so the cross-shard parent it
// reports has already been RESOLVED in the output (the child rides its root's
// shard). Refusing the save would therefore throw away authored work over a
// condition that no longer exists in the thing being written. What the author needs
// is to know the tag they set on that child is being ignored - which is what the
// report says.
enum class Severity { Error, Warning };

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string tag;     // tag the finding is about ("" = not tag-specific)
    std::string message; // one line, author-facing
};

// Per-tag authoring diagnostics - the numbers that make the scattered-tag failure
// mode VISIBLE while authoring instead of at runtime.
struct TagStat {
    std::string tag;
    TagId id = kTagUntagged;
    u32 shards = 0;
    u32 members = 0;         // entities, subtrees included
    u32 roots = 0;
    f32 loadRadius = 0.0f;
    f32 cell = 0.0f;         // clustering cell actually used
    f32 largestDiagonal = 0.0f;
    // sum(shard volume) / bounding volume of the whole tag. 1.0 = one solid blob
    // (nothing gained by sharding); small = well separated islands.
    f32 coherence = 1.0f;
    bool alwaysLoaded = false;
    bool autoShard = true;
};

// --- Bake input ---------------------------------------------------------------
// One row of the thing being baked, deliberately NEUTRAL: it is neither a registry
// entity nor an EntityData. That is what lets one algorithm serve the editor save
// path (rows built from the live registry) and the headless tests / any SceneData
// path, with no second implementation to drift.
//
// `parent` is an index into the SAME row vector, or -1 for a root (which includes
// "my parent exists but is not part of what is being saved").
struct BakeRow {
    i32 parent = -1;
    TagId tag = kTagUntagged;
    glm::mat4 world{1.0f};          // world matrix (Parent chain already composed)
    glm::vec3 localMin{0.0f};       // LOCAL-space extent; equal min/max = no bounds
    glm::vec3 localMax{0.0f};
    bool hasExtent = false;
    std::string name;               // diagnostics only (names are not identity)
    bool uiDoc = false;             // a `.hbui` document entity (may never be tagged)
    bool navInput = false;          // SceneLayer{Static} + a mesh = real navmesh input
    // A chunked TerrainComponent. Diagnostics only, and it is a VRAM statement rather
    // than a correctness one: terrain::Sync re-uploads every chunk mesh when the
    // component respawns (renderer.UploadMesh, no cache key, and the RHI has no mesh
    // destroy - see TagStreaming.h), so each streaming cycle of a tagged terrain ADDS
    // VRAM. Paint canvases and world-UI render targets are cached across respawns for
    // exactly this reason; terrain chunks are not.
    bool terrain = false;
    i32 shard = -1;                 // OUT: the baked shard index (-1 = untagged)
    // OUT: the tag the row's whole SUBTREE was resolved to (kTagUntagged = resident).
    //
    // WHY THIS EXISTS AND WHY WRITING IT BACK IS NOT OPTIONAL. A subtree is one atom,
    // so the bake stamps the ROOT's resolved tag onto every member and counts every
    // member in ShardDesc::count - members that were never tagged themselves (the
    // PROMOTE branch: the author tagged a child, so the untagged root joins the group)
    // and members that carried a DIFFERENT tag (the cross-shard-parent branch: the
    // descendant rides its root's shard and its own tag is ignored). If only `shard`
    // is written back, those rows carry no `tag`/`shard` in the file at all, or carry a
    // shard index belonging to a different tag's shard space - so tagshard::FromParsed
    // finds fewer members than the header claims, marks the WHOLE table untrusted, and
    // the level stops streaming entirely with a "re-save the scene" warning on a file
    // that was just saved. Excluding those rows from `count` instead is NOT the fix:
    // that splits the subtree across slices, which is the silent-teleport case
    // whole-subtree ownership exists to prevent.
    //
    // kTagUntagged is written for a subtree that stays resident, INCLUDING the
    // several-tags-under-one-untagged-root conflict. The caller must not remove an
    // authored Tag on that reading - the report says the tags do not take effect, not
    // that the author's work is deleted.
    TagId resolvedTag = kTagUntagged;
};

struct BakeReport {
    // What lands in the scene file header, in write order (tag id, then shard index).
    std::vector<scene::ShardDesc> shards;
    // Parallel to `shards`: the row indices each shard owns (subtrees included).
    std::vector<std::vector<u32>> members;
    std::vector<Diagnostic> diagnostics;
    std::vector<TagStat> stats;
    u32 errors = 0;
    u32 warnings = 0;
    u32 tagged = 0; // rows that came out with a shard
};

// Clusters every tagged row into shards, stamps `BakeRow::shard`, and validates.
// Deterministic. `defs` is the project's tag list (index == TagId); a tag id past
// the end of it - a scene-only tag nothing in the project lists - is baked with
// TagDef defaults rather than dropped, because dropping it would silently move
// content into the always-resident set.
BakeReport Bake(std::vector<BakeRow>& rows, const std::vector<TagDef>& defs);

// --- Adapters -----------------------------------------------------------------
// Rows for the entities of a PARSED scene file (headless; no registry, no GPU).
// World matrices are composed from the file's parent links; extents come from
// EntityData::aabb, else from whatever volume the entity's components declare.
// MAIN THREAD ONLY: it interns the file's tag NAMES (tags::Intern mutates the table).
std::vector<BakeRow> Rows(const scene::SceneData& data);

// Rows for the LIVE registry, restricted to the entities `include` accepts AND that
// the scene writer actually writes (scene::IsSerializedEntity - the shard member
// counts are cross-checked against the file, so the two sets must agree).
// `handlesOut` comes back parallel to the returned rows.
std::vector<BakeRow> Rows(const Scene& scene,
                          const std::function<bool(entt::entity)>& include,
                          std::vector<entt::entity>& handlesOut);

// THE EDITOR SAVE-PATH ENTRY POINT. Bakes the live scene and stamps every included
// entity's `Tag::shard`, so the following SaveScene writes the shard indices out.
// Call it BEFORE SaveScene and hand SaveScene the returned report's `shards` - the
// same relationship scene::SavePaintCanvases already has with SaveScene.
//
// Mutates Tag through tags::Assign, the one enforced mutation site, writing back BOTH
// halves of the bake's decision (`BakeRow::resolvedTag` and `BakeRow::shard`) so the
// file it is about to produce is self-consistent - see BakeRow::resolvedTag for why
// writing only the shard index silently disables streaming for the whole level. It
// therefore CAN add a Tag to a promoted subtree's untagged members and CAN rewrite a
// descendant's tag to its root's; it never removes one (a resident subtree keeps
// whatever the author set, with its shard cleared to -1).
BakeReport BakeScene(Scene& scene, const std::vector<TagDef>& defs,
                     const std::function<bool(entt::entity)>& include = {});

// --- Runtime side -------------------------------------------------------------
// Rebuilds shard membership from a parsed scene: buckets the entities by their
// per-entity (`tag`, `shard`) and matches them against the header's ShardDescs.
//
// `trusted` is false when the two disagree in ANY way - a header row with no
// entities, entities with a shard index no header row describes, or a member count
// that does not match. An untrusted set MUST be treated as "everything always
// loaded": a stale or hand-edited header has to degrade to correct-but-unstreamed,
// never to missing content.
struct ParsedShards {
    std::vector<scene::ShardDesc> shards;
    std::vector<std::vector<u32>> members; // parallel: entity rows in the file
    bool trusted = true;
    std::string reason; // why it is untrusted (empty when trusted)
};
ParsedShards FromParsed(const scene::SceneData& data);

// Pairs of shards (indices into `shards`) whose AABBs intersect by more than
// kOverlapWarnFraction of the smaller one's volume. Two such shards are not really
// two streaming units - crossing either boundary pulls in both - so the split costs
// evaluation and buys nothing.
//
// Exposed rather than buried inside Bake because it has to be testable on
// hand-built shards: the grid clustering makes heavy overlap HARD to produce (any
// two occupied cells that touch are unioned), so a test that could only reach this
// through Bake would be asserting on a case the algorithm mostly prevents. Bake
// calls it per tag; --test-shardbake calls it both ways.
std::vector<std::pair<u32, u32>> HeavyOverlaps(const std::vector<scene::ShardDesc>& shards);

// Distance from a point to a shard's baked AABB (0 inside). This is the measurement
// the streamer will use, and it lives here rather than in the streamer because the
// bake's padding is part of its definition. Deliberately distance-to-BOX, not
// distance-to-centre: a 300 m wall's centre is 150 m away while the player stands
// against it.
f32 DistanceToShard(const scene::ShardDesc& s, const glm::vec3& p);

// Headless proof of everything above (--test-shardbake): sharding is deterministic
// under shuffled input, every tagged entity lands in exactly one shard, a scattered
// tag produces many shards while a clustered one produces a single shard, shard
// AABBs actually contain their members (checked against a brute-force union,
// including meshless volume-only entities), whole subtrees ride their root, a
// cross-shard parent is reported at BAKE time, heavy overlap and pathological
// spread are reported, the per-tag shard cap merges instead of exploding, and the
// file header round-trips and cross-checks. No GPU, no window.
bool SelfTest();

} // namespace tagshard
} // namespace hbe

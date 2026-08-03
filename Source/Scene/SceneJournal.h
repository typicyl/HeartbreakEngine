// Scene/SceneJournal.h - the bridge between a live scene and a collaboration journal.
//
// This is the ONE place that knows about both. Source/Collab/ deliberately includes
// nothing from the engine (so a headless server can host history with no GPU, no entt
// and no glm), and Source/Scene/ knows nothing about commits. Everything that has to
// understand both meets here and nowhere else - if this file's include list grows a
// third world, the layering has been lost.
//
// WHAT A SAVE MEANS NOW. The `.hbscene` on disk is a WORKING COPY. Saving diffs the
// live scene against the snapshot taken when it was opened (or last committed), seals
// the difference as a commit, and appends it to the document's journal. Going offline
// changes nothing about that - a commit is valid because it is well-formed and names
// its parent, not because a server blessed it.
#pragma once

#include "Collab/Journal.h"
#include "Scene/SceneSerializer.h"

#include <map>
#include <string>

namespace hbe::scene {

// Every (entity guid, component) this build can journal, and its serialized value.
// Ordered so two snapshots of the same content produce the same commit regardless of
// registry iteration order - a commit whose change ORDER depends on entt internals
// would diff differently on two machines holding identical scenes.
using JournalSnapshot = std::map<std::pair<u64, std::string>, std::string>;

// Which entities belong to the document being journalled. Empty = all of them.
//
// THIS IS NOT OPTIONAL POLISH. One editor save writes several files: the active scene,
// and one per additively-streamed scene (entities carrying SceneSource). A snapshot that
// walks the whole registry therefore mixes objects from SEVERAL documents into one
// document's commit, and the other side would apply another file's edits to this one.
// The caller passes the SAME predicate it uses to decide what to write, so the commit
// and the file cannot describe different sets.
//
// It takes the registry rather than capturing one, so the predicate is STATELESS and can
// be stored across frames. A lambda holding a `registry&` would outlive the world it
// referred to the first time a scene was replaced.
using JournalFilter = std::function<bool(const entt::registry&, entt::entity)>;

// Captures every journalable component of every guid-bearing entity the filter admits.
//
// Entities with no guid are SKIPPED, not invented: a guid is what makes an entity the
// same object on two machines, and journaling one without it would record a change
// nobody else can apply.
JournalSnapshot SnapshotForJournal(const Scene& scene, const JournalFilter& include = {});

// Diffs two snapshots into a commit body. Deterministic: same inputs, same order.
std::vector<collab::Change> DiffSnapshots(const JournalSnapshot& before,
                                          const JournalSnapshot& after);

// Applies one commit's changes to a live scene, resolving entities BY GUID.
//
// Returns the number applied. A change naming a guid this scene does not contain is
// SKIPPED and counted in `outMissing` rather than silently dropped - that is the
// signal that the two sides disagree about what exists, which a caller must surface
// instead of pretending the merge was clean.
usize ApplyCommit(Scene& scene, const collab::Commit& c, usize* outMissing = nullptr);

// Applies every commit of an accepted plan, in order.
usize ApplyMergePlan(Scene& scene, const collab::MergePlan& plan, usize* outMissing = nullptr);

// What a person decided about one conflict.
enum class Resolution : u8 {
    Undecided = 0, // the default, deliberately: an unanswered question is not an answer
    KeepMine,
    TakeTheirs,
};
const char* ResolutionName(Resolution r);

// Applies a NeedsReview plan once a human has decided EVERY conflict.
//
// This is the ONLY way conflicted work lands. ApplyMergePlan refuses a NeedsReview plan
// outright and must keep doing so - the non-conflicting part of a contested merge is a
// state neither person authored and neither reviewed.
//
// `perConflict` is parallel to plan.conflicts. If it is the wrong length, or ANY entry
// is Undecided, NOTHING is applied and `outRefused` is set: a partially reviewed merge
// is the same failure as an unreviewed one, and half-applying it would be worse than
// either because the result looks deliberate.
//
// KeepMine skips that (guid, component) in EVERY commit of the plan, not just the first
// - one key can be touched by several of their commits, and skipping only one of them
// would let a later one quietly overwrite the choice.
usize ApplyReviewedMerge(Scene& scene, const collab::MergePlan& plan,
                         const std::vector<Resolution>& perConflict,
                         usize* outMissing = nullptr, bool* outRefused = nullptr);

// --test-p2p: drives the WHOLE collaboration stack end to end against real scenes -
// live host/client editing with locks and revisions over a real transport, then a peer
// going offline, editing, and reconciling through the journal.
bool P2PEndToEndSelfTest();

} // namespace hbe::scene

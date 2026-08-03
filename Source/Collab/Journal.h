// Collab/Journal.h - the durable record of work done, online or off.
//
// THE MODEL: version control for one file type, not a distributed database. A save
// seals a COMMIT - the before and after bytes of every (entity, component) that
// changed, against a named parent commit. The `.hbscene` on disk is a WORKING COPY.
// Going offline is a fork; reconnecting is a fast-forward or a three-way merge; a
// merge a machine cannot decide STOPS AND ASKS A PERSON.
//
// That is how locks and offline reconcile. Locks prevent conflicts, and disconnection
// guarantees them, so they cannot be one mechanism. Locks exist precisely to stop a
// LIVE session from becoming a merge; the seam between the two models is not a mode
// flag, it is a branch operation.
//
// LAYERING: Core/ + STL only, like everything else under Source/Collab/. A commit
// stores component JSON as opaque strings - this file has no idea what a Transform is,
// which is what lets a headless server host history with no engine linked.
#pragma once

#include "Collab/CollabTypes.h"
#include "Core/BinaryStream.h"

#include <optional>
#include <string>
#include <vector>

namespace hbe::collab {

// One install. Minted once and stored next to the journal - NEVER a per-session
// counter like the server's nextUser_, which starts at 1 in every process and would
// make two peers' first commits claim the same author forever, on disk, unrepairably.
using PeerId = u64;

// A commit's identity: WHO made it and their own monotonic counter.
//
// Deliberately not a content hash. A hash would be elegant, but it would make the
// durable identity of history depend on two machines formatting the same float to the
// same bytes. {peer, n} is unique by construction and needs no agreement.
struct CommitId {
    PeerId peer = 0;
    u64 n = 0;
    bool Valid() const { return peer != 0; }
    bool operator==(const CommitId& o) const { return peer == o.peer && n == o.n; }
    bool operator!=(const CommitId& o) const { return !(*this == o); }
    std::string ToString() const;
};

// Record kinds, with PINNED VALUES and their own number space.
//
// Two rules, both load-bearing. (1) Explicit values, because the moment a kind is
// written to a file it is a durable identifier and inserting anything but at the end
// renumbers every record ever written. (2) NOT MsgType - the wire enum has no explicit
// values at all, so reusing it would couple the on-disk format to the order somebody
// happens to declare messages in.
enum class RecKind : u16 {
    Invalid = 0,
    CommitBegin = 1,
    Change = 2,
    CommitEnd = 3,
};

// What a change did to one component of one entity.
enum class ChangeOp : u8 {
    Set = 0,     // component created or modified; `after` holds its JSON
    Remove = 1,  // component removed; `after` is empty, `before` holds what it was
    CreateEntity = 2,
    DeleteEntity = 3,
};
const char* ChangeOpName(ChangeOp o);

// One (entity, component) transition.
//
// `before` IS STORED, not hashed. Without it a review cannot show base / mine / theirs,
// which means a human cannot tell WHO MOVED WHAT - and that is the entire difference
// between a merge tool and a diff viewer. It also gives an unrelated-histories check
// for free: if two branches record different `before` bytes for the same key, they
// never shared a base.
struct Change {
    u64 guid = 0;              // entity, stable across machines (see guidEpoch)
    std::string component;     // serializer key: "transform", "name", ...
    ChangeOp op = ChangeOp::Set;
    std::string before;        // empty = the component did not exist
    std::string after;         // empty = it does not exist now
};

struct Commit {
    CommitId id;
    CommitId parent;           // invalid = the root commit
    DocId doc = 0;
    u64 guidEpoch = 0;         // see the mismatch guard in Journal::CanMerge
    u64 timestampMs = 0;       // wall clock, DISPLAY ONLY - never used to order anything
    std::string author;        // display name at the time, for the review UI
    std::string message;       // optional; "saved scene" by default
    std::vector<Change> changes;
};

// A durable, append-only journal for ONE document.
//
// Append-only and length-prefixed on purpose: a crash mid-write leaves a truncated
// TAIL, and a truncated tail is detectable and discardable. Rewriting a file in place
// would instead leave a hole in the middle, which is not.
class Journal {
public:
    // Reads whatever is intact. A truncated or corrupt trailing record is DROPPED and
    // reported through `outTruncated` rather than failing the load: the whole point of
    // an append-only journal is that a crash costs you the last commit, not all of them.
    bool Load(const std::filesystem::path& file, bool* outTruncated = nullptr);
    // Appends one commit. Flushed and closed per call - a journal that buffers is a
    // journal that loses the work it was written to protect.
    bool Append(const std::filesystem::path& file, const Commit& c);

    const std::vector<Commit>& Commits() const { return commits_; }
    // The newest commit, i.e. this branch's head. Invalid when empty.
    CommitId Head() const;
    const Commit* Find(const CommitId& id) const;
    void Clear() { commits_.clear(); }
    void Add(const Commit& c) { commits_.push_back(c); }

private:
    std::vector<Commit> commits_;
};

// --- reconciliation ---------------------------------------------------------

enum class MergeVerdict : u8 {
    UpToDate,        // theirs adds nothing we do not have
    FastForward,     // we have no local commits they lack - just take theirs
    Merge,           // both sides moved; disjoint keys, safe to apply
    NeedsReview,     // both sides touched the SAME (entity, component)
    RefusedEpoch,    // same document, different guidEpoch - guids are NOT comparable
    RefusedDocument, // different documents entirely
};
const char* MergeVerdictName(MergeVerdict v);

// One thing a human has to decide.
struct Conflict {
    u64 guid = 0;
    std::string component;
    std::string base;   // what both sides started from
    std::string mine;
    std::string theirs;
    std::string mineAuthor, theirsAuthor;
};

struct MergePlan {
    MergeVerdict verdict = MergeVerdict::UpToDate;
    std::vector<Commit> toApply;      // theirs, in order, that we lack
    std::vector<Conflict> conflicts;  // non-empty only for NeedsReview
    std::string explanation;          // human-facing; safe to show verbatim
};

// Decides what happens when `theirs` meets `mine`. PURE - no files, no clock, no
// network - which is what lets --test-journal drive every branch of it.
//
// THE EPOCH GUARD IS FIRST, and it is the reason this function exists at all.
// scene::MigrateSceneGuids derives entity guids as Derive(SeedFromPath(path), row) - a
// pure function of the path and the ROW INDEX. Two peers holding divergent copies of a
// pre-guid scene (100 rows vs 80) therefore both derive Derive(seed, 5) and get the
// SAME guid for DIFFERENT objects, deterministically, on two machines, with nothing to
// detect it: each file is internally unique so the duplicate-guid guard sees nothing.
// Merging them would land one person's tent transform onto another's rock and report it
// CLEAN. A random per-document guidEpoch makes that case visible: same doc + different
// epoch means the two sides migrated independently, and the merge is refused BY NAME.
MergePlan PlanMerge(const Journal& mine, const Journal& theirs, DocId doc, u64 guidEpoch);

// --test-journal
bool JournalSelfTest();

} // namespace hbe::collab

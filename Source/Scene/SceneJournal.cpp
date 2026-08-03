// Scene/SceneJournal.cpp
#include "Scene/SceneJournal.h"

#include "Collab/CollabClient.h"
#include "Collab/CollabServer.h"
#include "Collab/LoopbackTransport.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/EntityGuid.h"
#include "Scene/Scene.h" // SceneSerializer.h only forward-declares Scene

#include <cstdio>
#include <set>

namespace hbe::scene {

JournalSnapshot SnapshotForJournal(const Scene& scene, const JournalFilter& include) {
    JournalSnapshot out;
    const entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<Guid>()) {
        if (include && !include(reg, e)) continue; // belongs to a different document
        const u64 g = reg.get<Guid>(e).value;
        if (g == 0) continue; // no stable identity = not journalable
        for (const std::string& key : DeltaComponentKeys()) {
            std::string json;
            if (ComponentToJson(scene, e, key, json)) out[{g, key}] = std::move(json);
        }
    }
    return out;
}

std::vector<collab::Change> DiffSnapshots(const JournalSnapshot& before,
                                          const JournalSnapshot& after) {
    std::vector<collab::Change> changes;
    // Both maps are ordered, so a single merge walk gives added / removed / modified in
    // one pass AND a stable order. Order matters: two machines with identical scenes
    // must produce identical commits, and a hash-map walk would not.
    auto a = before.begin();
    auto b = after.begin();
    const auto emit = [&changes](u64 guid, const std::string& comp, collab::ChangeOp op,
                                 std::string bef, std::string aft) {
        collab::Change c;
        c.guid = guid;
        c.component = comp;
        c.op = op;
        c.before = std::move(bef);
        c.after = std::move(aft);
        changes.push_back(std::move(c));
    };
    while (a != before.end() || b != after.end()) {
        if (b == after.end() || (a != before.end() && a->first < b->first)) {
            // Present before, gone now.
            emit(a->first.first, a->first.second, collab::ChangeOp::Remove, a->second, {});
            ++a;
        } else if (a == before.end() || b->first < a->first) {
            emit(b->first.first, b->first.second, collab::ChangeOp::Set, {}, b->second);
            ++b;
        } else {
            if (a->second != b->second)
                emit(a->first.first, a->first.second, collab::ChangeOp::Set, a->second,
                     b->second);
            ++a;
            ++b;
        }
    }
    return changes;
}

usize ApplyCommit(Scene& scene, const collab::Commit& c, usize* outMissing) {
    usize applied = 0, missing = 0;
    entt::registry& reg = scene.Registry();
    for (const collab::Change& ch : c.changes) {
        // BY GUID. An entt handle is an index into one process's registry - it recycles
        // and does not survive a reload, so it cannot name an object across machines.
        entt::entity target = entt::null;
        for (const entt::entity e : reg.view<Guid>()) {
            if (reg.get<Guid>(e).value == ch.guid) { target = e; break; }
        }
        if (target == entt::null) {
            ++missing; // the sides disagree about what exists - the caller must know
            continue;
        }
        const std::string& payload =
            ch.op == collab::ChangeOp::Remove ? std::string() : ch.after;
        if (ApplyComponentJson(scene, target, ch.component, payload) == DeltaApply::Applied ||
            ch.op == collab::ChangeOp::Remove)
            ++applied;
    }
    if (outMissing) *outMissing = missing;
    return applied;
}

const char* ResolutionName(Resolution r) {
    switch (r) {
    case Resolution::Undecided: return "undecided";
    case Resolution::KeepMine: return "keep mine";
    case Resolution::TakeTheirs: return "take theirs";
    }
    return "?";
}

usize ApplyReviewedMerge(Scene& scene, const collab::MergePlan& plan,
                         const std::vector<Resolution>& perConflict, usize* outMissing,
                         bool* outRefused) {
    if (outMissing) *outMissing = 0;
    if (outRefused) *outRefused = false;

    // Only a contested plan is reviewed. Routing an ordinary merge through here would
    // silently accept an empty decision list as "all reviewed".
    if (plan.verdict != collab::MergeVerdict::NeedsReview) {
        if (outRefused) *outRefused = true;
        return 0;
    }
    if (perConflict.size() != plan.conflicts.size()) {
        if (outRefused) *outRefused = true;
        return 0;
    }
    for (const Resolution r : perConflict) {
        if (r == Resolution::Undecided) {
            if (outRefused) *outRefused = true;
            return 0;
        }
    }

    // The keys the person chose to keep their own version of. Their incoming value for
    // these is dropped wherever it appears.
    std::set<std::pair<u64, std::string>> keepMine;
    for (usize i = 0; i < perConflict.size(); ++i) {
        if (perConflict[i] == Resolution::KeepMine)
            keepMine.insert({plan.conflicts[i].guid, plan.conflicts[i].component});
    }

    usize total = 0, missing = 0;
    for (const collab::Commit& c : plan.toApply) {
        collab::Commit filtered = c;
        filtered.changes.clear();
        filtered.changes.reserve(c.changes.size());
        for (const collab::Change& ch : c.changes) {
            if (keepMine.count({ch.guid, ch.component})) continue;
            filtered.changes.push_back(ch);
        }
        usize m = 0;
        total += ApplyCommit(scene, filtered, &m);
        missing += m;
    }
    if (outMissing) *outMissing = missing;
    return total;
}

usize ApplyMergePlan(Scene& scene, const collab::MergePlan& plan, usize* outMissing) {
    usize total = 0, missing = 0;
    // A plan that needs a human is NOT applicable. Applying the non-conflicting part
    // would leave the scene in a state neither person authored and neither reviewed.
    if (plan.verdict == collab::MergeVerdict::NeedsReview ||
        plan.verdict == collab::MergeVerdict::RefusedEpoch ||
        plan.verdict == collab::MergeVerdict::RefusedDocument) {
        if (outMissing) *outMissing = 0;
        return 0;
    }
    for (const collab::Commit& c : plan.toApply) {
        usize m = 0;
        total += ApplyCommit(scene, c, &m);
        missing += m;
    }
    if (outMissing) *outMissing = missing;
    return total;
}

// ===========================================================================
// --test-p2p : the whole stack, end to end
// ===========================================================================

namespace {

int g_fails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("p2p FAIL: %s\n", what);
}

// Builds a small scene with STABLE guids on both peers, as a shared document would
// have after one central migration.
void BuildScene(Scene& s, u64 epochSeed) {
    entt::registry& reg = s.Registry();
    for (int i = 0; i < 3; ++i) {
        const entt::entity e = s.CreateEntity("Obj" + std::to_string(i));
        Transform t;
        t.position = {static_cast<f32>(i), 0.0f, 0.0f};
        reg.emplace_or_replace<Transform>(e, t);
        // Same guids on both peers - that is the precondition the epoch guard protects.
        reg.emplace_or_replace<Guid>(e, Guid{guid::Derive(epochSeed, static_cast<u32>(i))});
    }
}

entt::entity ByGuid(Scene& s, u64 g) {
    entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<Guid>())
        if (reg.get<Guid>(e).value == g) return e;
    return entt::null;
}

glm::vec3 PosOf(Scene& s, u64 g) {
    const entt::entity e = ByGuid(s, g);
    return e == entt::null ? glm::vec3(-999.0f) : s.Registry().get<Transform>(e).position;
}

} // namespace

bool P2PEndToEndSelfTest() {
    g_fails = 0;
    constexpr collab::DocId kDoc = 0x1234ABCDull;
    constexpr u64 kEpoch = 0xFEEDFACEull;
    constexpr u64 kSeed = 0xA5A5A5A5ull;
    const u64 g0 = guid::Derive(kSeed, 0), g1 = guid::Derive(kSeed, 1);

    // ==================================================================
    // PART 0 - EVERY SUPPORTED COMPONENT ACTUALLY ROUND-TRIPS.
    //
    // The delta table maps a JSON KEY to a parsed FIELD, and the two are
    // written out separately per row. A row that names "spotLight" but checks
    // hasPointLight parses fine, finds its flag false, and reports BadJson - a
    // component that silently never syncs, with the sender none the wiser.
    // Thirty-odd rows written in one sitting is exactly where that slip lives.
    //
    // So: drive EVERY key the build claims to support, and require it to apply,
    // to appear afterwards, and to survive a second round trip unchanged.
    // ==================================================================
    {
        Scene probe;
        for (const std::string& key : DeltaComponentKeys()) {
            const entt::entity e = probe.CreateEntity("probe_" + key);
            // A neutral sample per shape: the readers fill defaults for absent
            // fields, so an empty object exercises the mapping without needing a
            // hand-authored sample for all of them.
            const std::string sample = (key == "name") ? "\"probe\"" : "{}";
            const DeltaApply r = ApplyComponentJson(probe, e, key, sample);
            Check(r == DeltaApply::Applied,
                  ("a supported component did not apply - its table row probably names "
                   "one key and checks another component's flag: " + key)
                      .c_str());
            if (r != DeltaApply::Applied) continue;

            std::string back;
            Check(ComponentToJson(probe, e, key, back),
                  ("a component applied but then did not read back: " + key).c_str());

            // Idempotence: feeding a component its own serialized form must not
            // change it. A field the writer emits but the reader ignores shows up
            // here as a value that keeps changing between peers.
            const DeltaApply again = ApplyComponentJson(probe, e, key, back);
            Check(again == DeltaApply::Applied,
                  ("a component would not accept its OWN output: " + key).c_str());
            std::string back2;
            ComponentToJson(probe, e, key, back2);
            Check(back == back2,
                  ("a component is not stable across a round trip - it would drift a "
                   "little further on every edit: " + key)
                      .c_str());

            // ...and removal works, or a deleted component would linger on peers.
            Check(ApplyComponentJson(probe, e, key, std::string()) == DeltaApply::Removed,
                  ("a component could not be removed: " + key).c_str());
            std::string gone;
            Check(!ComponentToJson(probe, e, key, gone),
                  ("a removed component is still present: " + key).c_str());
        }
        Check(DeltaComponentKeys().size() >= 20,
              "the delta seam covers almost nothing - live editing would carry a "
              "fraction of what the inspector can change");
        Check(ApplyComponentJson(probe, probe.CreateEntity("x"), "mesh", "{}") ==
                  DeltaApply::UnknownKey,
              "an UNSUPPORTED component must be refused by name, never silently "
              "ignored - 'mesh' needs staged assets and is deliberately absent");
    }

    // ==================================================================
    // PART 1 - LIVE SESSION. "P2P" here means an ELECTED HOST: one peer runs
    // the authoritative server IN ITS OWN PROCESS and connects to itself, so
    // there is no dedicated box and no `if (isHost)` branch anywhere.
    // ==================================================================
    Scene hostScene, guestScene;
    BuildScene(hostScene, kSeed);
    BuildScene(guestScene, kSeed);

    collab::LoopbackHub hub;
    collab::CollabServer server(&hub);

    // The host's own client - the same class the guest uses, over the same transport.
    collab::ClientCallbacks hostCb, guestCb;
    int hostApplied = 0, guestApplied = 0, guestRejected = 0;
    hostCb.onDelta = [&](const collab::MsgDeltaApplied& d) {
        ++hostApplied;
        const entt::entity e = ByGuid(hostScene, d.key.guid);
        if (e != entt::null) ApplyComponentJson(hostScene, e, d.componentKey, d.json);
    };
    guestCb.onDelta = [&](const collab::MsgDeltaApplied& d) {
        ++guestApplied;
        const entt::entity e = ByGuid(guestScene, d.key.guid);
        if (e != entt::null) ApplyComponentJson(guestScene, e, d.componentKey, d.json);
    };
    guestCb.onRejected = [&](const collab::MsgDeltaRejected&) { ++guestRejected; };

    collab::CollabClient host(hub.CreateClient(), hostCb);
    collab::CollabClient guest(hub.CreateClient(), guestCb);

    u64 now = 1000;
    const auto step = [&](u64 ms = 16) {
        now += ms;
        host.Pump(now);
        guest.Pump(now);
        server.Tick(now);
        host.Pump(now);
        guest.Pump(now);
    };

    host.Hello("ana");
    guest.Hello("ben");
    step();
    Check(host.Ready() && guest.Ready(), "both peers should join the session");
    Check(server.PeerCount() == 2, "the host's server should see two peers");
    Check(host.IsWriter() != guest.IsWriter(), "exactly one peer must be the writer");

    // --- a real edit propagates, with the lock enforced ---
    collab::EntityKey k0;
    k0.doc = kDoc;
    k0.guid = g0;
    host.RequestLock(k0);
    step();
    Check(server.LockOf(k0).owner == host.User(), "the host should hold the lock");

    // The GUEST tries to edit what the host holds. It must be refused, and the host's
    // scene must not move.
    {
        const glm::vec3 was = PosOf(hostScene, g0);
        guest.SendDelta(k0, "transform", "{\"p\":[99.0,0.0,0.0],\"r\":[1.0,0.0,0.0,0.0],\"s\":[1.0,1.0,1.0]}");
        step();
        Check(guestRejected == 1, "a non-owner's live edit must be refused");
        Check(glm::length(PosOf(hostScene, g0) - was) < 1e-4f,
              "a refused edit must not reach the other peer's scene");
    }

    // The OWNER edits: it must apply here and appear THERE, through the real protocol.
    {
        const entt::entity he = ByGuid(hostScene, g0);
        Transform t = hostScene.Registry().get<Transform>(he);
        t.position = {7.0f, 8.0f, 9.0f};
        hostScene.Registry().emplace_or_replace<Transform>(he, t);
        std::string json;
        Check(ComponentToJson(hostScene, he, "transform", json), "the edit should serialize");
        host.SendDelta(k0, "transform", json);
        step();
        Check(guestApplied >= 1, "the guest never received the host's edit");
        Check(glm::length(PosOf(guestScene, g0) - glm::vec3(7.0f, 8.0f, 9.0f)) < 1e-3f,
              "REAL-TIME SYNC FAILED: the guest's scene did not follow the host's edit");
        Check(server.RevisionOf(k0) == 1, "the accepted edit should bump the revision once");
    }

    // ==================================================================
    // PART 2 - GOING OFFLINE. The guest leaves and keeps working. Its edits
    // become a journal, not a stream.
    // ==================================================================
    const JournalSnapshot guestBase = SnapshotForJournal(guestScene);
    {
        const entt::entity e = ByGuid(guestScene, g1);
        Transform t = guestScene.Registry().get<Transform>(e);
        t.position = {-5.0f, 0.0f, 0.0f};
        guestScene.Registry().emplace_or_replace<Transform>(e, t);
        guestScene.Registry().emplace_or_replace<Name>(e, Name{"RenamedOffline"});
    }
    const JournalSnapshot guestAfter = SnapshotForJournal(guestScene);
    const std::vector<collab::Change> guestChanges = DiffSnapshots(guestBase, guestAfter);
    Check(guestChanges.size() == 2,
          "an offline move + rename should seal exactly two changes");
    Check(!guestChanges.empty() && !guestChanges[0].before.empty(),
          "a sealed change must carry its BEFORE bytes or a review cannot show base");

    // Determinism: diffing the same pair twice must give the same commit, or two
    // machines holding identical scenes would produce different history.
    Check(DiffSnapshots(guestBase, guestAfter).size() == guestChanges.size(),
          "the diff must be deterministic");

    collab::Journal guestJournal;
    {
        collab::Commit c;
        c.id = collab::CommitId{2, 1};
        c.doc = kDoc;
        c.guidEpoch = kEpoch;
        c.author = "ben";
        c.changes = guestChanges;
        guestJournal.Add(c);
    }

    // Meanwhile the HOST also works, on a DIFFERENT object.
    collab::Journal hostJournal;
    {
        const JournalSnapshot base = SnapshotForJournal(hostScene);
        const entt::entity e = ByGuid(hostScene, g0);
        Transform t = hostScene.Registry().get<Transform>(e);
        t.position = {1.0f, 2.0f, 3.0f};
        hostScene.Registry().emplace_or_replace<Transform>(e, t);
        collab::Commit c;
        c.id = collab::CommitId{1, 1};
        c.doc = kDoc;
        c.guidEpoch = kEpoch;
        c.author = "ana";
        c.changes = DiffSnapshots(base, SnapshotForJournal(hostScene));
        hostJournal.Add(c);
    }

    // ==================================================================
    // PART 3 - RECONNECTING. Disjoint work must merge with no questions, and
    // must actually LAND in the scene.
    // ==================================================================
    {
        const collab::MergePlan plan =
            collab::PlanMerge(guestJournal, hostJournal, kDoc, kEpoch);
        Check(plan.verdict == collab::MergeVerdict::Merge,
              "disjoint offline work should merge without review");
        Check(plan.conflicts.empty(), "disjoint work is not a conflict");
        usize missing = 0;
        const usize applied = ApplyMergePlan(guestScene, plan, &missing);
        Check(applied >= 1, "the host's offline commit did not apply to the guest scene");
        Check(missing == 0, "no entity should have been missing");
        Check(glm::length(PosOf(guestScene, g0) - glm::vec3(1.0f, 2.0f, 3.0f)) < 1e-3f,
              "OFFLINE MERGE FAILED: the host's change did not land in the guest's scene");
        // ...and the guest's OWN offline work must survive the merge untouched. A merge
        // that silently reverts your own work is the worst outcome in the system.
        Check(glm::length(PosOf(guestScene, g1) - glm::vec3(-5.0f, 0.0f, 0.0f)) < 1e-3f,
              "the merge REVERTED the guest's own offline work");
    }

    // ==================================================================
    // PART 4 - CONFLICT. Both edited the same thing; a human must decide, and
    // NOTHING may be applied until they do.
    // ==================================================================
    {
        collab::Journal mine, theirs;
        collab::Commit a, b;
        a.id = collab::CommitId{1, 9};
        a.doc = kDoc; a.guidEpoch = kEpoch; a.author = "ana";
        a.changes = {{g0, "name", collab::ChangeOp::Set, "\"Obj0\"", "\"AnaName\""}};
        b.id = collab::CommitId{2, 9};
        b.doc = kDoc; b.guidEpoch = kEpoch; b.author = "ben";
        b.changes = {{g0, "name", collab::ChangeOp::Set, "\"Obj0\"", "\"BenName\""}};
        mine.Add(a);
        theirs.Add(b);
        const collab::MergePlan plan = collab::PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(plan.verdict == collab::MergeVerdict::NeedsReview, "an overlap needs review");
        Check(plan.conflicts.size() == 1, "one conflict expected");
        if (!plan.conflicts.empty()) {
            Check(plan.conflicts[0].mineAuthor == "ana" &&
                      plan.conflicts[0].theirsAuthor == "ben",
                  "the review must name who made each edit");
            Check(plan.conflicts[0].base == "\"Obj0\"", "the review must show the base value");
        }
        Scene probe;
        BuildScene(probe, kSeed);
        const std::string wasName = probe.Registry().get<Name>(ByGuid(probe, g0)).value;
        usize missing = 0;
        Check(ApplyMergePlan(probe, plan, &missing) == 0,
              "a plan needing review must apply NOTHING - a half-applied merge leaves a "
              "state neither person authored and neither reviewed");
        Check(probe.Registry().get<Name>(ByGuid(probe, g0)).value == wasName,
              "an unreviewed merge modified the scene");

        // ...and every way a review can be INCOMPLETE. Each of these must apply nothing:
        // a half-answered review is the same failure as an unanswered one, except that
        // the result looks deliberate.
        bool refused = false;
        Check(ApplyReviewedMerge(probe, plan, {}, &missing, &refused) == 0 && refused,
              "a review with NO decisions must be refused");
        Check(ApplyReviewedMerge(probe, plan, {Resolution::Undecided}, &missing, &refused) == 0 &&
                  refused,
              "an UNDECIDED conflict must be refused, not treated as a default");
        Check(ApplyReviewedMerge(probe, plan, {Resolution::TakeTheirs, Resolution::KeepMine},
                                 &missing, &refused) == 0 &&
                  refused,
              "a decision list of the wrong length must be refused rather than "
              "silently matched up by position");
        Check(probe.Registry().get<Name>(ByGuid(probe, g0)).value == wasName,
              "a refused review modified the scene");

        // KEEP MINE: their value is dropped and the local edit stands.
        {
            Scene keep;
            BuildScene(keep, kSeed);
            keep.Registry().get<Name>(ByGuid(keep, g0)).value = "AnaName";
            bool r = false;
            ApplyReviewedMerge(keep, plan, {Resolution::KeepMine}, &missing, &r);
            Check(!r, "a fully decided review must not be refused");
            Check(keep.Registry().get<Name>(ByGuid(keep, g0)).value == "AnaName",
                  "KEEP MINE must leave the local value alone");
        }
        // TAKE THEIRS: their value lands.
        {
            Scene take;
            BuildScene(take, kSeed);
            take.Registry().get<Name>(ByGuid(take, g0)).value = "AnaName";
            bool r = false;
            const usize applied =
                ApplyReviewedMerge(take, plan, {Resolution::TakeTheirs}, &missing, &r);
            Check(!r && applied == 1, "a decided review should apply their change");
            Check(take.Registry().get<Name>(ByGuid(take, g0)).value == "BenName",
                  "TAKE THEIRS must land the incoming value");
        }
        // An ordinary merge must NOT be routable through the review path, where an empty
        // decision list would otherwise read as "all conflicts reviewed".
        {
            collab::Journal empty;
            const collab::MergePlan ff = collab::PlanMerge(empty, theirs, kDoc, kEpoch);
            Check(ff.verdict == collab::MergeVerdict::FastForward, "expected a fast-forward");
            Scene s2;
            BuildScene(s2, kSeed);
            bool r = false;
            Check(ApplyReviewedMerge(s2, ff, {}, &missing, &r) == 0 && r,
                  "an uncontested plan must not be applicable through the REVIEW path");
        }
    }

    // ==================================================================
    // PART 4b - KEEP MINE across SEVERAL of their commits. One key touched more
    // than once must be skipped every time, or a later commit quietly overwrites
    // the decision the person just made.
    // ==================================================================
    {
        collab::Journal mine, theirs;
        collab::Commit a, b1, b2;
        a.id = collab::CommitId{1, 20};
        a.doc = kDoc; a.guidEpoch = kEpoch; a.author = "ana";
        a.changes = {{g0, "name", collab::ChangeOp::Set, "\"Obj0\"", "\"AnaName\""}};
        b1.id = collab::CommitId{2, 20};
        b1.doc = kDoc; b1.guidEpoch = kEpoch; b1.author = "ben";
        b1.changes = {{g0, "name", collab::ChangeOp::Set, "\"Obj0\"", "\"BenFirst\""}};
        b2.id = collab::CommitId{2, 21};
        b2.parent = b1.id;
        b2.doc = kDoc; b2.guidEpoch = kEpoch; b2.author = "ben";
        b2.changes = {{g0, "name", collab::ChangeOp::Set, "\"BenFirst\"", "\"BenSecond\""}};
        mine.Add(a);
        theirs.Add(b1);
        theirs.Add(b2);
        const collab::MergePlan plan = collab::PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(plan.verdict == collab::MergeVerdict::NeedsReview, "still a conflict");
        Check(plan.toApply.size() == 2, "both of their commits should be in the plan");
        Scene s;
        BuildScene(s, kSeed);
        s.Registry().get<Name>(ByGuid(s, g0)).value = "AnaName";
        std::vector<Resolution> keepAll(plan.conflicts.size(), Resolution::KeepMine);
        usize missing = 0;
        bool r = false;
        ApplyReviewedMerge(s, plan, keepAll, &missing, &r);
        Check(!r, "a fully decided review must not be refused");
        Check(s.Registry().get<Name>(ByGuid(s, g0)).value == "AnaName",
              "KEEP MINE must hold against EVERY one of their commits - skipping only the "
              "first lets a later one overwrite the person's decision");
    }

    // ==================================================================
    // PART 5 - THE EPOCH GUARD, against a REAL scene. Two peers that migrated
    // independently derive colliding guids; the merge must refuse.
    // ==================================================================
    {
        collab::Journal mine, theirs;
        collab::Commit b;
        b.id = collab::CommitId{3, 1};
        b.doc = kDoc;
        b.guidEpoch = 0x9999888877776666ull; // a DIFFERENT epoch: migrated separately
        b.author = "cat";
        b.changes = {{g0, "name", collab::ChangeOp::Set, "", "\"Wrong\""}};
        theirs.Add(b);
        const collab::MergePlan plan = collab::PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(plan.verdict == collab::MergeVerdict::RefusedEpoch,
              "independently migrated copies must be refused");
        Scene probe;
        BuildScene(probe, kSeed);
        usize missing = 0;
        Check(ApplyMergePlan(probe, plan, &missing) == 0, "a refused merge must apply nothing");
    }

    // ==================================================================
    // PART 6 - A PEER THAT NEVER SAW AN ENTITY. Applying a commit that names a
    // guid we do not have must be COUNTED, not silently dropped.
    // ==================================================================
    {
        Scene tiny;
        collab::Commit c;
        c.doc = kDoc;
        c.guidEpoch = kEpoch;
        c.changes = {{g0, "name", collab::ChangeOp::Set, "", "\"Ghost\""}};
        usize missing = 0;
        const usize applied = ApplyCommit(tiny, c, &missing);
        Check(applied == 0 && missing == 1,
              "a change for an entity we do not have must be reported as missing");
    }

    if (g_fails == 0) {
        std::printf("p2p: every supported component applies, reads back, accepts its own "
                    "output unchanged and can be removed, while an unsupported one is "
                    "refused by name; elected-host session over a real transport - lock enforced, a "
                    "non-owner's edit refused, an owner's edit reaching the other peer's "
                    "SCENE; offline work sealed deterministically with before-bytes; "
                    "disjoint work merged and landed without reverting local edits; an "
                    "overlap held for review and applied NOTHING; independently migrated "
                    "copies refused; unknown entities counted; and a REVIEWED conflict "
                    "landing only once every question is answered - keep-mine holding "
                    "against all of their commits, an empty, undecided or wrong-length "
                    "answer refused\n");
    }
    return g_fails == 0;
}

} // namespace hbe::scene

// Collab/JournalSelfTest.cpp - `--test-journal`.
//
// Every case asserts the WRONG behaviour fails. The two that matter most are the crash
// survival (a torn tail must cost the LAST commit, never all of them) and the epoch
// guard (a merge across independently-migrated copies must be REFUSED, because guids
// collide by construction there and a silent merge moves the wrong objects).
#include "Collab/Journal.h"

#include <cstdio>
#include <fstream>

namespace hbe::collab {

namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("journal FAIL: %s\n", what);
}

Change Set(u64 guid, const char* comp, const char* before, const char* after) {
    Change c;
    c.guid = guid;
    c.component = comp;
    c.op = ChangeOp::Set;
    c.before = before;
    c.after = after;
    return c;
}

Commit Make(PeerId peer, u64 n, DocId doc, u64 epoch, const char* author,
            std::vector<Change> ch, CommitId parent = {}) {
    Commit c;
    c.id = CommitId{peer, n};
    c.parent = parent;
    c.doc = doc;
    c.guidEpoch = epoch;
    c.author = author;
    c.message = "saved scene";
    c.changes = std::move(ch);
    return c;
}

} // namespace

bool JournalSelfTest() {
    g_fails = 0;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_journal_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    constexpr DocId kDoc = 0xABCDEF0123456789ull;
    constexpr u64 kEpoch = 0x1111222233334444ull;

    // ------------------------------------------------------------------
    // 1) ROUND TRIP. Everything a review needs must survive to disk, `before`
    //    included - without it the UI can show that something changed but not
    //    what it changed FROM, which is the difference between a merge tool and
    //    a diff viewer.
    // ------------------------------------------------------------------
    const fs::path f = dir / "a.hbjournal";
    {
        Journal j;
        Check(j.Append(f, Make(1, 1, kDoc, kEpoch, "ana",
                               {Set(100, "transform", "{\"p\":[0,0,0]}", "{\"p\":[1,0,0]}"),
                                Set(101, "name", "\"Old\"", "\"New\"")})),
              "the first commit should append");
        Check(j.Append(f, Make(1, 2, kDoc, kEpoch, "ana",
                               {Set(102, "transform", "", "{\"p\":[5,5,5]}")},
                               CommitId{1, 1})),
              "the second commit should append");

        Journal r;
        bool torn = true;
        Check(r.Load(f, &torn), "the journal should load");
        Check(!torn, "a cleanly written journal must NOT report truncation");
        Check(r.Commits().size() == 2, "both commits should survive");
        if (r.Commits().size() == 2) {
            const Commit& c0 = r.Commits()[0];
            Check(c0.id == CommitId{1, 1}, "commit id did not survive");
            Check(c0.doc == kDoc && c0.guidEpoch == kEpoch, "identity did not survive");
            Check(c0.author == "ana", "author did not survive");
            Check(c0.changes.size() == 2, "changes did not survive");
            Check(c0.changes[0].before == "{\"p\":[0,0,0]}",
                  "the BEFORE bytes did not survive - a review cannot show base vs mine");
            Check(c0.changes[0].after == "{\"p\":[1,0,0]}", "the after bytes did not survive");
            Check(r.Commits()[1].parent == CommitId{1, 1}, "the parent link did not survive");
        }
        Check(r.Head() == CommitId{1, 2}, "head should be the newest commit");
    }

    // ------------------------------------------------------------------
    // 2) CRASH SURVIVAL. Truncate the file at every byte and assert the same two
    //    properties every time: it never fails to load, and it never invents a
    //    commit. Losing the last commit is acceptable; losing all of them, or
    //    surfacing HALF of one, is not.
    // ------------------------------------------------------------------
    {
        std::ifstream in(f, std::ios::binary | std::ios::ate);
        const std::streamoff full = in.tellg();
        in.seekg(0);
        std::vector<char> all(static_cast<usize>(full));
        in.read(all.data(), full);
        in.close();

        bool everFailed = false, everInvented = false, everPartial = false;
        for (std::streamoff cut = 4; cut < full; ++cut) {
            const fs::path t = dir / "torn.hbjournal";
            {
                std::ofstream o(t, std::ios::binary | std::ios::trunc);
                o.write(all.data(), cut);
            }
            Journal r;
            bool torn = false;
            if (!r.Load(t, &torn)) everFailed = true;
            if (r.Commits().size() > 2) everInvented = true;
            for (const Commit& c : r.Commits()) {
                // A commit that surfaced must be WHOLE. The two written here have 2 and
                // 1 changes; anything else means a partial commit escaped.
                const usize want = c.id.n == 1 ? 2u : 1u;
                if (c.changes.size() != want) everPartial = true;
            }
        }
        Check(!everFailed, "a truncated journal must still LOAD what is intact");
        Check(!everInvented, "a truncated journal invented commits");
        Check(!everPartial,
              "a PARTIAL commit surfaced from a torn file - half an artist's edit "
              "applied is worse than none, because nobody would know");

        // ...and a torn tail must be REPORTED, not silently swallowed.
        const fs::path t = dir / "torn2.hbjournal";
        {
            std::ofstream o(t, std::ios::binary | std::ios::trunc);
            o.write(all.data(), full - 5);
        }
        Journal r;
        bool torn = false;
        r.Load(t, &torn);
        Check(torn, "a torn tail must be reported so the user can be told");
        Check(r.Commits().size() == 1, "the intact commit before the tear must survive");
    }

    // ------------------------------------------------------------------
    // 3) THE EPOCH GUARD. The single most important case in this file.
    // ------------------------------------------------------------------
    {
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(100, "transform", "", "{\"a\":1}")}));
        // Same document, DIFFERENT epoch: the other side migrated its own copy, so its
        // guid 100 is a different object from ours.
        theirs.Add(Make(2, 1, kDoc, 0x9999888877776666ull, "ben",
                        {Set(100, "transform", "", "{\"b\":2}")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::RefusedEpoch,
              "a merge across independently-migrated copies MUST be refused - guids "
              "collide by construction there and merging moves the wrong objects");
        Check(p.toApply.empty(), "a refused merge must apply nothing");
        Check(!p.explanation.empty(), "a refusal must explain itself to a human");
    }
    {
        // ...and a DIFFERENT document is refused separately, with its own reason.
        Journal mine, theirs;
        theirs.Add(Make(2, 1, 0xDEADBEEFull, kEpoch, "ben", {}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::RefusedDocument, "a foreign document must be refused");
    }

    // ------------------------------------------------------------------
    // 4) FAST-FORWARD: they moved, we did not.
    // ------------------------------------------------------------------
    {
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(1, "name", "", "\"a\"")}));
        theirs.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(1, "name", "", "\"a\"")}));
        theirs.Add(Make(2, 1, kDoc, kEpoch, "ben", {Set(2, "name", "", "\"b\"")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::FastForward, "no local divergence = fast-forward");
        Check(p.toApply.size() == 1, "exactly the one commit we lack should apply");
    }

    // ------------------------------------------------------------------
    // 5) UP TO DATE: nothing new.
    // ------------------------------------------------------------------
    {
        Journal mine, theirs;
        const Commit c = Make(1, 1, kDoc, kEpoch, "ana", {Set(1, "name", "", "\"a\"")});
        mine.Add(c);
        theirs.Add(c);
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::UpToDate, "identical histories are up to date");
        Check(p.toApply.empty(), "nothing to apply when up to date");
    }

    // ------------------------------------------------------------------
    // 6) CLEAN MERGE: both moved, DISJOINT keys. This must NOT ask a human -
    //    a tool that asks about non-conflicts trains people to click through.
    // ------------------------------------------------------------------
    {
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(10, "transform", "", "{\"a\":1}")}));
        theirs.Add(Make(2, 1, kDoc, kEpoch, "ben", {Set(20, "transform", "", "{\"b\":2}")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::Merge, "disjoint edits must merge without review");
        Check(p.conflicts.empty(), "disjoint edits are not conflicts");
        Check(p.toApply.size() == 1, "their commit should be applied");
    }
    {
        // Same ENTITY, different COMPONENT is still disjoint - one person moving an
        // object while another renames it is not a conflict.
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(10, "transform", "", "{\"a\":1}")}));
        theirs.Add(Make(2, 1, kDoc, kEpoch, "ben", {Set(10, "name", "", "\"x\"")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::Merge,
              "same entity + different component must merge cleanly");
    }

    // ------------------------------------------------------------------
    // 7) CONFLICT: same (entity, component), different results. This is the one
    //    case a machine may not decide.
    // ------------------------------------------------------------------
    {
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana",
                      {Set(10, "transform", "{\"p\":0}", "{\"p\":1}")}));
        theirs.Add(Make(2, 1, kDoc, kEpoch, "ben",
                        {Set(10, "transform", "{\"p\":0}", "{\"p\":2}")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.verdict == MergeVerdict::NeedsReview, "an overlapping edit needs review");
        Check(p.conflicts.size() == 1, "exactly one conflict expected");
        if (p.conflicts.size() == 1) {
            const Conflict& c = p.conflicts[0];
            Check(c.base == "{\"p\":0}", "the conflict must carry the BASE value");
            Check(c.mine == "{\"p\":1}" && c.theirs == "{\"p\":2}",
                  "the conflict must carry both sides");
            Check(c.mineAuthor == "ana" && c.theirsAuthor == "ben",
                  "the conflict must name WHO made each edit - the review is unusable "
                  "without it");
        }
    }
    {
        // ...but agreeing on the RESULT is not a conflict, even on the same key.
        Journal mine, theirs;
        mine.Add(Make(1, 1, kDoc, kEpoch, "ana", {Set(10, "transform", "{\"p\":0}", "{\"p\":9}")}));
        theirs.Add(Make(2, 1, kDoc, kEpoch, "ben", {Set(10, "transform", "{\"p\":0}", "{\"p\":9}")}));
        const MergePlan p = PlanMerge(mine, theirs, kDoc, kEpoch);
        Check(p.conflicts.empty(),
              "two people reaching the SAME value is not a conflict - asking would "
              "train people to click through real ones");
        Check(p.verdict == MergeVerdict::Merge, "an agreeing overlap should merge");
    }

    // ------------------------------------------------------------------
    // 8) AN UNKNOWN RECORD KIND IS SKIPPED, not fatal. This is what makes every
    //    other format decision reversible: a newer peer's journal must still be
    //    readable by an older build.
    // ------------------------------------------------------------------
    {
        const fs::path u = dir / "future.hbjournal";
        {
            Journal j;
            j.Append(u, Make(1, 1, kDoc, kEpoch, "ana", {Set(1, "name", "", "\"a\"")}));
        }
        // Splice a record with a kind this build will never know onto the end.
        {
            std::ofstream o(u, std::ios::binary | std::ios::app);
            const u32 len = 4;
            const u16 kind = 4242, ver = 1;
            o.write(reinterpret_cast<const char*>(&len), 4);
            o.write(reinterpret_cast<const char*>(&kind), 2);
            o.write(reinterpret_cast<const char*>(&ver), 2);
            const char junk[4] = {1, 2, 3, 4};
            o.write(junk, 4);
        }
        Journal r;
        bool torn = false;
        Check(r.Load(u, &torn), "a journal with an unknown record must still load");
        Check(r.Commits().size() == 1, "the known commit must survive an unknown record");
        Check(!torn, "an unknown-but-WELL-FORMED record is not truncation");
    }

    // ------------------------------------------------------------------
    // 9) A file that is not a journal must be REJECTED, not parsed as an empty one.
    // ------------------------------------------------------------------
    {
        const fs::path bad = dir / "notajournal.bin";
        { std::ofstream o(bad, std::ios::binary); o << "this is not a journal at all"; }
        Journal r;
        Check(!r.Load(bad), "a file with the wrong magic must be refused");
    }

    fs::remove_all(dir, ec);
    if (g_fails == 0) {
        std::printf("journal: commits round-trip with their BEFORE bytes; a torn tail "
                    "costs only the last commit and never surfaces a partial one; "
                    "independently-migrated copies are REFUSED; disjoint edits merge, "
                    "overlapping ones go to review with base/mine/theirs and both "
                    "authors; unknown records are skipped\n");
    }
    return g_fails == 0;
}

} // namespace hbe::collab

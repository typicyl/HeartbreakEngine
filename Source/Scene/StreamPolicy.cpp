// Scene/StreamPolicy.cpp
#include "Scene/StreamPolicy.h"

#include "Core/Log.h"
#include "Scene/StreamingSalvage.h" // SALVAGE 2 (hysteresis), used by the self-test

#include <algorithm>
#include <cmath>
#include <limits>

namespace hbe::stream {

f32 DistanceToBox(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& p) {
    // Per-axis overshoot, then the length of that vector. Zero inside the box, which
    // is the answer that makes "the player is standing in this shard" unambiguous.
    const glm::vec3 d = glm::max(glm::max(mn - p, p - mx), glm::vec3(0.0f));
    return glm::length(d);
}

namespace {

// Closest focus to a shard. Returns false when there is no focus at all, which is
// NOT the same as "infinitely far": see rule 3.
bool NearestFocus(const PolicyShard& s, const glm::vec3* foci, u32 n, f32& outDist) {
    if (!foci || n == 0) return false;
    f32 best = std::numeric_limits<f32>::max();
    for (u32 i = 0; i < n; ++i) best = glm::min(best, DistanceToBox(s.min, s.max, foci[i]));
    outDist = best;
    return true;
}

using Candidate = PolicyCandidate; // ranking scratch lives in PolicyOut (see the header)

} // namespace

void Evaluate(const PolicyIn& in, PolicyOut& out) {
    out.load.clear();
    out.unload.clear();
    out.inRange = 0;
    out.residentCount = 0;
    out.moreWork = false;
    if (!in.shards || in.count == 0) return;

    for (u32 i = 0; i < in.count; ++i)
        if (in.shards[i].resident) ++out.residentCount;

    // Streaming disabled: everything loads, nothing unloads. "Off" cannot mean "half
    // the level is missing" - that is a debugging trap, not a debugging aid.
    if (!in.enabled) {
        std::vector<Candidate>& cands = out.scratchLoad; // reused; no per-call allocation
        cands.clear();
        for (u32 i = 0; i < in.count; ++i) {
            const PolicyShard& s = in.shards[i];
            if (s.resident || s.busy || s.failed) continue;
            cands.push_back({i, s.priority, 0.0f});
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            return a.priority > b.priority;
        });
        const u32 room = in.maxConcurrent > in.inFlight ? in.maxConcurrent - in.inFlight : 0u;
        for (const Candidate& c : cands) {
            if (out.load.size() >= room) {
                out.moreWork = true;
                break;
            }
            out.load.push_back(c.index);
        }
        return;
    }

    // Reused caller-owned scratch, so the "allocation-free once warm" contract in the
    // header is actually true (these were function locals - a heap pair per call).
    std::vector<Candidate>& loads = out.scratchLoad;
    std::vector<Candidate>& unloads = out.scratchUnload;
    loads.clear();
    unloads.clear();
    for (u32 i = 0; i < in.count; ++i) {
        const PolicyShard& s = in.shards[i];
        f32 d = 0.0f;
        // RULE 3: no focus means nothing changes. Not "unload everything".
        if (!NearestFocus(s, in.foci, in.fociCount, d)) return;
        if (d <= s.loadRadius) ++out.inRange;

        if (s.resident) {
            if (s.pinned) continue;
            // RULE 2: outside the UNLOAD radius, which is strictly larger than the load
            // radius, so the boundary cannot oscillate. The `d > 0` half is belt and
            // braces: a corrected band already implies it, but a shard the focus is
            // physically inside must never be able to vanish, whatever the radii say.
            if (d > s.unloadRadius && d > 0.0f) unloads.push_back({i, s.priority, d});
            continue;
        }
        if (s.busy || s.failed) continue;
        if (d <= s.loadRadius) loads.push_back({i, s.priority, d});
    }

    // RULE 4: priority first, then nearest. stable_sort so an exact tie keeps shard
    // order, which keeps the whole evaluation deterministic.
    std::stable_sort(loads.begin(), loads.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.dist < b.dist;
    });
    // Furthest first: the shard most safely out of sight is the one to drop.
    std::stable_sort(unloads.begin(), unloads.end(), [](const Candidate& a, const Candidate& b) {
        return a.dist > b.dist;
    });

    const u32 room = in.maxConcurrent > in.inFlight ? in.maxConcurrent - in.inFlight : 0u;
    for (const Candidate& c : loads) {
        if (out.load.size() >= room) {
            out.moreWork = true; // RULE 5
            break;
        }
        out.load.push_back(c.index);
    }
    for (const Candidate& c : unloads) {
        if (out.unload.size() >= in.maxUnloads) {
            out.moreWork = true;
            break;
        }
        out.unload.push_back(c.index);
    }
}

// --- Self-test ----------------------------------------------------------------

bool PolicySelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("tagpolicy: FAILED - {}", what);
        }
    };

    // A shard with a corrected band, exactly as tags::Normalize produces one.
    const auto band = [](f32 load) {
        f32 unload = salvage::DefaultUnloadRadius(load);
        salvage::EnforceHysteresis(load, unload);
        return unload;
    };

    PolicyOut out;

    // --- 1. Distance is to the BOX, not the centre --------------------------
    {
        // A 300 m wall along X, 1 m thick. Its centre is 150 m from either end.
        const glm::vec3 mn(0.0f, 0.0f, -0.5f), mx(300.0f, 4.0f, 0.5f);
        const glm::vec3 centre = (mn + mx) * 0.5f;
        const glm::vec3 atTheEnd(0.0f, 0.0f, 3.0f); // standing against the wall
        expect(DistanceToBox(mn, mx, atTheEnd) < 3.01f,
               "a player against one end of a 300 m wall is ~2.5 m from it, not 150 m");
        expect(glm::distance(centre, atTheEnd) > 149.0f,
               "...and the centre measurement really would have said ~150 m (the bug)");
        expect(DistanceToBox(mn, mx, glm::vec3(150.0f, 2.0f, 0.0f)) == 0.0f,
               "a point inside the box is at distance 0");

        PolicyShard s;
        s.min = mn;
        s.max = mx;
        s.loadRadius = 50.0f;
        s.unloadRadius = band(50.0f);
        const glm::vec3 focus = atTheEnd;
        PolicyIn in;
        in.shards = &s;
        in.count = 1;
        in.foci = &focus;
        in.fociCount = 1;
        Evaluate(in, out);
        expect(out.load.size() == 1 && out.unload.empty(),
               "the wall LOADS for a player standing against it (centre distance would "
               "have refused)");
        s.resident = true;
        Evaluate(in, out);
        expect(out.unload.empty(), "and never unloads while the player is against it");
    }

    // --- 2. Hysteresis: a focus oscillating on the boundary does not thrash --
    {
        PolicyShard s;
        s.min = glm::vec3(-5.0f);
        s.max = glm::vec3(5.0f);
        s.loadRadius = 100.0f;
        s.unloadRadius = band(100.0f); // 135
        u32 spawns = 0, despawns = 0;
        // Sweep back and forth ACROSS the load boundary 200 times. With a correct band
        // this is one spawn and no despawn; with unload == load it is 200 of each.
        for (u32 step = 0; step < 200; ++step) {
            const f32 x = (step % 2 == 0) ? 99.0f : 101.0f;
            const glm::vec3 focus(x + 5.0f, 0.0f, 0.0f); // +5 = the box's own half-extent
            PolicyIn in;
            in.shards = &s;
            in.count = 1;
            in.foci = &focus;
            in.fociCount = 1;
            Evaluate(in, out);
            if (!out.load.empty()) {
                ++spawns;
                s.resident = true;
            }
            if (!out.unload.empty()) {
                ++despawns;
                s.resident = false;
            }
        }
        expect(spawns == 1 && despawns == 0,
               "oscillating on the LOAD boundary spawns once and never despawns");

        // Now prove the band is what did it: a degenerate band thrashes.
        PolicyShard bad = s;
        bad.resident = false;
        bad.unloadRadius = bad.loadRadius; // what EnforceHysteresis exists to prevent
        u32 badSpawns = 0, badDespawns = 0;
        for (u32 step = 0; step < 20; ++step) {
            const f32 x = (step % 2 == 0) ? 99.0f : 101.0f;
            const glm::vec3 focus(x + 5.0f, 0.0f, 0.0f);
            PolicyIn in;
            in.shards = &bad;
            in.count = 1;
            in.foci = &focus;
            in.fociCount = 1;
            Evaluate(in, out);
            if (!out.load.empty()) {
                ++badSpawns;
                bad.resident = true;
            }
            if (!out.unload.empty()) {
                ++badDespawns;
                bad.resident = false;
            }
        }
        expect(badSpawns > 5 && badDespawns > 5,
               "a DEGENERATE band (unload == load) really does thrash - so the band is "
               "what stopped it, not the shape of the test");
    }

    // --- 3. Two foci: union loads, intersection unloads ---------------------
    {
        PolicyShard s;
        s.min = glm::vec3(-1.0f);
        s.max = glm::vec3(1.0f);
        s.loadRadius = 50.0f;
        s.unloadRadius = band(50.0f); // 67.5
        const glm::vec3 far3(1000.0f, 0.0f, 0.0f);
        const glm::vec3 near3(20.0f, 0.0f, 0.0f);
        {
            const glm::vec3 foci[2] = {far3, near3}; // camera far, player near
            PolicyIn in;
            in.shards = &s;
            in.count = 1;
            in.foci = foci;
            in.fociCount = 2;
            Evaluate(in, out);
            expect(out.load.size() == 1, "ANY focus in range loads (a cutscene camera counts)");
        }
        s.resident = true;
        {
            const glm::vec3 foci[2] = {far3, near3};
            PolicyIn in;
            in.shards = &s;
            in.count = 1;
            in.foci = foci;
            in.fociCount = 2;
            Evaluate(in, out);
            expect(out.unload.empty(),
                   "one focus still nearby keeps it resident even though the other is 1 km away");
        }
        {
            const glm::vec3 foci[2] = {far3, glm::vec3(-1000.0f, 0.0f, 0.0f)};
            PolicyIn in;
            in.shards = &s;
            in.count = 1;
            in.foci = foci;
            in.fociCount = 2;
            Evaluate(in, out);
            expect(out.unload.size() == 1, "EVERY focus out of unload range unloads it");
        }
        {
            PolicyIn in; // no foci at all
            in.shards = &s;
            in.count = 1;
            Evaluate(in, out);
            expect(out.load.empty() && out.unload.empty(),
                   "an EMPTY focus list changes nothing - it is not 'infinitely far away'");
        }
    }

    // --- 4. Priority, then distance; throttle; unload cap -------------------
    {
        std::vector<PolicyShard> ss(6);
        for (u32 i = 0; i < 6; ++i) {
            const f32 x = 10.0f * static_cast<f32>(i + 1); // 10, 20, .. 60 m away
            ss[i].min = glm::vec3(x, 0.0f, 0.0f);
            ss[i].max = glm::vec3(x + 1.0f, 1.0f, 1.0f);
            ss[i].loadRadius = 500.0f;
            ss[i].unloadRadius = band(500.0f);
        }
        ss[4].priority = 10; // the FURTHEST-but-one, promoted
        ss[5].priority = 10; // the furthest, promoted
        const glm::vec3 focus(0.0f);
        PolicyIn in;
        in.shards = ss.data();
        in.count = 6;
        in.foci = &focus;
        in.fociCount = 1;
        in.maxConcurrent = 3;
        Evaluate(in, out);
        expect(out.load.size() == 3 && out.moreWork,
               "the concurrency throttle caps the batch at 3 and reports moreWork");
        expect(out.load.size() == 3 && out.load[0] == 4 && out.load[1] == 5,
               "priority wins over distance, and distance breaks the priority tie (4 before 5)");
        expect(out.load.size() == 3 && out.load[2] == 0,
               "then the nearest of the rest");
        expect(out.inRange == 6, "inRange counts every shard within a load radius");

        in.inFlight = 2; // two already loading
        Evaluate(in, out);
        expect(out.load.size() == 1 && out.moreWork,
               "in-flight loads consume the same budget (room = max - inFlight)");
        in.inFlight = 3;
        Evaluate(in, out);
        expect(out.load.empty() && out.moreWork, "a full pipe orders nothing");

        // Unload cap: put the focus 10 km away with everything resident.
        for (PolicyShard& s : ss) s.resident = true;
        const glm::vec3 gone(100000.0f, 0.0f, 0.0f);
        in.foci = &gone;
        in.inFlight = 0;
        in.maxUnloads = 1;
        Evaluate(in, out);
        expect(out.unload.size() == 1 && out.moreWork,
               "unloads are capped per evaluation and report moreWork too");
        expect(out.unload.size() == 1 && out.unload[0] == 0,
               "and the FURTHEST goes first (shard 0 is furthest from x=100000)");
        in.maxUnloads = 99;
        Evaluate(in, out);
        expect(out.unload.size() == 6 && !out.moreWork,
               "raising the cap drains them all in one evaluation");
    }

    // --- 5. pinned / failed / disabled --------------------------------------
    {
        // 0 = resident+pinned, 1 = failed, 2 = busy, 3 = a plain idle shard.
        std::vector<PolicyShard> ss(4);
        for (PolicyShard& s : ss) {
            s.min = glm::vec3(-1.0f);
            s.max = glm::vec3(1.0f);
            s.loadRadius = 10.0f;
            s.unloadRadius = band(10.0f);
        }
        ss[0].resident = true;
        ss[0].pinned = true;
        ss[1].failed = true;
        ss[2].busy = true;
        const glm::vec3 gone(1000.0f, 0.0f, 0.0f);
        PolicyIn in;
        in.shards = ss.data();
        in.count = 4;
        in.foci = &gone;
        in.fociCount = 1;
        Evaluate(in, out);
        expect(out.unload.empty(), "a PINNED resident shard is never unloaded, however far away");
        expect(out.load.empty(), "and nothing loads from 1 km away");
        const glm::vec3 here(0.0f);
        in.foci = &here;
        Evaluate(in, out);
        expect(out.load.size() == 1 && out.load[0] == 3,
               "in range, ONLY the plain shard is ordered: a FAILED shard is terminal (per "
               "SALVAGE 3's correction) and a BUSY one is not ordered twice");

        in.enabled = false;
        in.foci = &gone; // nowhere near anything
        in.maxConcurrent = 8;
        Evaluate(in, out);
        expect(out.load.size() == 1 && out.load[0] == 3 && out.unload.empty(),
               "DISABLED loads the idle shard regardless of distance and unloads nothing");
        expect(out.residentCount == 1, "residentCount is reported either way");
    }

    if (ok) HBE_INFO("tagpolicy: PASS");
    return ok;
}

} // namespace hbe::stream

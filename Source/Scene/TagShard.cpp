// Scene/TagShard.cpp - the save-time spatial shard bake. See TagShard.h.
#include "Scene/TagShard.h"

#include "Core/Log.h"
#include "Scene/Scene.h"
#include "Scene/StreamPolicy.h" // stream::kMaxAssocDepth - RULE 6's hop cap
#include "Scene/TagTable.h"
#include "Scene/TerrainSystem.h" // terrain::ExtentXZ - the ONE heightfield layout

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hbe::tagshard {

namespace fs = std::filesystem;
namespace {

// --- Small geometry helpers ---------------------------------------------------
struct Box {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;

    void Add(const glm::vec3& p) {
        if (!valid) {
            min = max = p;
            valid = true;
            return;
        }
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void Add(const Box& b) {
        if (!b.valid) return;
        Add(b.min);
        Add(b.max);
    }
    glm::vec3 Center() const { return (min + max) * 0.5f; }
    f32 Volume() const {
        if (!valid) return 0.0f;
        const glm::vec3 d = glm::max(max - min, glm::vec3(0.0f));
        return d.x * d.y * d.z;
    }
    f32 Diagonal() const { return valid ? glm::length(max - min) : 0.0f; }
};

// The 8-corner transform. Open-coded at five sites in this tree already (shadow
// fitting, ray picking, drop-point projection); one copy here, because a local box
// under a rotated/scaled world matrix is NOT its min/max transformed.
Box WorldBoxOf(const glm::mat4& world, const glm::vec3& lmin, const glm::vec3& lmax) {
    Box b;
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner{(c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y,
                               (c & 4) ? lmax.z : lmin.z};
        b.Add(glm::vec3(world * glm::vec4(corner, 1.0f)));
    }
    return b;
}

f32 IntersectVolume(const scene::ShardDesc& a, const scene::ShardDesc& b) {
    const glm::vec3 lo = glm::max(a.min, b.min);
    const glm::vec3 hi = glm::min(a.max, b.max);
    const glm::vec3 d = hi - lo;
    if (d.x <= 0.0f || d.y <= 0.0f || d.z <= 0.0f) return 0.0f;
    return d.x * d.y * d.z;
}
f32 VolumeOf(const scene::ShardDesc& s) {
    const glm::vec3 d = glm::max(s.max - s.min, glm::vec3(0.0f));
    return d.x * d.y * d.z;
}

// --- Union-find over grid cells ------------------------------------------------
struct Dsu {
    std::vector<u32> parent;
    void Reset(usize n) {
        parent.resize(n);
        for (usize i = 0; i < n; ++i) parent[i] = static_cast<u32>(i);
    }
    u32 Find(u32 a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]]; // path halving
            a = parent[a];
        }
        return a;
    }
    void Union(u32 a, u32 b) {
        a = Find(a);
        b = Find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b); // lower id wins: deterministic
    }
};

// --- Local extents from component-declared volumes ----------------------------
// A meshless 40 m trigger volume must not bake to a 2 m point, and a light's reach
// is part of what "this shard is nearby" means. Every one of these is LOCAL space
// (the entity's world scale applies through the world matrix), matching how the
// components themselves are documented and consumed.
void AddHalf(Box& b, const glm::vec3& half) {
    b.Add(-half);
    b.Add(half);
}
void AddSphere(Box& b, const glm::vec3& center, f32 r) {
    b.Add(center - glm::vec3(r));
    b.Add(center + glm::vec3(r));
}

// The TagDef a tag id resolves to. A scene-only tag (auto-interned from a file but
// absent from the project's list) gets DEFAULTS rather than being dropped: dropping
// it would fold its content into the always-resident set without telling anyone.
TagDef DefFor(const std::vector<TagDef>& defs, TagId id) {
    if (id != kTagUntagged && static_cast<usize>(id) < defs.size()) return defs[id];
    TagDef d;
    d.name = tags::Name(id);
    return d;
}

std::string ShardKeyName(const std::vector<TagDef>& defs, TagId id) {
    const std::string& live = tags::Name(id);
    if (!live.empty()) return live;
    if (static_cast<usize>(id) < defs.size() && !defs[id].name.empty()) return defs[id].name;
    return "Tag" + std::to_string(static_cast<u32>(id));
}

// Cap on how many findings of one kind get their own line, so one badly authored
// tag cannot bury the rest of the report.
constexpr usize kMaxSameKind = 8;

struct Reporter {
    BakeReport* out = nullptr;
    void Add(Severity sev, const std::string& tag, std::string msg) {
        if (sev == Severity::Error) ++out->errors;
        else ++out->warnings;
        out->diagnostics.push_back({sev, tag, std::move(msg)});
    }
};

} // namespace

std::vector<std::pair<u32, u32>> HeavyOverlaps(const std::vector<scene::ShardDesc>& shards) {
    std::vector<std::pair<u32, u32>> out;
    for (usize a = 0; a < shards.size(); ++a) {
        for (usize b = a + 1; b < shards.size(); ++b) {
            const f32 inter = IntersectVolume(shards[a], shards[b]);
            if (inter <= 0.0f) continue;
            const f32 smaller = std::min(VolumeOf(shards[a]), VolumeOf(shards[b]));
            if (smaller <= 0.0f || inter < kOverlapWarnFraction * smaller) continue;
            out.emplace_back(static_cast<u32>(a), static_cast<u32>(b));
        }
    }
    return out;
}

f32 DistanceToShard(const scene::ShardDesc& s, const glm::vec3& p) {
    const glm::vec3 d = glm::max(glm::max(s.min - p, p - s.max), glm::vec3(0.0f));
    return glm::length(d);
}

BakeReport Bake(std::vector<BakeRow>& rows, const std::vector<TagDef>& defs) {
    BakeReport rep;
    Reporter report{&rep};
    const usize n = rows.size();
    for (BakeRow& r : rows) {
        r.shard = -1;
        r.resolvedTag = kTagUntagged;
    }
    if (n == 0) return rep;

    // --- 1. Hierarchy: root of every row, and each root's owned rows -----------
    // Depth-capped and cycle-guarded: a malformed row set must produce a report,
    // not a hang. A row whose parent index is out of range is treated as a root.
    std::vector<i32> rootOf(n, -1);
    usize badParents = 0, cycles = 0;
    for (usize i = 0; i < n; ++i) {
        usize cur = i;
        int guard = 0;
        while (guard++ < 64) {
            const i32 p = rows[cur].parent;
            if (p < 0) break;
            if (p >= static_cast<i32>(n) || static_cast<usize>(p) == cur) {
                ++badParents;
                break;
            }
            cur = static_cast<usize>(p);
        }
        if (guard >= 64) ++cycles;
        rootOf[i] = static_cast<i32>(cur);
    }
    if (badParents > 0) {
        report.Add(Severity::Error, "",
                   std::to_string(badParents) +
                       " entity(ies) reference a parent row that does not exist; each was "
                       "treated as a root for sharding.");
    }
    if (cycles > 0) {
        report.Add(Severity::Error, "",
                   std::to_string(cycles) +
                       " entity(ies) sit in a parent CYCLE (or deeper than 64 levels); "
                       "their shard assignment is not meaningful.");
    }

    // Rows owned by each root, in ascending row order (determinism).
    std::vector<std::vector<u32>> owned(n);
    for (usize i = 0; i < n; ++i) owned[static_cast<usize>(rootOf[i])].push_back(static_cast<u32>(i));

    // --- 2. World bounds per row, then per subtree ------------------------------
    std::vector<Box> worldBox(n);
    for (usize i = 0; i < n; ++i) {
        const BakeRow& r = rows[i];
        glm::vec3 lmin = r.localMin, lmax = r.localMax;
        if (!r.hasExtent || lmax.x < lmin.x || lmax.y < lmin.y || lmax.z < lmin.z) {
            lmin = glm::vec3(-kMinHalfExtent);
            lmax = glm::vec3(kMinHalfExtent);
        }
        worldBox[i] = WorldBoxOf(r.world, lmin, lmax);
    }
    std::vector<Box> subtreeBox(n);
    for (usize i = 0; i < n; ++i) {
        if (owned[i].empty()) continue; // not a root
        for (const u32 m : owned[i]) subtreeBox[i].Add(worldBox[m]);
    }

    // --- 3. The atom's tag ------------------------------------------------------
    // A SUBTREE IS ONE ATOM (TagShard.h): its members must all land in one shard or
    // the loader turns a cross-slice child into a root at its LOCAL transform. So
    // one tag per subtree, resolved here:
    //   * root tagged      -> the root's tag wins; a differing descendant is an ERROR
    //                         (its tag is being ignored, and the author must know).
    //   * root untagged, exactly one tag below -> PROMOTE it to the whole subtree,
    //     with a warning. This is the normal Inspector path (tags::AssignSubtree is
    //     called on the SELECTED entity, which is often a child), and promoting is
    //     the only reading under which the author's click does anything at all.
    //   * root untagged, several tags below -> subtree stays resident, ERROR naming
    //     the conflict. Picking one silently would move content between groups.
    std::vector<TagId> atomTag(n, kTagUntagged);
    usize mismatchLines = 0, promoteLines = 0, conflictLines = 0;
    for (usize root = 0; root < n; ++root) {
        if (owned[root].empty()) continue;
        std::vector<TagId> distinct;
        for (const u32 m : owned[root]) {
            const TagId t = rows[m].tag;
            if (t == kTagUntagged) continue;
            if (std::find(distinct.begin(), distinct.end(), t) == distinct.end())
                distinct.push_back(t);
        }
        std::sort(distinct.begin(), distinct.end());
        const TagId rt = rows[root].tag;
        if (rt != kTagUntagged) {
            atomTag[root] = rt;
            for (const u32 m : owned[root]) {
                if (rows[m].tag == kTagUntagged || rows[m].tag == rt) continue;
                if (mismatchLines++ < kMaxSameKind) {
                    report.Add(Severity::Error, ShardKeyName(defs, rows[m].tag),
                               "CROSS-SHARD PARENT: '" + rows[m].name + "' is tagged '" +
                                   ShardKeyName(defs, rows[m].tag) + "' but its subtree root '" +
                                   rows[root].name + "' is tagged '" + ShardKeyName(defs, rt) +
                                   "'. A subtree cannot be split across shards, so it was "
                                   "baked into the ROOT's shard and its own tag is ignored. "
                                   "Tag the root instead, or unparent it.");
                }
            }
        } else if (distinct.size() == 1) {
            atomTag[root] = distinct[0];
            if (promoteLines++ < kMaxSameKind) {
                report.Add(Severity::Warning, ShardKeyName(defs, distinct[0]),
                           "Tag '" + ShardKeyName(defs, distinct[0]) +
                               "' was set below the untagged root '" + rows[root].name +
                               "', so the WHOLE subtree (root included) joins that "
                               "streaming group - a subtree cannot be split across shards.");
            }
        } else if (distinct.size() > 1) {
            std::string list;
            for (const TagId t : distinct) list += (list.empty() ? "" : ", ") + ShardKeyName(defs, t);
            if (conflictLines++ < kMaxSameKind) {
                report.Add(Severity::Error, "",
                           "CROSS-SHARD PARENT: the subtree under untagged root '" +
                               rows[root].name + "' carries several tags (" + list +
                               "). A subtree is one streaming unit, so it stays RESIDENT "
                               "and none of those tags take effect. Give the root one tag.");
            }
        }
    }

    // PUBLISH the resolution, per row, so the caller can write BOTH halves back. The
    // atom's tag is a property of the SUBTREE, not of the row that happened to carry
    // the author's click: a promoted subtree's untagged members and a cross-shard
    // descendant whose own tag was overridden are counted in the shard's member count,
    // so the file has to say so on every one of them or the whole table reads as stale.
    // See BakeRow::resolvedTag. (Deliberately AFTER step 3 and independent of
    // `rows[].tag`, which stays the AUTHORED value so the diagnostics below - and the
    // `.hbui` check next - still report what the author actually set.)
    for (usize i = 0; i < n; ++i) rows[i].resolvedTag = atomTag[static_cast<usize>(rootOf[i])];

    // --- 4. `.hbui` document entities may never be tagged -----------------------
    {
        usize lines = 0;
        for (usize i = 0; i < n; ++i) {
            if (!rows[i].uiDoc || rows[i].tag == kTagUntagged) continue;
            if (lines++ < kMaxSameKind) {
                report.Add(Severity::Error, ShardKeyName(defs, rows[i].tag),
                           "'" + rows[i].name +
                               "' is a `.hbui` DOCUMENT entity carrying a streaming tag. UI "
                               "is asset content outside the streamed world, so that group "
                               "can never spawn or despawn. The tag is ignored.");
            }
        }
    }

    // --- 5. Cluster each tag ----------------------------------------------------
    // Roots per tag, ascending. std::map, not unordered: shard ORDER is content.
    std::map<TagId, std::vector<u32>> rootsByTag;
    for (usize root = 0; root < n; ++root) {
        if (owned[root].empty() || atomTag[root] == kTagUntagged) continue;
        rootsByTag[atomTag[root]].push_back(static_cast<u32>(root));
    }

    Dsu dsu;
    for (const auto& [tag, roots] : rootsByTag) {
        const TagDef def = DefFor(defs, tag);
        const std::string tagName = ShardKeyName(defs, tag);
        const f32 cell = def.shardCell > 0.0f ? def.shardCell
                                              : std::max(def.loadRadius, kMinShardCell);
        // Groups of roots, one per shard-to-be.
        std::vector<std::vector<u32>> groups;
        const bool single = def.alwaysLoaded || !def.autoShard || roots.size() <= 1;
        if (single) {
            // An alwaysLoaded tag is never distance-tested, and autoShard off is the
            // author saying "one group, I will split it by hand".
            groups.push_back(roots);
        } else {
            // Two passes, so a cell's DSU id comes from its SORTED position and not
            // from the order the rows happened to arrive in. Union-find picks the
            // lowest id as each component's representative, so row-order-derived ids
            // would make component order (and therefore every downstream tie-break,
            // including the shard-cap merge) depend on registry layout.
            using Cell = std::pair<i32, i32>;
            std::vector<std::vector<Cell>> rootKeys(roots.size());
            std::map<Cell, u32> cellId;
            for (usize k = 0; k < roots.size(); ++k) {
                const Box& b = subtreeBox[roots[k]];
                const i32 ix0 = static_cast<i32>(std::floor(b.min.x / cell));
                const i32 ix1 = static_cast<i32>(std::floor(b.max.x / cell));
                const i32 iz0 = static_cast<i32>(std::floor(b.min.z / cell));
                const i32 iz1 = static_cast<i32>(std::floor(b.max.z / cell));
                // A root far larger than the cell is strided rather than fully
                // enumerated: bounded work, and its tag gets a spread warning anyway.
                const i64 spanX = static_cast<i64>(ix1) - ix0 + 1;
                const i64 spanZ = static_cast<i64>(iz1) - iz0 + 1;
                i64 stride = 1;
                const auto cells = [&](i64 s) {
                    return ((spanX + s - 1) / s) * ((spanZ + s - 1) / s);
                };
                while (cells(stride) > static_cast<i64>(kMaxCellsPerRoot)) ++stride;
                for (i64 ix = ix0; ix <= ix1; ix += stride)
                    for (i64 iz = iz0; iz <= iz1; iz += stride)
                        rootKeys[k].emplace_back(static_cast<i32>(ix), static_cast<i32>(iz));
                // Always include the far corner, so a strided range still spans the
                // whole extent instead of stopping short of it.
                rootKeys[k].emplace_back(ix1, iz1);
                for (const Cell& c : rootKeys[k]) cellId.emplace(c, 0u);
            }
            {
                u32 next = 0;
                for (auto& [key, id] : cellId) id = next++;
            }
            std::vector<std::vector<u32>> rootCells(roots.size());
            for (usize k = 0; k < roots.size(); ++k) {
                rootCells[k].reserve(rootKeys[k].size());
                for (const Cell& c : rootKeys[k]) rootCells[k].push_back(cellId.at(c));
            }
            dsu.Reset(cellId.size());
            // A root is one atom: every cell it touches is the same component.
            for (const std::vector<u32>& cells : rootCells)
                for (usize c = 1; c < cells.size(); ++c) dsu.Union(cells[0], cells[c]);
            // 8-neighbour adjacency between OCCUPIED cells (two things closer than a
            // load radius always load together anyway).
            for (const auto& [key, id] : cellId) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 dz = -1; dz <= 1; ++dz) {
                        if (dx == 0 && dz == 0) continue;
                        const auto it = cellId.find({key.first + dx, key.second + dz});
                        if (it != cellId.end()) dsu.Union(id, it->second);
                    }
                }
            }
            std::map<u32, std::vector<u32>> byComponent; // sorted by representative
            for (usize k = 0; k < roots.size(); ++k)
                byComponent[dsu.Find(rootCells[k][0])].push_back(roots[k]);
            for (auto& [comp, rs] : byComponent) groups.push_back(std::move(rs));
        }

        // Cap: merge the smallest into its nearest neighbour until under the limit.
        if (groups.size() > kMaxShardsPerTag) {
            report.Add(Severity::Warning, tagName,
                       "Tag '" + tagName + "' clustered into " +
                           std::to_string(groups.size()) + " shards; merged down to " +
                           std::to_string(kMaxShardsPerTag) +
                           ". Raise its shard cell (or split the tag) - this many "
                           "streaming units costs more to evaluate than it saves.");
            std::vector<Box> gb(groups.size());
            for (usize g = 0; g < groups.size(); ++g)
                for (const u32 r : groups[g]) gb[g].Add(subtreeBox[r]);
            // Merging picks "the smallest, ties to the LOWEST INDEX", so it is only
            // deterministic because the group order is already GEOMETRIC: cell ids are
            // handed out in sorted (ix, iz) order above, union-find keeps the lowest id
            // as each component's representative, and `byComponent` is a sorted map of
            // those. --test-shardbake pins it by re-baking the same 400 roots in a
            // shuffled order and demanding an identical shard table - mutating the cell
            // ids back to arrival order makes that assertion fail.
            while (groups.size() > kMaxShardsPerTag) {
                usize small = 0;
                for (usize g = 1; g < groups.size(); ++g)
                    if (groups[g].size() < groups[small].size()) small = g;
                usize near = static_cast<usize>(-1);
                f32 best = 0.0f;
                for (usize g = 0; g < groups.size(); ++g) {
                    if (g == small) continue;
                    const f32 d = glm::length(gb[g].Center() - gb[small].Center());
                    if (near == static_cast<usize>(-1) || d < best) {
                        near = g;
                        best = d;
                    }
                }
                if (near == static_cast<usize>(-1)) break;
                groups[near].insert(groups[near].end(), groups[small].begin(),
                                    groups[small].end());
                std::sort(groups[near].begin(), groups[near].end());
                gb[near].Add(gb[small]);
                groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(small));
                gb.erase(gb.begin() + static_cast<std::ptrdiff_t>(small));
            }
        }

        // Shard AABBs, then a GEOMETRIC order so the index a shard gets does not
        // depend on the order the rows arrived in (--test-shardbake shuffles them).
        struct Built {
            Box box;
            std::vector<u32> roots;
            std::vector<u32> members;
        };
        std::vector<Built> built;
        built.reserve(groups.size());
        for (std::vector<u32>& rs : groups) {
            Built b;
            std::sort(rs.begin(), rs.end());
            for (const u32 r : rs) {
                b.box.Add(subtreeBox[r]);
                for (const u32 m : owned[r]) b.members.push_back(m);
            }
            std::sort(b.members.begin(), b.members.end());
            b.roots = std::move(rs);
            built.push_back(std::move(b));
        }
        std::sort(built.begin(), built.end(), [](const Built& a, const Built& b) {
            if (a.box.min.x != b.box.min.x) return a.box.min.x < b.box.min.x;
            if (a.box.min.z != b.box.min.z) return a.box.min.z < b.box.min.z;
            if (a.box.min.y != b.box.min.y) return a.box.min.y < b.box.min.y;
            return a.members.size() > b.members.size();
        });

        TagStat st;
        st.tag = tagName;
        st.id = tag;
        st.members = 0;
        st.roots = static_cast<u32>(roots.size());
        st.loadRadius = def.loadRadius;
        st.cell = single ? 0.0f : cell;
        st.alwaysLoaded = def.alwaysLoaded;
        st.autoShard = def.autoShard;
        st.shards = static_cast<u32>(built.size());

        Box tagBox;
        f32 volumeSum = 0.0f;
        const usize firstShard = rep.shards.size();
        for (usize g = 0; g < built.size(); ++g) {
            scene::ShardDesc d;
            d.tag = tagName;
            d.index = static_cast<u32>(g);
            d.min = built[g].box.min - glm::vec3(kBoundsPad);
            d.max = built[g].box.max + glm::vec3(kBoundsPad);
            d.count = static_cast<u32>(built[g].members.size());
            for (const u32 m : built[g].members) {
                rows[m].shard = static_cast<i32>(g);
                ++rep.tagged;
            }
            st.members += d.count;
            st.largestDiagonal = std::max(st.largestDiagonal, glm::length(d.max - d.min));
            tagBox.Add(built[g].box);
            volumeSum += VolumeOf(d);
            rep.shards.push_back(std::move(d));
            rep.members.push_back(std::move(built[g].members));
        }

        const f32 tagVolume = tagBox.Volume();
        st.coherence = tagVolume > 0.0f ? volumeSum / tagVolume : 1.0f;
        rep.stats.push_back(st);

        // --- Validator: pathological spread ---
        // The degenerate case this whole feature exists for: a tag whose members are
        // scattered over the level so its ONE shard is level-sized. It reads as
        // "streaming enabled" in the Inspector and behaves as always-loaded.
        if (!def.alwaysLoaded && st.shards == 1 &&
            st.largestDiagonal > kSpreadRadiiWarn * std::max(def.loadRadius, 1.0f)) {
            report.Add(Severity::Warning, tagName,
                       "Tag '" + tagName + "' baked to ONE shard " +
                           std::to_string(static_cast<int>(st.largestDiagonal)) +
                           " m across, with a load radius of " +
                           std::to_string(static_cast<int>(def.loadRadius)) +
                           " m. It is effectively ALWAYS LOADED: the player is inside its "
                           "bounds nearly everywhere." +
                           (def.autoShard ? std::string(" Lower its shard cell, or split "
                                                        "the objects into separate tags.")
                                          : std::string(" Auto-sharding is OFF for this "
                                                        "tag - turn it on.")));
        }
        // --- Validator: heavy overlap ---
        // Two shards that mostly occupy the same volume are not two streaming units:
        // entering either pulls in both, so the split costs evaluation and buys nothing.
        {
            const std::vector<scene::ShardDesc> mine(rep.shards.begin() +
                                                         static_cast<std::ptrdiff_t>(firstShard),
                                                     rep.shards.end());
            usize lines = 0;
            for (const auto& [a, b] : HeavyOverlaps(mine)) {
                if (lines++ >= kMaxSameKind) break;
                const f32 inter = IntersectVolume(mine[a], mine[b]);
                const f32 smaller = std::min(VolumeOf(mine[a]), VolumeOf(mine[b]));
                report.Add(Severity::Warning, tagName,
                           "Shards '" + tagName + "#" + std::to_string(mine[a].index) + "' and '" +
                               tagName + "#" + std::to_string(mine[b].index) + "' overlap by " +
                               std::to_string(static_cast<int>(100.0f * inter /
                                                              std::max(smaller, 1e-6f))) +
                               "% of the smaller one. They will nearly always load "
                               "together; merging them (a larger shard cell) is cheaper.");
            }
        }
        // NOTE: the former "navmesh input inside a streamed tag" warning is GONE.
        // Navigation is now BAKED into a persistent .hbnav (Recast) and streamed
        // INDEPENDENTLY of the tags, so nav geometry inside a streamed tag is captured at
        // bake time and AI keeps its ground even while the visual shard is unloaded - the
        // exact hazard that warning described no longer exists. (A streamed prop that
        // MOVES at runtime is not re-baked and should carry a NavigationObstacle instead;
        // that is a per-object authoring note, not a per-tag bake error. `rows[].navInput`
        // is kept for that future per-object check.)
        // --- Validator: chunked terrain inside a streamed tag ---
        // The one place a respawn cycle GROWS VRAM. Paint canvases and world-UI render
        // targets are cached across respawns precisely because the RHI has no texture or
        // mesh destroy (TagStreaming.h); terrain chunk meshes are not - terrain::Sync
        // calls renderer.UploadMesh per chunk with no cache key, so every cycle of a
        // tagged terrain adds a whole chunk set that nothing can free. Terrain is also
        // usually the world FLOOR and a monolithic component no tag can subdivide, so
        // streaming it is almost never what the author meant.
        if (!def.alwaysLoaded) {
            usize terrainCount = 0;
            std::string firstTerrain;
            for (usize s = firstShard; s < rep.members.size(); ++s) {
                for (const u32 m : rep.members[s]) {
                    if (!rows[m].terrain) continue;
                    if (terrainCount++ == 0) firstTerrain = rows[m].name;
                }
            }
            if (terrainCount > 0) {
                report.Add(Severity::Warning, tagName,
                           std::to_string(terrainCount) + " chunked TERRAIN component(s) (e.g. '" +
                               firstTerrain + "') are in the streamed tag '" + tagName +
                               "'. Terrain is one monolithic component (no tag subdivides it), "
                               "it is usually the world floor, and its chunk meshes are "
                               "RE-UPLOADED on every respawn with no way to free them - so each "
                               "streaming cycle grows VRAM. Mark the tag alwaysLoaded, or move "
                               "the terrain out of it.");
            }
        }
    }

    // --- 5b. RULE 6: ASSOCIATED TAGS -------------------------------------------
    // An association is a PROJECT-level relation, but whether it does anything is a
    // LEVEL-level question, so it is validated here where both are in hand. Every
    // finding is a WARNING and none blocks the save: the same reasoning as the rest
    // of this file, and more so - refusing to write a level over a tag relation that
    // is not in the file being written would throw away authored work for nothing.
    //
    // Scoped to DRIVERS PRESENT IN THIS BAKE. The editor re-bakes once per scene
    // source, so reporting every project-wide relation from every bake would say the
    // same thing N times about tags this level has never heard of.
    {
        std::unordered_set<std::string> present;
        for (const scene::ShardDesc& sd : rep.shards) present.insert(sd.tag);
        std::unordered_map<std::string, usize> byName;
        byName.reserve(defs.size());
        for (usize t = 0; t < defs.size(); ++t) byName.emplace(defs[t].name, t);

        // THREE BUDGETS, NOT ONE. These used to share a counter, so eight dangling or
        // content-less associations - routine in a multi-scene project, where "the
        // target has no shards in this level" fires once per bake and the editor re-bakes
        // once per scene source - consumed the whole allowance and the CYCLE and DEPTH
        // scans below never ran at all. The cycle message is the finding this validation
        // most exists to produce, and it was the one silently dropped.
        usize linkFindings = 0, cycleFindings = 0, depthFindings = 0;
        const auto sayIn = [&](usize& budget, const std::string& tag, std::string msg) {
            if (budget++ >= kMaxSameKind) return;
            report.Add(Severity::Warning, tag, std::move(msg));
        };
        const auto say = [&](const std::string& tag, std::string msg) {
            sayIn(linkFindings, tag, std::move(msg));
        };
        const auto sayCycle = [&](const std::string& tag, std::string msg) {
            sayIn(cycleFindings, tag, std::move(msg));
        };
        const auto sayDepth = [&](const std::string& tag, std::string msg) {
            sayIn(depthFindings, tag, std::move(msg));
        };
        // Integer adjacency, same shape the runtime resolves (tags::BuildAssocGraph).
        std::vector<std::vector<usize>> adj(defs.size());
        bool anyPresentDriver = false;
        for (usize t = 0; t < defs.size(); ++t) {
            const TagDef& d = defs[t];
            for (const std::string& a : d.associates) {
                const auto it = byName.find(a);
                if (it != byName.end() && it->second != t) adj[t].push_back(it->second);
            }
            if (!d.associates.empty() && present.count(d.name) > 0) anyPresentDriver = true;
        }

        for (usize t = 0; t < defs.size(); ++t) {
            const TagDef& d = defs[t];
            if (d.associates.empty() || present.count(d.name) == 0) continue;
            for (const std::string& a : d.associates) {
                if (a == d.name) {
                    say(d.name, "Tag '" + d.name +
                                    "' associates ITSELF. A tag cannot pull itself in; the "
                                    "entry does nothing and is dropped when the tag list is "
                                    "normalized.");
                    continue;
                }
                const auto it = byName.find(a);
                if (it == byName.end()) {
                    say(d.name, "Tag '" + d.name + "' associates '" + a +
                                    "', which this project does not list as a tag. The link "
                                    "is IGNORED at runtime - '" + d.name +
                                    "' still streams normally. Add the tag (Tags panel), or "
                                    "remove the association.");
                    continue;
                }
                const TagDef& td = defs[it->second];
                if (td.alwaysLoaded) {
                    say(d.name, "Tag '" + d.name + "' associates '" + a +
                                    "', which is ALWAYS LOADED. The association is a no-op: "
                                    "'" + a + "' is resident for the whole level anyway.");
                    continue;
                }
                if (present.count(a) == 0) {
                    say(d.name, "Tag '" + d.name + "' associates '" + a +
                                    "', but no object in this level carries '" + a +
                                    "' - so there is nothing here for it to pull in and the "
                                    "association does NOTHING in this level. (Associations "
                                    "do not reach across scene files.)");
                }
            }
            if (d.alwaysLoaded) {
                say(d.name, "Tag '" + d.name +
                                "' is ALWAYS LOADED and associates other tags. It therefore "
                                "drives them for the entire level, so everything it pulls in "
                                "(and everything those pull in) can NEVER unload. That is the "
                                "literal reading of the relation and it is honoured - but it "
                                "is rarely what an author means.");
            }
        }

        if (anyPresentDriver) {
            // CYCLES. Not a parse error and not refused: the propagation terminates by
            // construction (a visited set) and collapses as soon as no member is in
            // range on its own. But while any one of them IS in range they hold each
            // other resident, which the author has to be told in those words.
            std::vector<u8> colour(defs.size(), 0u); // 0 white, 1 grey, 2 black
            std::vector<i32> parent(defs.size(), -1);
            std::vector<std::pair<usize, usize>> stack;
            for (usize s = 0; s < defs.size() && cycleFindings < kMaxSameKind; ++s) {
                if (colour[s] != 0u) continue;
                colour[s] = 1u;
                stack.clear();
                stack.emplace_back(s, 0u);
                while (!stack.empty()) {
                    const usize u = stack.back().first;
                    if (stack.back().second < adj[u].size()) {
                        const usize v = adj[u][stack.back().second++];
                        if (colour[v] == 0u) {
                            colour[v] = 1u;
                            parent[v] = static_cast<i32>(u);
                            stack.emplace_back(v, 0u);
                        } else if (colour[v] == 1u) {
                            // Back edge u -> v: walk the parent chain back to v to
                            // NAME the cycle. A message that says "there is a cycle"
                            // without saying which tags is not actionable.
                            std::vector<usize> path{u};
                            for (usize w = u; w != v && parent[w] >= 0;) {
                                w = static_cast<usize>(parent[w]);
                                path.push_back(w);
                                if (path.size() > defs.size()) break;
                            }
                            std::string chain;
                            for (usize k = path.size(); k-- > 0;)
                                chain += defs[path[k]].name + " -> ";
                            chain += defs[v].name;
                            sayCycle(defs[v].name,
                                "Association CYCLE: " + chain +
                                    ". It terminates (each tag is visited once) and it "
                                    "collapses as soon as none of them is within its own "
                                    "unload radius - but while ANY one of them is in range, "
                                    "every tag in the cycle is held resident.");
                            // No break: the DFS is allowed to finish so no node is
                            // left grey, which would make the NEXT tree report a
                            // cycle that is not there.
                        }
                    } else {
                        colour[u] = 2u;
                        stack.pop_back();
                    }
                }
            }

            // DEPTH. Only kMaxAssocDepth hops are followed, so a longer chain has a
            // tail that silently never arrives. Breadth-first from each present
            // driver, one hop past the cap.
            std::vector<u8> seen(defs.size(), 0u);
            std::vector<usize> frontier, next;
            for (usize s = 0; s < defs.size() && depthFindings < kMaxSameKind; ++s) {
                if (defs[s].associates.empty() || present.count(defs[s].name) == 0) continue;
                std::fill(seen.begin(), seen.end(), 0u);
                seen[s] = 1u;
                frontier.assign(1, s);
                for (u32 depth = 0; depth <= stream::kMaxAssocDepth && !frontier.empty();
                     ++depth) {
                    next.clear();
                    for (const usize u : frontier)
                        for (const usize v : adj[u]) {
                            if (seen[v]) continue;
                            seen[v] = 1u;
                            next.push_back(v);
                        }
                    if (depth == stream::kMaxAssocDepth && !next.empty()) {
                        sayDepth(defs[s].name,
                            "Tag '" + defs[s].name + "' associates a chain more than " +
                                std::to_string(stream::kMaxAssocDepth) +
                                " hops deep (e.g. '" + defs[next[0]].name +
                                "'). Only the first " +
                                std::to_string(stream::kMaxAssocDepth) +
                                " hops are followed; anything beyond them is never pulled "
                                "in. Associate it directly if you meant it.");
                        break;
                    }
                    frontier.swap(next);
                }
            }
        }
    }

    // --- 6. The pin: after stamping, no parent link may cross a shard -----------
    // Unreachable by construction (whole subtrees are stamped together); it is here
    // so that any future change which breaks that is caught at bake time rather
    // than by a silently de-parented child on a player's machine. Skipped when the
    // hierarchy itself was malformed - that is already reported above, and a cycle
    // has no well-defined root to compare against.
    for (usize i = 0; badParents == 0 && cycles == 0 && i < n; ++i) {
        const i32 p = rows[i].parent;
        if (p < 0 || p >= static_cast<i32>(n)) continue;
        if (rows[i].shard == rows[static_cast<usize>(p)].shard &&
            atomTag[static_cast<usize>(rootOf[i])] ==
                atomTag[static_cast<usize>(rootOf[static_cast<usize>(p)])])
            continue;
        report.Add(Severity::Error, "",
                   "INTERNAL: '" + rows[i].name +
                       "' was baked into a different shard from its parent '" +
                       rows[static_cast<usize>(p)].name +
                       "'. It would load as a ROOT at its local transform. This is a bake "
                       "bug, not a content error.");
        break; // one line: the condition is systemic, not per-entity
    }
    return rep;
}

// --- Adapters -----------------------------------------------------------------
std::vector<BakeRow> Rows(const scene::SceneData& data) {
    const usize n = data.entities.size();
    std::vector<BakeRow> rows(n);
    // Local transform matrices first, then compose down the parent chain. Depth-
    // capped exactly like Scene::WorldMatrix (a malformed file must not hang a save).
    for (usize i = 0; i < n; ++i) {
        const scene::EntityData& d = data.entities[i];
        BakeRow& r = rows[i];
        r.parent = d.parent >= 0 && d.parent < static_cast<int>(n) ? d.parent : -1;
        r.name = d.name;
        r.tag = d.hasTag ? tags::Intern(d.tag) : kTagUntagged;
        r.uiDoc = false; // a `.hbui` document's entities are never in a `.hbscene`
        r.navInput = d.hasMesh && !d.meshSource.empty() &&
                     (!d.hasSceneLayerTag || d.sceneLayerKind == SceneKind::Static) &&
                     !(d.hasNavmeshInput && !d.navmeshInput.enabled);

        Box b;
        if (d.hasAABB) b.Add(d.aabb.min), b.Add(d.aabb.max);
        if (d.hasTrigger) AddHalf(b, d.trigger.halfExtents);
        if (d.hasCheckpoint) AddHalf(b, d.checkpoint.halfExtents);
        if (d.hasSpawner) AddHalf(b, d.spawner.halfExtents);
        if (d.hasCameraZone) AddHalf(b, d.cameraZone.halfExtents);
        if (d.hasMusicZone) AddHalf(b, d.musicZone.halfExtents);
        if (d.hasPostVolume) AddHalf(b, d.postVolume.halfExtents);
        if (d.hasProbe) AddHalf(b, d.probe.halfExtents);
        if (d.hasCensor) AddSphere(b, d.censor.offset, d.censor.radius);
        if (d.hasInteractable) AddSphere(b, glm::vec3(0.0f), d.interactable.range);
        if (d.hasPointLight) AddSphere(b, glm::vec3(0.0f), d.pointLight.range);
        if (d.hasSpotLight) AddSphere(b, glm::vec3(0.0f), d.spotLight.range);
        if (d.hasRectLight) AddSphere(b, glm::vec3(0.0f), d.rectLight.range);
        if (d.hasTerrain) {
            r.terrain = true;
            const f32 side = static_cast<f32>(d.terrain.chunks) * d.terrain.chunkSize;
            b.Add(glm::vec3(0.0f, -d.terrain.height, 0.0f));
            b.Add(glm::vec3(side, d.terrain.height, side));
        }
        r.hasExtent = b.valid;
        r.localMin = b.min;
        r.localMax = b.max;
    }
    // Compose world matrices (local first, then walk up). Cached per row.
    std::vector<glm::mat4> local(n, glm::mat4(1.0f));
    for (usize i = 0; i < n; ++i)
        if (data.entities[i].hasTransform) local[i] = data.entities[i].transform.Matrix();
    for (usize i = 0; i < n; ++i) {
        glm::mat4 m = local[i];
        i32 p = rows[i].parent;
        int guard = 0;
        while (p >= 0 && guard++ < 64) {
            m = local[static_cast<usize>(p)] * m;
            p = rows[static_cast<usize>(p)].parent;
        }
        rows[i].world = m;
    }
    return rows;
}

std::vector<BakeRow> Rows(const Scene& scene, const std::function<bool(entt::entity)>& include,
                          std::vector<entt::entity>& handlesOut) {
    const entt::registry& reg = scene.Registry();
    handlesOut.clear();
    // The set must be EXACTLY what the writer writes: shard member counts are
    // cross-checked against the file at load, so one extra entity here reports every
    // shard of that tag as corrupt.
    for (const entt::entity e : reg.view<entt::entity>()) {
        if (!reg.valid(e)) continue;
        if (!scene::IsSerializedEntity(reg, e)) continue;
        if (include && !include(e)) continue;
        handlesOut.push_back(e);
    }
    std::unordered_map<u32, i32> rowOf;
    rowOf.reserve(handlesOut.size());
    for (usize i = 0; i < handlesOut.size(); ++i)
        rowOf[static_cast<u32>(handlesOut[i])] = static_cast<i32>(i);

    std::vector<BakeRow> rows(handlesOut.size());
    for (usize i = 0; i < handlesOut.size(); ++i) {
        const entt::entity e = handlesOut[i];
        BakeRow& r = rows[i];
        // A parent outside the saved set is not a parent for sharding purposes: the
        // file will not carry the link either (EntityToJson drops a parent that is
        // not in `indexOf`), so the child is genuinely a root in that file.
        r.parent = -1;
        if (const Parent* p = reg.try_get<Parent>(e)) {
            const auto it = rowOf.find(static_cast<u32>(p->entity));
            if (it != rowOf.end()) r.parent = it->second;
        }
        if (const Name* nm = reg.try_get<Name>(e)) r.name = nm->value;
        if (const Tag* tg = reg.try_get<Tag>(e)) r.tag = tg->id;
        r.uiDoc = reg.all_of<UIDocMember>(e); // filtered out above; kept for honesty
        const SceneLayer* sl = reg.try_get<SceneLayer>(e);
        const NavmeshInput* ni = reg.try_get<NavmeshInput>(e);
        r.navInput = reg.all_of<MeshRef>(e) && (!sl || sl->kind == SceneKind::Static) &&
                     !(ni && !ni->enabled);
        r.world = scene.WorldMatrix(e);

        Box b;
        if (const AABB* a = reg.try_get<AABB>(e)) b.Add(a->min), b.Add(a->max);
        if (const TriggerVolume* c = reg.try_get<TriggerVolume>(e)) AddHalf(b, c->halfExtents);
        if (const Checkpoint* c = reg.try_get<Checkpoint>(e)) AddHalf(b, c->halfExtents);
        if (const Spawner* c = reg.try_get<Spawner>(e)) AddHalf(b, c->halfExtents);
        if (const CameraZone* c = reg.try_get<CameraZone>(e)) AddHalf(b, c->halfExtents);
        if (const MusicZone* c = reg.try_get<MusicZone>(e)) AddHalf(b, c->halfExtents);
        if (const PostVolume* c = reg.try_get<PostVolume>(e)) AddHalf(b, c->halfExtents);
        if (const ReflectionProbe* c = reg.try_get<ReflectionProbe>(e)) AddHalf(b, c->halfExtents);
        if (const CensorComponent* c = reg.try_get<CensorComponent>(e))
            AddSphere(b, c->offset, c->radius);
        if (const Interactable* c = reg.try_get<Interactable>(e))
            AddSphere(b, glm::vec3(0.0f), c->range);
        if (const PointLightComponent* c = reg.try_get<PointLightComponent>(e))
            AddSphere(b, glm::vec3(0.0f), c->range);
        if (const SpotLightComponent* c = reg.try_get<SpotLightComponent>(e))
            AddSphere(b, glm::vec3(0.0f), c->range);
        if (const RectLightComponent* c = reg.try_get<RectLightComponent>(e))
            AddSphere(b, glm::vec3(0.0f), c->range);
        if (const TerrainComponent* c = reg.try_get<TerrainComponent>(e)) {
            r.terrain = true;
            // Terrain is CENTRED on the entity origin (terrain::ExtentXZ is the ONE
            // definition - see TerrainSystem.h), so the local span is [-half, +half], not
            // [0, side]. The old box was offset by half the terrain: +256 m in X and Z on
            // the 16-chunk reference project, so the residency test compared the player
            // against a box a quarter of the map away.
            const f32 half = terrain::ExtentXZ(*c) * 0.5f;
            // And the vertical span came from `c->height`, which is the PROCEDURAL NOISE
            // AMPLITUDE and defaults to 0 - nothing to do with sculpted heights, so the
            // box was also vertically degenerate. Scan the real heightmap when it is sized.
            f32 minH = -c->height, maxH = c->height;
            const usize need = static_cast<usize>(c->GridN()) * c->GridN();
            if (c->heights.size() == need && need > 0) {
                minH = maxH = c->heights[0];
                for (const f32 h : c->heights) {
                    minH = glm::min(minH, h);
                    maxH = glm::max(maxH, h);
                }
            }
            if (maxH < minH) std::swap(minH, maxH);
            b.Add(glm::vec3(-half, minH, -half));
            b.Add(glm::vec3(half, maxH, half));
        }
        r.hasExtent = b.valid;
        r.localMin = b.min;
        r.localMax = b.max;
    }
    return rows;
}

BakeReport BakeScene(Scene& scene, const std::vector<TagDef>& defs,
                     const std::function<bool(entt::entity)>& include) {
    std::vector<entt::entity> handles;
    std::vector<BakeRow> rows = Rows(scene, include, handles);
    BakeReport rep = Bake(rows, defs);
    entt::registry& reg = scene.Registry();
    usize refused = 0;
    for (usize i = 0; i < rows.size(); ++i) {
        // BOTH HALVES, through the one mutation site. The bake resolved a tag per
        // SUBTREE (BakeRow::resolvedTag) and a shard index per row; writing only the
        // index leaves a promoted subtree's untagged members, and a descendant whose
        // own tag was overridden by its root's, with a file row that does not match
        // the header's member count - which makes tagshard::FromParsed distrust the
        // ENTIRE table and silently turn streaming off for the level.
        if (rows[i].resolvedTag != kTagUntagged) {
            if (!tags::Assign(reg, handles[i], rows[i].resolvedTag, rows[i].shard)) ++refused;
            continue;
        }
        // A resident subtree. Clear the baked index but NEVER remove the component:
        // the several-tags-under-one-untagged-root conflict is reported, not resolved
        // by deleting the author's tags (tags::Assign(kTagUntagged) would do exactly
        // that). Absence of a shard index is already "not streamed".
        if (Tag* tg = reg.try_get<Tag>(handles[i])) tg->shard = -1;
    }
    if (refused > 0) {
        // Unreachable through the editor save path: tags::Assign only refuses a
        // `.hbui` document entity, and scene::IsSerializedEntity already excluded
        // those from `handles`. Said out loud because a silent refusal here is
        // exactly the header/file mismatch this write-back exists to prevent.
        HBE_WARN("TagShard: {} entity(ies) refused a tag write-back; their shard rows will "
                 "not match the header and the level will not stream.", refused);
    }
    return rep;
}

ParsedShards FromParsed(const scene::SceneData& data) {
    ParsedShards out;
    out.shards = data.tagShards;
    out.members.assign(out.shards.size(), {});
    // (tag, shard) -> header row. std::map: the diagnostic text has to be stable.
    std::map<std::pair<std::string, u32>, usize> lookup;
    for (usize i = 0; i < out.shards.size(); ++i) {
        const auto key = std::make_pair(out.shards[i].tag, out.shards[i].index);
        if (lookup.contains(key)) {
            out.trusted = false;
            out.reason = "duplicate header row for shard '" + out.shards[i].tag + "#" +
                         std::to_string(out.shards[i].index) + "'";
            continue;
        }
        lookup.emplace(key, i);
    }
    usize orphans = 0;
    std::string firstOrphan;
    for (usize e = 0; e < data.entities.size(); ++e) {
        const scene::EntityData& d = data.entities[e];
        if (!d.hasTag || d.shard < 0) continue;
        const auto it = lookup.find({d.tag, static_cast<u32>(d.shard)});
        if (it == lookup.end()) {
            if (orphans++ == 0) firstOrphan = d.tag + "#" + std::to_string(d.shard);
            continue;
        }
        out.members[it->second].push_back(static_cast<u32>(e));
    }
    if (orphans > 0 && out.trusted) {
        out.trusted = false;
        out.reason = std::to_string(orphans) +
                     " entity(ies) reference a shard the header does not describe (e.g. '" +
                     firstOrphan + "')";
    }
    for (usize i = 0; i < out.shards.size() && out.trusted; ++i) {
        if (out.members[i].size() == out.shards[i].count) continue;
        out.trusted = false;
        out.reason = "shard '" + out.shards[i].tag + "#" +
                     std::to_string(out.shards[i].index) + "' says " +
                     std::to_string(out.shards[i].count) + " members but the file has " +
                     std::to_string(out.members[i].size());
    }
    if (!out.trusted) {
        HBE_WARN("TagShard: scene shard table is stale ({}); treating every shard as "
                 "always-loaded. Re-save the scene in the editor to re-bake it.",
                 out.reason);
    }
    return out;
}

// --- Self-test ----------------------------------------------------------------
namespace {

// A row with no bounds of its own: the bake gives it kMinHalfExtent.
BakeRow Marker(const char* name, const glm::vec3& pos, TagId tag) {
    BakeRow r;
    r.name = name;
    r.tag = tag;
    r.world = glm::translate(glm::mat4(1.0f), pos);
    return r;
}

// Sum of a shard's members' world boxes, computed the long way, so the shard's
// baked AABB can be checked against something that shares no code with it.
Box BruteForceBox(const std::vector<BakeRow>& rows, const std::vector<u32>& members) {
    Box b;
    for (const u32 m : members) {
        const BakeRow& r = rows[m];
        glm::vec3 lmin = r.localMin, lmax = r.localMax;
        if (!r.hasExtent) {
            lmin = glm::vec3(-kMinHalfExtent);
            lmax = glm::vec3(kMinHalfExtent);
        }
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner{(c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y,
                                   (c & 4) ? lmax.z : lmin.z};
            b.Add(glm::vec3(r.world * glm::vec4(corner, 1.0f)));
        }
    }
    return b;
}

std::string ShardFingerprint(const BakeReport& rep) {
    std::string s;
    for (const scene::ShardDesc& d : rep.shards) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s#%u[%.3f,%.3f,%.3f..%.3f,%.3f,%.3f]x%u;", d.tag.c_str(),
                      d.index, d.min.x, d.min.y, d.min.z, d.max.x, d.max.y, d.max.z, d.count);
        s += buf;
    }
    return s;
}

} // namespace

bool SelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("shardbake: FAILED - {}", what);
        }
    };

    // The table has to be seeded by hand: no project is open in a headless test.
    std::vector<TagDef> defs;
    {
        TagDef untagged;
        untagged.name = "Untagged";
        untagged.alwaysLoaded = true;
        defs.push_back(untagged);
        // Village and Props are the suite's SHARDING fixtures - autoShard is set
        // EXPLICITLY rather than inherited, because TagDef's default is now `false`
        // (one tag = one streaming unit) and a test that exercises the split path
        // must not silently stop exercising it when a default moves.
        TagDef village;
        village.name = "Village";
        village.loadRadius = 120.0f;
        village.unloadRadius = 160.0f;
        village.autoShard = true;
        defs.push_back(village);
        TagDef props;
        props.name = "Props";
        props.loadRadius = 120.0f;
        props.unloadRadius = 160.0f;
        props.autoShard = true;
        defs.push_back(props);
        TagDef terrainTag;
        terrainTag.name = "Ground";
        terrainTag.alwaysLoaded = true;
        defs.push_back(terrainTag);
        TagDef manual;
        manual.name = "Manual";
        manual.loadRadius = 120.0f;
        manual.unloadRadius = 160.0f;
        manual.autoShard = false;
        defs.push_back(manual);
        tags::Normalize(defs);
        tags::SeedFromProject(defs);
    }
    const TagId village = tags::Find("Village");
    const TagId props = tags::Find("Props");
    const TagId ground = tags::Find("Ground");
    const TagId manual = tags::Find("Manual");
    expect(village != kTagUntagged && props != kTagUntagged && ground != kTagUntagged &&
               manual != kTagUntagged,
           "the four test tags interned");

    // --- 1. A CLUSTERED tag becomes ONE shard ---------------------------------
    {
        std::vector<BakeRow> rows;
        for (int i = 0; i < 9; ++i)
            rows.push_back(Marker("Hut", glm::vec3(10.0f * (i % 3), 0.0f, 10.0f * (i / 3)),
                                  village));
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == 1, "nine huts 10 m apart bake to ONE shard");
        expect(rep.shards[0].count == 9, "the one shard owns all nine");
        expect(rep.errors == 0 && rep.warnings == 0, "a tidy cluster reports nothing");
        for (const BakeRow& r : rows) expect(r.shard == 0, "every member is in shard 0");
    }

    // --- 2. THE DEGENERATE CASE: a SCATTERED tag becomes MANY shards ----------
    // This is the whole reason the bake exists: a semantic tag ("Props") spread over
    // the level would otherwise be one level-sized group that always loads.
    {
        std::vector<BakeRow> rows;
        for (int x = 0; x < 12; ++x)
            for (int z = 0; z < 12; ++z)
                rows.push_back(Marker("Crate", glm::vec3(400.0f * x, 0.0f, 400.0f * z), props));
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == 144,
               "144 crates 400 m apart (cell 120) bake to 144 separate shards");
        u32 total = 0;
        f32 worst = 0.0f;
        for (const scene::ShardDesc& d : rep.shards) {
            total += d.count;
            worst = std::max(worst, glm::length(d.max - d.min));
        }
        expect(total == 144, "every crate landed in exactly one shard");
        expect(worst < kSpreadRadiiWarn * 120.0f,
               "no shard of the scattered tag is wider than 4 load radii");
        // And the counter-proof: the SAME objects as one un-sharded group would be
        // level-sized, which is what the validator has to shout about.
        std::vector<TagDef> off = defs;
        off[props].autoShard = false;
        std::vector<BakeRow> rows2 = rows;
        const BakeReport rep2 = Bake(rows2, off);
        expect(rep2.shards.size() == 1, "with auto-sharding OFF the same tag is ONE shard");
        expect(rep2.warnings > 0, "and that one level-sized shard is REPORTED as spread");
        bool spreadLine = false;
        for (const Diagnostic& d : rep2.diagnostics)
            if (d.message.find("effectively ALWAYS LOADED") != std::string::npos) spreadLine = true;
        expect(spreadLine, "the spread warning names the actual consequence");
    }

    // --- 3. DETERMINISM under shuffled input ---------------------------------
    // Shard indices are geometric, never row-order derived, so the same content
    // must bake to the same bytes no matter what order the rows arrive in.
    {
        std::vector<BakeRow> a;
        for (int i = 0; i < 20; ++i) { // 20 subtrees on a 5x4 grid, 400 m apart
            const f32 gx = 400.0f * static_cast<f32>(i % 5);
            const f32 gz = 400.0f * static_cast<f32>(i / 5);
            a.push_back(Marker("Root", glm::vec3(gx, 0.0f, gz), props));
            // BakeRow::world is a WORLD matrix (the adapters compose the Parent
            // chain), so the child's position is absolute, not relative.
            BakeRow child = Marker("Child", glm::vec3(gx + 3.0f, 0.0f, gz), props);
            child.parent = static_cast<i32>(a.size()) - 1;
            a.push_back(child);
        }
        std::vector<BakeRow> b = a;
        // A fixed, non-trivial permutation (no RNG - the test must be reproducible).
        std::vector<u32> perm(b.size());
        for (u32 i = 0; i < perm.size(); ++i) perm[i] = (i * 37u + 11u) % static_cast<u32>(b.size());
        {
            std::vector<BakeRow> shuffled(b.size());
            std::vector<i32> newIndexOf(b.size(), -1);
            for (u32 i = 0; i < perm.size(); ++i) newIndexOf[perm[i]] = static_cast<i32>(i);
            for (u32 i = 0; i < perm.size(); ++i) shuffled[newIndexOf[i]] = b[i];
            for (BakeRow& r : shuffled)
                if (r.parent >= 0) r.parent = newIndexOf[static_cast<usize>(r.parent)];
            b = std::move(shuffled);
        }
        const BakeReport ra = Bake(a, defs);
        const BakeReport rb = Bake(b, defs);
        expect(ra.shards.size() == 20 && rb.shards.size() == 20,
               "20 two-entity subtrees at 400 m spacing bake to 20 shards");
        expect(ShardFingerprint(ra) == ShardFingerprint(rb),
               "the shard table is BYTE-STABLE under a shuffled row order");
        // Each child rode its root, in both orderings.
        for (usize i = 0; i < a.size(); ++i)
            if (a[i].parent >= 0)
                expect(a[i].shard == a[static_cast<usize>(a[i].parent)].shard,
                       "a child is always in its parent's shard");
    }

    // --- 4. Bounds actually CONTAIN their members, volumes included -----------
    {
        std::vector<BakeRow> rows;
        rows.push_back(Marker("Hut", glm::vec3(0.0f), village));
        // A MESHLESS 40 m trigger volume: the mis-bounding case. Its extent comes
        // from the component, not from a mesh, and the shard must grow to hold it.
        BakeRow trig = Marker("BigTrigger", glm::vec3(30.0f, 0.0f, 0.0f), village);
        trig.hasExtent = true;
        trig.localMin = glm::vec3(-40.0f, -5.0f, -40.0f);
        trig.localMax = glm::vec3(40.0f, 5.0f, 40.0f);
        rows.push_back(trig);
        // A scaled + rotated child, so the 8-corner transform is exercised.
        BakeRow spun = Marker("Spun", glm::vec3(5.0f, 2.0f, 5.0f), village);
        spun.hasExtent = true;
        spun.localMin = glm::vec3(-1.0f, -8.0f, -1.0f);
        spun.localMax = glm::vec3(1.0f, 8.0f, 1.0f);
        spun.world = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 2.0f, 5.0f)) *
                     glm::rotate(glm::mat4(1.0f), 0.9f, glm::vec3(0.0f, 0.0f, 1.0f)) *
                     glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
        spun.parent = 0;
        rows.push_back(spun);
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == 1 && rep.members.size() == 1, "one shard, one member list");
        const Box brute = BruteForceBox(rows, rep.members[0]);
        const scene::ShardDesc& d = rep.shards[0];
        expect(brute.valid && d.min.x <= brute.min.x && d.min.y <= brute.min.y &&
                   d.min.z <= brute.min.z && d.max.x >= brute.max.x && d.max.y >= brute.max.y &&
                   d.max.z >= brute.max.z,
               "the shard AABB contains a brute-force union of its members");
        expect(d.max.x - brute.max.x >= kBoundsPad * 0.999f,
               "and it is padded, not merely tight");
        expect(d.max.x >= 69.0f, "the meshless 40 m trigger widened the shard (not a 2 m point)");
        expect(DistanceToShard(d, glm::vec3(0.0f, 0.0f, 0.0f)) == 0.0f,
               "distance to a shard is 0 inside it");
        // Distance is to the BOX, not the centre: standing against a long shard's
        // edge is 0 m away even though its centre is far.
        const f32 edge = DistanceToShard(d, glm::vec3(d.max.x + 10.0f, 0.0f, 0.0f));
        expect(edge > 9.9f && edge < 10.1f, "distance is measured to the AABB, not the centre");
    }

    // --- 5. Cross-shard parents are reported AT BAKE TIME --------------------
    {
        std::vector<BakeRow> rows;
        rows.push_back(Marker("Camp", glm::vec3(0.0f), village)); // 0: root, Village
        BakeRow child = Marker("Tent", glm::vec3(2.0f, 0.0f, 0.0f), props); // wrong tag
        child.parent = 0;
        rows.push_back(child);
        const BakeReport rep = Bake(rows, defs);
        expect(rep.errors >= 1, "a descendant tagged differently from its root is an ERROR");
        bool named = false;
        for (const Diagnostic& d : rep.diagnostics)
            if (d.severity == Severity::Error &&
                d.message.find("CROSS-SHARD PARENT") != std::string::npos &&
                d.message.find("Tent") != std::string::npos)
                named = true;
        expect(named, "the report names the offending entity and calls it a cross-shard parent");
        expect(rows[1].shard == rows[0].shard && rows[1].shard >= 0,
               "and the bake RESOLVES it: the child rides its root's shard, never a "
               "cross-slice link");
        expect(rep.shards.size() == 1 && rep.shards[0].tag == "Village" &&
                   rep.shards[0].count == 2,
               "both entities are in the root's Village shard");
    }

    // --- 6. A tag set on a CHILD is promoted to the whole subtree ------------
    // The normal Inspector path: the author selects a child and sets a tag. Ignoring
    // it would silently undo their click; splitting the subtree is impossible.
    {
        std::vector<BakeRow> rows;
        rows.push_back(Marker("Prop", glm::vec3(0.0f), kTagUntagged));
        BakeRow child = Marker("Lantern", glm::vec3(1.0f, 0.0f, 0.0f), village);
        child.parent = 0;
        rows.push_back(child);
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == 1 && rep.shards[0].count == 2,
               "the untagged root joins the tag its child carries");
        expect(rows[0].shard == 0 && rows[1].shard == 0, "both are in the promoted shard");
        expect(rep.warnings >= 1 && rep.errors == 0, "promotion is a warning, not an error");
        // Two conflicting tags under one untagged root cannot be resolved: it stays
        // resident, and says so.
        std::vector<BakeRow> rows2;
        rows2.push_back(Marker("Prop", glm::vec3(0.0f), kTagUntagged));
        BakeRow c1 = Marker("A", glm::vec3(1.0f, 0.0f, 0.0f), village);
        c1.parent = 0;
        rows2.push_back(c1);
        BakeRow c2 = Marker("B", glm::vec3(2.0f, 0.0f, 0.0f), props);
        c2.parent = 0;
        rows2.push_back(c2);
        const BakeReport rep2 = Bake(rows2, defs);
        expect(rep2.shards.empty() && rep2.errors >= 1,
               "conflicting tags under one untagged root: no shard, and an error");
        for (const BakeRow& r : rows2) expect(r.shard == -1, "the conflicted subtree stays resident");
    }

    // --- 7. alwaysLoaded / autoShard=false collapse to one shard -------------
    {
        std::vector<BakeRow> rows;
        for (int i = 0; i < 6; ++i)
            rows.push_back(Marker("Slab", glm::vec3(2000.0f * i, 0.0f, 0.0f), ground));
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == 1, "an alwaysLoaded tag is never split, however spread");
        bool spreadWarn = false;
        for (const Diagnostic& d : rep.diagnostics)
            if (d.message.find("effectively ALWAYS LOADED") != std::string::npos) spreadWarn = true;
        expect(!spreadWarn, "and it is not warned about for being spread - it IS always loaded");

        std::vector<BakeRow> rows2;
        for (int i = 0; i < 6; ++i)
            rows2.push_back(Marker("Hand", glm::vec3(2000.0f * i, 0.0f, 0.0f), manual));
        const BakeReport rep2 = Bake(rows2, defs);
        expect(rep2.shards.size() == 1, "autoShard=false means the author splits by hand");
    }

    // --- 8. The per-tag shard CAP merges instead of exploding ----------------
    {
        std::vector<BakeRow> rows;
        for (int x = 0; x < 20; ++x)
            for (int z = 0; z < 20; ++z)
                rows.push_back(Marker("Leaf", glm::vec3(400.0f * x, 0.0f, 400.0f * z), props));
        const BakeReport rep = Bake(rows, defs);
        expect(rep.shards.size() == kMaxShardsPerTag,
               "400 scattered roots are merged down to the per-tag shard cap");
        u32 total = 0;
        for (const scene::ShardDesc& d : rep.shards) total += d.count;
        expect(total == 400, "the cap MERGES - it never drops a member");
        u32 withShard = 0;
        for (const BakeRow& r : rows)
            if (r.shard >= 0) ++withShard;
        expect(withShard == 400, "every entity still has a shard after merging");
        bool capWarn = false;
        for (const Diagnostic& d : rep.diagnostics)
            if (d.message.find("merged down to") != std::string::npos) capWarn = true;
        expect(capWarn, "the merge is reported");

        // The merge picks "smallest, ties to lowest index", so it is only
        // deterministic because the groups are put in geometric order FIRST. Prove
        // it: the same 400 roots in a different order must merge identically.
        std::vector<BakeRow> shuffled(rows.size());
        for (usize i = 0; i < rows.size(); ++i)
            shuffled[(i * 137u + 29u) % rows.size()] = rows[i];
        const BakeReport rep2 = Bake(shuffled, defs);
        expect(ShardFingerprint(rep) == ShardFingerprint(rep2),
               "the CAP MERGE is deterministic under a shuffled row order too");
    }

    // --- 9. Heavy overlap is detected --------------------------------------
    // Tested directly, because the clustering deliberately makes it hard to produce
    // (touching cells always merge) - see HeavyOverlaps.
    {
        std::vector<scene::ShardDesc> s(3);
        s[0].tag = s[1].tag = s[2].tag = "Props";
        s[0].index = 0;
        s[0].min = glm::vec3(0.0f);
        s[0].max = glm::vec3(100.0f);
        s[1].index = 1; // 90% inside s[0]
        s[1].min = glm::vec3(10.0f);
        s[1].max = glm::vec3(60.0f);
        s[2].index = 2; // disjoint
        s[2].min = glm::vec3(500.0f);
        s[2].max = glm::vec3(560.0f);
        const auto pairs = HeavyOverlaps(s);
        expect(pairs.size() == 1 && pairs[0].first == 0 && pairs[0].second == 1,
               "a shard mostly contained in another is reported, a disjoint one is not");
    }

    // --- 10. A scene-only tag (not in the project list) still shards ---------
    {
        const TagId ghost = tags::Intern("SceneOnlyTag");
        std::vector<BakeRow> rows;
        rows.push_back(Marker("A", glm::vec3(0.0f), ghost));
        rows.push_back(Marker("B", glm::vec3(2000.0f, 0.0f, 0.0f), ghost));
        const BakeReport rep = Bake(rows, defs); // `defs` does not list it
        // ONE shard, not two: an unlisted tag gets TagDef's defaults, and the default
        // is now autoShard=false, so its two 2 km-apart members share one combined box.
        // What this test is actually for is that the tag is BAKED AT ALL rather than
        // dropped for being absent from the project list.
        expect(rep.shards.size() == 1 && rep.shards[0].tag == "SceneOnlyTag",
               "a tag the project does not list is baked with defaults, never dropped");
    }

    // --- 11. End to end: the file header, and the runtime cross-check --------
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_shardbake";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        Scene s;
        auto& reg = s.Registry();
        const auto make = [&](const char* n, const glm::vec3& p, TagId t) {
            const entt::entity e = s.CreateEntity(n);
            Transform tr;
            tr.position = p;
            reg.emplace<Transform>(e, tr);
            if (t != kTagUntagged) tags::Assign(reg, e, t);
            return e;
        };
        // Two clusters 3 km apart, one of them a two-entity subtree, plus a meshless
        // 30 m trigger and an untagged bystander.
        const entt::entity camp = make("Camp", glm::vec3(0.0f), village);
        const entt::entity tent = make("Tent", glm::vec3(4.0f, 0.0f, 0.0f), village);
        reg.emplace<Parent>(tent, Parent{camp});
        TriggerVolume tv;
        tv.halfExtents = glm::vec3(30.0f, 4.0f, 30.0f);
        reg.emplace<TriggerVolume>(make("Wire", glm::vec3(8.0f, 0.0f, 0.0f), village), tv);
        make("Mill", glm::vec3(3000.0f, 0.0f, 0.0f), village);
        make("Rock", glm::vec3(12.0f, 0.0f, 0.0f), kTagUntagged);

        BakeReport rep = BakeScene(s, defs);
        expect(rep.shards.size() == 2, "the live-registry bake finds two Village shards");
        expect(reg.get<Tag>(camp).shard == reg.get<Tag>(tent).shard,
               "BakeScene stamped the subtree with ONE shard index");
        expect(!reg.all_of<Tag>(s.FindByName("Rock")),
               "an untagged entity is not given a Tag by the bake");

        const fs::path file = dir / "Shards.hbscene";
        expect(scene::SaveScene(s, file, {}, SceneKind::Full, &rep.shards),
               "save the baked scene with its shard header");

        scene::SceneData data;
        expect(scene::ParseSceneFile(file, data), "the baked scene parses");
        expect(data.tagShards.size() == 2, "the \"tagShards\" header round-trips");
        expect(data.tagShards[0].tag == "Village" && data.tagShards[0].count > 0,
               "and carries the tag NAME plus a member count");
        expect(data.tagShards[0].min.x == rep.shards[0].min.x &&
                   data.tagShards[1].max.z == rep.shards[1].max.z,
               "the baked bounds survive the JSON round trip exactly");

        // The runtime side: bucket the file's entities and cross-check.
        ParsedShards ps = FromParsed(data);
        expect(ps.trusted, "a freshly baked file's shard table is TRUSTED");
        expect(ps.members.size() == 2 &&
                   ps.members[0].size() + ps.members[1].size() == data.tagShards[0].count +
                                                                     data.tagShards[1].count,
               "every shard's members are found in the file");
        // A stale header must degrade to always-loaded, never to missing content.
        scene::SceneData stale = data;
        stale.tagShards[0].count += 1;
        expect(!FromParsed(stale).trusted, "a member-count mismatch makes the table UNTRUSTED");
        scene::SceneData orphan = data;
        orphan.tagShards.clear();
        expect(!FromParsed(orphan).trusted,
               "entities referencing shards no header describes are UNTRUSTED");
        expect(FromParsed(scene::SceneData{}).trusted,
               "an unbaked scene is trusted-and-empty (absence means everything resident)");

        // THE TWO ADAPTERS MUST AGREE. Same content, one read from the live registry
        // and one from the parsed file (whose entity order is different), must produce
        // the same shard table - otherwise an editor bake and any SceneData-side bake
        // could disagree, which is the divergence the "bake result IS the file" rule
        // exists to prevent.
        std::vector<BakeRow> fileRows = Rows(data);
        const BakeReport fromFile = Bake(fileRows, defs);
        expect(ShardFingerprint(fromFile) == ShardFingerprint(rep),
               "the SceneData bake and the registry bake produce identical shards");
    }

    // --- 12. THE RESOLVED TAG IS WRITTEN BACK, so a promoted or overridden subtree
    // produces a TRUSTED file. The bake counts every member of a subtree, tagged or
    // not; if only `Tag::shard` is written back, those rows carry no tag in the file,
    // FromParsed finds fewer members than the header claims, and the ENTIRE level
    // stops streaming with a "re-save the scene" warning on a file that was just
    // saved. This is the exact shape the Inspector produces (tags::AssignSubtree runs
    // on the SELECTED entity, routinely a child), so it is not an exotic input.
    {
        Scene s;
        auto& reg = s.Registry();
        const auto make = [&](const char* n, const glm::vec3& p, TagId t) {
            const entt::entity e = s.CreateEntity(n);
            Transform tr;
            tr.position = p;
            reg.emplace<Transform>(e, tr);
            if (t != kTagUntagged) tags::Assign(reg, e, t);
            return e;
        };
        // (a) PROMOTE: the author tagged the child; the untagged root joins the group.
        const entt::entity root = make("Camp", glm::vec3(0.0f), kTagUntagged);
        const entt::entity tent = make("Tent", glm::vec3(3.0f, 0.0f, 0.0f), village);
        reg.emplace<Parent>(tent, Parent{root});
        // (b) OVERRIDE: a descendant tagged differently from its tagged root rides the
        // root's shard, so the file must say the ROOT's tag on it - not an index into
        // another tag's shard space.
        const entt::entity mill = make("Mill", glm::vec3(3000.0f, 0.0f, 0.0f), village);
        const entt::entity barrel = make("Barrel", glm::vec3(3002.0f, 0.0f, 0.0f), props);
        reg.emplace<Parent>(barrel, Parent{mill});

        BakeReport rep = BakeScene(s, defs);
        expect(reg.all_of<Tag>(root) && reg.get<Tag>(root).id == village,
               "the promoted subtree's untagged ROOT is given the tag it was promoted to");
        expect(reg.get<Tag>(root).shard == reg.get<Tag>(tent).shard,
               "and shares one shard index with the child that carried the author's click");
        expect(reg.get<Tag>(barrel).id == village &&
                   reg.get<Tag>(barrel).shard == reg.get<Tag>(mill).shard,
               "an overridden descendant is rewritten to its ROOT's tag and shard");

        const fs::path file2 = dir / "Promoted.hbscene";
        expect(scene::SaveScene(s, file2, {}, SceneKind::Full, &rep.shards),
               "save the promoted/overridden bake");
        scene::SceneData data2;
        expect(scene::ParseSceneFile(file2, data2), "it parses");
        const ParsedShards ps2 = FromParsed(data2);
        expect(ps2.trusted,
               "THE REGRESSION PIN: a promoted/overridden subtree bakes to a TRUSTED table "
               "(writing only Tag::shard made every shard in the level untrusted)");
        usize found = 0;
        for (const std::vector<u32>& m : ps2.members) found += m.size();
        usize claimed = 0;
        for (const scene::ShardDesc& d : ps2.shards) claimed += d.count;
        expect(found == claimed && found == 4,
               "and all four subtree members are accounted for in the file");
    }
    fs::remove_all(dir, ec);

    if (ok)
        HBE_INFO("shardbake: sharding is deterministic and geometric, a scattered tag "
                 "splits (and an unsharded one is reported), every entity lands in exactly "
                 "one shard, subtrees ride their root, cross-shard parents are reported and "
                 "resolved at bake time, shard AABBs contain their members including "
                 "meshless volumes, the cap merges without loss, and the file header "
                 "round-trips + cross-checks.");
    return ok;
}

} // namespace hbe::tagshard

// Scene/StrokeZone.cpp - see StrokeZone.h for the rule this file implements.
#include "Scene/StrokeZone.h"

#include "Core/Log.h"
#include "Project/Project.h" // TagDef (autoShard / shardCell) for per-cell groups
#include "Scene/Hierarchy.h"
#include "Scene/Scene.h"
#include "Scene/TagShard.h" // kMinShardCell - the same cell the bake uses
#include "Scene/TagTable.h"

// --test-strokezones only, below the implementation.
#include "Assets/MaterialAsset.h"
#include "Assets/MeshGenerator.h" // a real ribbon `.uaf` for the drag path's asset shape
#include "Assets/UAF.h"
#include "Renderer/Renderer.h"
#include "Scene/SceneSerializer.h"
#include "Scene/TagStreaming.h"
#include "Scene/WorldState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace hbe::strokezone {
namespace {

// A stroke group node is an EMPTY node. The legacy adoption below tests this so a
// mesh the artist happened to name "Paint Strokes" is never turned into a group.
bool LooksLikeGroupNode(const entt::registry& reg, entt::entity e) {
    return !reg.all_of<MeshInstance>(e) && !reg.all_of<MeshRef>(e);
}

// A node from a scene authored BEFORE zones existed: the literal name, no mesh, and -
// critically - no StrokeGroup and no Tag.
//
// Both exclusions are load-bearing. Without the StrokeGroup test, a group the artist
// has RENAMED to "Paint Strokes" (which the header promises is safe, since groups are
// found by component) is re-stamped as the untagged group while KEEPING its Tag - so
// the untagged group ends up inside a streaming atom and paint on always-resident
// terrain despawns with a zone. Without the Tag test, the same happens to any tagged
// empty node that happens to carry the name.
bool IsLegacyGroupNode(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return false;
    if (reg.all_of<StrokeGroup>(e) || reg.all_of<Tag>(e)) return false;
    if (reg.all_of<UIDocMember>(e)) return false; // `.hbui` content is not world content
    const Name* n = reg.try_get<const Name>(e);
    return n && n->value == kUntaggedGroupName && LooksLikeGroupNode(reg, e);
}

const std::string& SceneSrcOf(const entt::registry& reg, entt::entity e) {
    static const std::string kNone;
    const SceneSource* s = reg.valid(e) ? reg.try_get<const SceneSource>(e) : nullptr;
    return s ? s->scene : kNone;
}

// The shard grid a zone's groups are split on, or 0 when the zone is one atom.
// Mirrors tagshard::BakeScene's own cell derivation exactly (TagShard.cpp step 5), so
// a group node can never straddle a cell boundary the bake would have split on.
// Returns 0 - meaning "one group for the whole zone", the pre-existing behaviour -
// for the untagged zone, for a tag with autoShard off, and whenever there is no
// active project to read the tag list from (every headless self-test).
f32 ShardCellFor(TagId zone) {
    if (zone == kTagUntagged || !Project::HasActive()) return 0.0f;
    const std::string& name = tags::Name(zone);
    if (name.empty()) return 0.0f;
    for (const TagDef& d : Project::Active().Settings().tags) {
        if (d.name != name) continue;
        // alwaysLoaded is never distance-tested, so the bake never shards it either.
        if (!d.autoShard || d.alwaysLoaded) return 0.0f;
        return d.shardCell > 0.0f ? d.shardCell
                                  : std::max(d.loadRadius, tagshard::kMinShardCell);
    }
    return 0.0f;
}

std::pair<i32, i32> CellOf(const glm::vec3& p, f32 cell) {
    if (cell <= 0.0f) return {0, 0};
    return {static_cast<i32>(std::floor(p.x / cell)), static_cast<i32>(std::floor(p.z / cell))};
}

struct Box {
    glm::vec3 mn{std::numeric_limits<f32>::max()};
    glm::vec3 mx{std::numeric_limits<f32>::lowest()};
    bool valid = false;
    void Add(const glm::vec3& p) {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
        valid = true;
    }
    // `slack` expands the box on every axis. A stroke is never ON its surface (both
    // creation sites lift it along the hit normal), so exact containment reports paint
    // on a zone's own boundary wall as belonging to NO zone - see kStrokeLiftSlack.
    bool Contains(const glm::vec3& p, f32 slack = 0.0f) const {
        return valid && p.x >= mn.x - slack && p.x <= mx.x + slack && p.y >= mn.y - slack &&
               p.y <= mx.y + slack && p.z >= mn.z - slack && p.z <= mx.z + slack;
    }
    f32 Volume() const {
        if (!valid) return 0.0f;
        const glm::vec3 d = glm::max(mx - mn, glm::vec3(0.0f));
        return d.x * d.y * d.z;
    }
};

// World-space bounds of one entity: its local AABB's eight corners through its
// world matrix, or a point at its world position when it has no AABB (a volume /
// marker entity still says something about where its zone is).
void AddWorldBounds(const Scene& scene, entt::entity e, Box& box) {
    const entt::registry& reg = scene.Registry();
    const glm::mat4 w = scene.WorldMatrix(e);
    const AABB* bb = reg.try_get<const AABB>(e);
    if (!bb) {
        box.Add(glm::vec3(w[3]));
        return;
    }
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 p((c & 1) ? bb->max.x : bb->min.x, (c & 2) ? bb->max.y : bb->min.y,
                          (c & 4) ? bb->max.z : bb->min.z);
        box.Add(glm::vec3(w * glm::vec4(p, 1.0f)));
    }
}

} // namespace

std::string GroupName(TagId zone) {
    if (zone == kTagUntagged) return kUntaggedGroupName;
    const std::string& n = tags::Name(zone);
    // An id the live table cannot name should not silently collapse onto the
    // untagged group's NAME (the component still says which zone it is).
    return n.empty() ? std::string(kUntaggedGroupName) + " - ?"
                     : std::string(kUntaggedGroupName) + " - " + n;
}

TagId ZoneOfSurface(const entt::registry& reg, entt::entity hit) {
    // Depth-capped for the same reason Scene::WorldMatrix is: a Parent cycle is
    // reachable from a hand-edited `.hbscene`.
    entt::entity e = hit;
    for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
        if (const Tag* t = reg.try_get<const Tag>(e); t && t->id != kTagUntagged) return t->id;
        const Parent* p = reg.try_get<const Parent>(e);
        if (!p) break;
        e = p->entity;
    }
    return kTagUntagged;
}

TagId ZoneOfPosition(const Scene& scene, const glm::vec3& worldPos, f32 slack) {
    const entt::registry& reg = scene.Registry();
    std::vector<Box> byTag;
    for (const entt::entity e : reg.view<const Tag>()) {
        const TagId id = reg.get<const Tag>(e).id;
        if (id == kTagUntagged) continue;
        // CIRCULARITY GUARD: the thing being re-homed must not define the answer.
        // A zone's own stroke group (and its strokes) are already inside that zone
        // geometrically, so including them only ever widens a box - but a stroke
        // dragged into the wrong zone would then keep the wrong zone alive.
        if (reg.all_of<StrokeGroup>(e) || IsStroke(reg, e)) continue;
        if (byTag.size() <= id) byTag.resize(static_cast<usize>(id) + 1);
        AddWorldBounds(scene, e, byTag[id]);
    }
    TagId best = kTagUntagged;
    f32 bestVol = std::numeric_limits<f32>::max();
    for (usize i = 1; i < byTag.size(); ++i) {
        if (!byTag[i].Contains(worldPos, slack)) continue;
        // Smallest containing box wins, so a room nested inside a district resolves
        // to the room. Ties break on the LOWER tag id, which is the authored order
        // in the project's tag list - deterministic, not registry-order dependent.
        const f32 v = byTag[i].Volume();
        if (v < bestVol) {
            bestVol = v;
            best = static_cast<TagId>(i);
        }
    }
    return best;
}

TagId GroupZone(const entt::registry& reg, entt::entity group) {
    // DERIVED, never a stored copy - see StrokeZone.h. Absence of Tag IS Untagged
    // (tags::Assign removes the component for id 0), so this is total.
    if (!reg.valid(group)) return kTagUntagged;
    const Tag* t = reg.try_get<const Tag>(group);
    return t ? t->id : kTagUntagged;
}

bool IsGroupNode(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return false;
    return reg.all_of<StrokeGroup>(e) || IsLegacyGroupNode(reg, e);
}

entt::entity FindGroup(const entt::registry& reg, TagId zone) {
    for (const entt::entity e : reg.view<const StrokeGroup>())
        if (GroupZone(reg, e) == zone) return e;
    return entt::null;
}

namespace {

// The group for (zone, scene file, shard cell), or null. `cell` <= 0 means the zone
// is one atom and the cell test is skipped.
entt::entity FindGroupFor(const Scene& scene, TagId zone, const std::string& sceneSrc,
                          const glm::vec3& anchor, f32 cell) {
    const entt::registry& reg = scene.Registry();
    const std::pair<i32, i32> want = CellOf(anchor, cell);
    for (const entt::entity e : reg.view<const StrokeGroup>()) {
        if (GroupZone(reg, e) != zone) continue;
        if (SceneSrcOf(reg, e) != sceneSrc) continue;
        if (cell > 0.0f && CellOf(glm::vec3(scene.WorldMatrix(e)[3]), cell) != want) continue;
        return e;
    }
    return entt::null;
}

} // namespace

entt::entity EnsureGroup(Scene& scene, TagId zone, const glm::vec3& anchor,
                         const std::string& sceneSrc) {
    entt::registry& reg = scene.Registry();
    const f32 cell = ShardCellFor(zone);
    if (const entt::entity found = FindGroupFor(scene, zone, sceneSrc, anchor, cell);
        found != entt::null)
        return found;

    // LEGACY ADOPTION. Before zones there was exactly one group, found by scanning
    // for the literal name. Stamping StrokeGroup on it (rather than creating a
    // second node beside it) is what makes a scene authored before this file keep
    // collecting its untagged strokes where the artist already sees them - and it
    // is why no migration flag is needed. Only the untagged zone can adopt: a
    // legacy node is by definition untagged.
    //
    // IsLegacyGroupNode - not a bare name match - is what stops this from hijacking a
    // RENAMED zone group (which still carries StrokeGroup and its Tag) or a `.hbui`
    // element that happens to share the name. And it must be in the SAME scene file,
    // or the adoption re-creates the cross-file parent link it is meant to avoid.
    if (zone == kTagUntagged) {
        for (const entt::entity e : reg.view<const Name>()) {
            if (!IsLegacyGroupNode(reg, e)) continue;
            if (SceneSrcOf(reg, e) != sceneSrc) continue;
            reg.emplace_or_replace<StrokeGroup>(e, StrokeGroup{});
            if (!reg.all_of<Transform>(e)) reg.emplace<Transform>(e, Transform{});
            return e;
        }
    }

    const entt::entity g = scene.CreateEntity(GroupName(zone));
    Transform t;
    // Translation only, identity basis - Attach's world->local conversion is exact
    // for a pure translation, and the anchor keeps the node (and therefore the
    // shard AABB the bake unions) inside its own zone instead of at the origin.
    t.position = anchor;
    reg.emplace<Transform>(g, t);
    reg.emplace<StrokeGroup>(g, StrokeGroup{});
    // The group belongs to the same FILE as the content that asked for it, so Ctrl+S
    // writes the group and its strokes together. (Editor::Reparent keeps the same
    // invariant with MoveToScene; this is the paint path's version of it.)
    if (!sceneSrc.empty()) reg.emplace_or_replace<SceneSource>(g, SceneSource{sceneSrc});
    // THE GROUP CARRIES THE TAG - and it is the group's ZONE, not a copy of it.
    // Assign is the one mutation site and it REMOVES the component for kTagUntagged,
    // so the untagged group correctly ends up with no Tag at all.
    tags::Assign(reg, g, zone);
    return g;
}

bool Attach(Scene& scene, entt::entity stroke, TagId zone) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(stroke)) return false;
    Transform* st = reg.try_get<Transform>(stroke);
    if (!st) return false;

    // The stroke is created as a ROOT with a world-space transform, so its current
    // position IS the anchor a freshly created group wants. The group is looked up
    // for the STROKE's own scene file: an additively streamed `.hbscene` contributes
    // its own groups, and parenting across that boundary is the silent-teleport bug
    // Editor::Reparent guards against with MoveToScene.
    const glm::vec3 worldPos = st->position;
    const entt::entity group = EnsureGroup(scene, zone, worldPos, SceneSrcOf(reg, stroke));
    if (group == entt::null || group == stroke) return false;

    reg.emplace_or_replace<Parent>(stroke, Parent{group});
    // World -> group-local, AFTER the parent link so WorldMatrix(group) is read from
    // the group's own chain (a group node is a root today, but adoption could hand
    // back one an artist parented somewhere). Position only: a group node is created
    // with an identity basis, and an artist who rotates one afterwards means to
    // rotate its strokes with it, which is what a group is for.
    const glm::mat4 inv = glm::inverse(scene.WorldMatrix(group));
    reg.get<Transform>(stroke).position = glm::vec3(inv * glm::vec4(worldPos, 1.0f));

    // The stroke gets the SAME tag as its group. Not required by the bake (the
    // atom's tag comes from its root) but it makes the two agree instead of the
    // bake having to promote, and it keeps the stroke's zone legible if it is ever
    // unparented. Untagged removes the component, so nothing is written.
    tags::Assign(reg, stroke, zone);
    return true;
}

bool IsStroke(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return false;
    const Parent* p = reg.try_get<const Parent>(e);
    // IsGroupNode, not all_of<StrokeGroup>: a scene authored before this file has a
    // legacy group and no marker anywhere, and nothing stamps one until somebody
    // paints. Reading the legacy shape here is what makes GridNav's exclusion - "a
    // stroke is never nav geometry" - true on those scenes too, instead of only on
    // strokes painted since the upgrade.
    return p && reg.valid(p->entity) && IsGroupNode(reg, p->entity);
}

namespace {

// Every group node - marked and legacy - in a deterministic order: by zone, then by
// sibling order. Sequence stability matters because Rehome reports counts and moves
// things, and because a group's position is part of the answer.
std::vector<entt::entity> GroupNodes(const entt::registry& reg) {
    std::vector<entt::entity> groups;
    for (const entt::entity g : reg.view<const StrokeGroup>()) groups.push_back(g);
    for (const entt::entity g : reg.view<const Name>())
        if (IsLegacyGroupNode(reg, g)) groups.push_back(g);
    std::sort(groups.begin(), groups.end(), [&reg](entt::entity a, entt::entity b) {
        const TagId ta = GroupZone(reg, a), tb = GroupZone(reg, b);
        return ta != tb ? ta < tb : scene::OrderLess(reg, a, b);
    });
    return groups;
}

} // namespace

std::vector<entt::entity> AllStrokes(const entt::registry& reg) {
    std::vector<entt::entity> out;
    const scene::ChildrenMap kids = scene::BuildChildrenMap(reg);
    for (const entt::entity g : GroupNodes(reg)) {
        const auto it = kids.find(static_cast<u32>(g));
        if (it == kids.end()) continue;
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
}

bool HasAnyGroup(const entt::registry& reg) {
    // The pool test first, because it is O(1) and it is the answer on every scene
    // that has been painted in since the upgrade. Only a scene with NO marked group
    // pays for the name scan - and that is exactly the legacy scene the caller (the
    // Scene menu's enable predicate) used to answer "no strokes here" for.
    if (const auto* pool = reg.storage<StrokeGroup>(); pool && !pool->empty()) return true;
    for (const entt::entity e : reg.view<const Name>())
        if (IsLegacyGroupNode(reg, e)) return true;
    return false;
}

usize AdoptLegacyGroups(Scene& scene) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> legacy;
    for (const entt::entity e : reg.view<const Name>())
        if (IsLegacyGroupNode(reg, e)) legacy.push_back(e);
    for (const entt::entity e : legacy) {
        reg.emplace_or_replace<StrokeGroup>(e, StrokeGroup{});
        if (!reg.all_of<Transform>(e)) reg.emplace<Transform>(e, Transform{});
    }
    if (!legacy.empty())
        HBE_INFO("Paint strokes: adopted {} pre-zones '{}' node(s).", legacy.size(),
                 kUntaggedGroupName);
    return legacy.size();
}

usize Rehome(Scene& scene) {
    entt::registry& reg = scene.Registry();
    // A legacy scene has no marker anywhere, so without this the walk below finds
    // nothing to move and the "migration path for strokes authored before zones
    // existed" would be a no-op on exactly the scenes it exists for.
    AdoptLegacyGroups(scene);
    // Snapshot first: Attach mutates the Parent pool, which the children map is
    // derived from.
    const std::vector<entt::entity> strokes = AllStrokes(reg);
    usize moved = 0;
    for (const entt::entity s : strokes) {
        if (!reg.valid(s) || !reg.all_of<Transform>(s)) continue;
        const Parent* p = reg.try_get<const Parent>(s);
        const entt::entity wasGroup =
            (p && reg.valid(p->entity)) ? p->entity : entt::null;
        const TagId was = wasGroup != entt::null ? GroupZone(reg, wasGroup) : kTagUntagged;
        const glm::vec3 worldPos = glm::vec3(scene.WorldMatrix(s)[3]);
        // SLACK, because a stroke is never ON its surface. Paint on the outer face of
        // the wall that defines a zone's combined box sits 1-3 cm outside it, and an
        // exact containment test called that "no zone" - so a re-home DEMOTED
        // correctly-zoned strokes into the always-resident untagged group and then
        // reported success. See kStrokeLiftSlack.
        const TagId now = ZoneOfPosition(scene, worldPos, kStrokeLiftSlack);
        // Losing a zone is the one irreversible direction (the untagged bucket is
        // pinned resident and can never be sharded), so DEMOTION takes a wider band
        // than admission does. Inside that band the stroke keeps the zone it has.
        if (now == kTagUntagged && was != kTagUntagged &&
            ZoneOfPosition(scene, worldPos, kStrokeDemoteSlack) != kTagUntagged)
            continue;
        // The destination GROUP, not just the destination zone: a zone can hold more
        // than one group (one per scene file, and one per shard cell on an
        // auto-sharded tag), so a stroke already in the right zone may still be in the
        // wrong node - which is how a group that grew across cells gets re-split.
        const entt::entity dst =
            FindGroupFor(scene, now, SceneSrcOf(reg, s), worldPos, ShardCellFor(now));
        if (now == was && dst == wasGroup) continue;
        // Back to world space before re-attaching; Attach re-localises against the
        // destination group.
        reg.get<Transform>(s).position = worldPos;
        if (Attach(scene, s, now)) ++moved;
    }
    if (moved > 0)
        HBE_INFO("Paint strokes: re-homed {} stroke(s) into the zone they sit in.", moved);
    return moved;
}

// ---------------------------------------------------------------------------
// --test-strokezones - the headless proof. See StrokeZone.h.
//
// The nav-exclusion contract is asserted directly through strokezone::IsStroke (the
// same predicate the navmesh baker consults, Navigation/NavBaker.cpp) rather than by
// building a navmesh - so this test needs no dependency on the Navigation module and
// keeps Scene/ free of an upward include.
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

usize LiveCount(const Scene& s) {
    const entt::registry& reg = s.Registry();
    const auto* st = reg.storage<entt::entity>();
    if (!st) return 0;
    usize n = 0;
    for (const entt::entity e : *st)
        if (reg.valid(e)) ++n;
    return n;
}

entt::entity ByGuid(const Scene& s, u64 g) {
    const entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<const Guid>())
        if (reg.get<const Guid>(e).value == g) return e;
    return entt::null;
}

// Everything about a stroke that has to survive a despawn/respawn cycle, as ONE
// comparable string. This is the blocker-B3 (morph-cache) shape: the first spawn
// worked and the second silently came back missing something, so the assertion has
// to be "cycle 2 is byte-identical to cycle 1", not "cycle 2 is non-null".
std::string StrokeSignature(const Scene& s, entt::entity e) {
    const entt::registry& reg = s.Registry();
    if (e == entt::null || !reg.valid(e)) return "<absent>";
    char buf[512];
    const MeshRef* mr = reg.try_get<const MeshRef>(e);
    const MaterialRef* mat = reg.try_get<const MaterialRef>(e);
    const MeshInstance* mi = reg.try_get<const MeshInstance>(e);
    const AABB* bb = reg.try_get<const AABB>(e);
    const Tag* tg = reg.try_get<const Tag>(e);
    const Parent* p = reg.try_get<const Parent>(e);
    const bool grouped = p && reg.valid(p->entity) && reg.all_of<const StrokeGroup>(p->entity);
    const glm::vec3 wp = glm::vec3(s.WorldMatrix(e)[3]);
    std::snprintf(
        buf, sizeof(buf),
        "mesh=%s mat=%s rgba=%.3f,%.3f,%.3f,%.3f flags=%u mr=%.3f/%.3f "
        "aabb=%.3f,%.3f,%.3f..%.3f,%.3f,%.3f world=%.4f,%.4f,%.4f tag=%u group=%u",
        mr ? mr->source.c_str() : "-", mat ? mat->asset.c_str() : "-",
        mi ? mi->surface.base_color.r : -1.0f, mi ? mi->surface.base_color.g : -1.0f,
        mi ? mi->surface.base_color.b : -1.0f, mi ? mi->surface.base_color.a : -1.0f,
        mi ? mi->materialFlags : 0u, mi ? mi->surface.base_metalness : -1.0f, mi ? mi->surface.specular_roughness : -1.0f,
        bb ? bb->min.x : 0.0f, bb ? bb->min.y : 0.0f, bb ? bb->min.z : 0.0f,
        bb ? bb->max.x : 0.0f, bb ? bb->max.y : 0.0f, bb ? bb->max.z : 0.0f, wp.x, wp.y,
        wp.z, tg ? static_cast<unsigned>(tg->id) : 0u,
        grouped ? static_cast<unsigned>(GroupZone(reg, p->entity)) : 0xFFFFu);
    return buf;
}

// A stroke as the paint tool leaves it: a ROOT with a world-space transform, a
// mesh, a material link and bounds. Parenting/tagging is Attach's job.
entt::entity MakeStroke(Scene& s, const glm::vec3& worldPos, const std::string& meshSrc,
                        const std::string& matRel) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity("Stroke");
    Transform t;
    t.position = worldPos;
    reg.emplace<Transform>(e, t);
    MeshInstance mi;
    mi.surface.base_color = glm::vec4(0.8f, 0.2f, 0.35f, 0.9f);
    mi.surface.base_metalness = 0.1f;
    mi.surface.specular_roughness = 0.7f;
    mi.materialFlags = rhi::MaterialFlag_Transparent | rhi::MaterialFlag_NoShadow;
    reg.emplace<MeshInstance>(e, mi);
    reg.emplace<MeshRef>(e, MeshRef{meshSrc});
    if (!matRel.empty()) reg.emplace<MaterialRef>(e, MaterialRef{matRel});
    reg.emplace<AABB>(e, AABB{glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f)});
    return e;
}

entt::entity MakeProp(Scene& s, const char* name, const glm::vec3& p, const glm::vec3& half,
                      TagId tag, const char* meshSrc) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity(name);
    Transform t;
    t.position = p;
    reg.emplace<Transform>(e, t);
    reg.emplace<AABB>(e, AABB{-half, half});
    if (meshSrc) {
        reg.emplace<MeshInstance>(e, MeshInstance{});
        reg.emplace<MeshRef>(e, MeshRef{meshSrc});
    }
    if (tag != kTagUntagged) tags::Assign(reg, e, tag);
    return e;
}

} // namespace

bool SelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("strokezones: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_strokezones";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // A real material file, so "material intact" is proved through the same staged
    // load the runtime uses rather than through a dangling path string.
    const std::string matRel = "Strokes/stroke_0.hbmat";
    {
        fs::create_directories(dir / "Strokes", ec);
        MaterialAsset m;
        m.name = "Stroke";
        m.surface.base_color = glm::vec4(0.8f, 0.2f, 0.35f, 0.9f);
        m.surface.base_metalness = 0.1f;
        m.surface.specular_roughness = 0.7f;
        m.flags = rhi::MaterialFlag_Transparent | rhi::MaterialFlag_NoShadow;
        expect(assets::SaveMaterial(dir / matRel, m), "write the test stroke material");
    }

    // The project's tag list. "Camp" and "Mill" stream; nothing is alwaysLoaded but
    // Untagged, which tags::Normalize pins for us.
    std::vector<TagDef> defs;
    {
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        TagDef camp;
        camp.name = "Camp";
        camp.loadRadius = 60.0f;
        TagDef mill;
        mill.name = "Mill";
        mill.loadRadius = 60.0f;
        defs = {untagged, camp, mill};
        tags::Normalize(defs);
        tags::SeedFromProject(defs);
    }
    const TagId tCamp = tags::Intern("Camp"), tMill = tags::Intern("Mill");
    expect(tCamp != kTagUntagged && tMill != kTagUntagged && tCamp != tMill,
           "the two streaming tags intern to distinct non-zero ids");

    // === 1. Zone resolution from the surface hit ==============================
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity hut = MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f),
                                          tCamp, "prim:cube");
        const entt::entity roof = MakeProp(s, "HutRoof", {0.0f, 3.0f, 0.0f}, glm::vec3(4.0f),
                                           kTagUntagged, "prim:cube");
        reg.emplace<Parent>(roof, Parent{hut});
        const entt::entity floor = MakeProp(s, "Floor", {0.0f, 0.0f, 0.0f}, glm::vec3(50.0f),
                                            kTagUntagged, "prim:plane");

        expect(ZoneOfSurface(reg, hut) == tCamp, "a tagged surface resolves to its own zone");
        expect(ZoneOfSurface(reg, roof) == tCamp,
               "an UNTAGGED child of a tagged prop resolves to the prop's zone (walks up)");
        expect(ZoneOfSurface(reg, floor) == kTagUntagged,
               "untagged geometry resolves to the untagged zone");
        expect(ZoneOfSurface(reg, entt::null) == kTagUntagged,
               "no surface hit at all resolves to the untagged zone, not to a crash");

        // Position resolution (what Rehome uses) agrees with the surface answer.
        expect(ZoneOfPosition(s, glm::vec3(100.0f, 1.0f, 0.0f)) == tCamp,
               "a point inside the Camp prop's box resolves to Camp");
        expect(ZoneOfPosition(s, glm::vec3(-40.0f, 0.5f, -40.0f)) == kTagUntagged,
               "a point in no tagged zone resolves to Untagged, NOT to the nearest zone");
    }

    // === 2. Grouping, tagging, and the world position not moving ==============
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");

        const glm::vec3 pA(101.0f, 1.0f, 0.5f), pB(103.0f, 1.0f, -0.5f), pU(0.0f, 0.1f, 0.0f);
        const entt::entity a = MakeStroke(s, pA, "uaf:Strokes/ribbon_0.uaf#0", matRel);
        expect(Attach(s, a, tCamp), "attach a stroke to the Camp zone");
        const entt::entity gCamp = reg.get<Parent>(a).entity;
        expect(reg.all_of<StrokeGroup>(gCamp) && GroupZone(reg, gCamp) == tCamp,
               "the group node is marked, and its zone reads back from its own Tag");
        expect(reg.all_of<Tag>(gCamp) && reg.get<Tag>(gCamp).id == tCamp,
               "THE GROUP NODE ITSELF IS TAGGED - the only shape tagshard::Bake accepts");
        expect(reg.all_of<Tag>(a) && reg.get<Tag>(a).id == tCamp,
               "the stroke carries the same tag, so the bake reads agreement not conflict");
        expect(reg.get<Name>(gCamp).value == GroupName(tCamp),
               "the group node is named for its zone");
        expect(!reg.all_of<Parent>(gCamp), "a group node is a root (one streaming atom)");
        expect(glm::distance(glm::vec3(s.WorldMatrix(a)[3]), pA) < 1e-4f,
               "attaching does not MOVE the stroke (world -> group-local conversion)");
        expect(glm::length(reg.get<Transform>(a).position - pA) > 1e-4f,
               "...and it really was converted: the local transform is no longer the world one");
        expect(glm::distance(glm::vec3(s.WorldMatrix(gCamp)[3]), glm::vec3(0.0f)) > 1.0f,
               "the group node is anchored IN its zone, not at the origin (shard AABB)");

        const entt::entity b = MakeStroke(s, pB, "uaf:Strokes/ribbon_1.uaf#0", matRel);
        expect(Attach(s, b, tCamp), "attach a second Camp stroke");
        expect(reg.get<Parent>(b).entity == gCamp, "the second stroke REUSES the same group");
        expect(glm::distance(glm::vec3(s.WorldMatrix(b)[3]), pB) < 1e-4f,
               "the second stroke does not move either");

        const entt::entity u = MakeStroke(s, pU, "uaf:Strokes/ribbon_2.uaf#0", matRel);
        expect(Attach(s, u, kTagUntagged), "attach a stroke painted on untagged geometry");
        const entt::entity gUn = reg.get<Parent>(u).entity;
        expect(gUn != gCamp, "untagged strokes get their OWN group, not the Camp one");
        expect(!reg.all_of<Tag>(gUn),
               "the untagged group has NO Tag component (Assign removes it for id 0)");
        expect(reg.get<Name>(gUn).value == kUntaggedGroupName,
               "the untagged group keeps the original 'Paint Strokes' name");

        expect(IsStroke(reg, a) && IsStroke(reg, b) && IsStroke(reg, u),
               "all three read as strokes (membership IS the parent link)");
        expect(!IsStroke(reg, gCamp) && !IsStroke(reg, gUn), "a group node is not a stroke");
        expect(AllStrokes(reg).size() == 3, "AllStrokes finds every stroke across every group");

        // A tap stroke goes through the SAME path - before this, SpawnStroke never
        // parented at all and tap strokes were loose untagged roots.
        const entt::entity tap = MakeStroke(s, glm::vec3(102.0f, 1.0f, 0.0f), "prim:plane", matRel);
        expect(Attach(s, tap, tCamp) && reg.get<Parent>(tap).entity == gCamp,
               "a quad TAP stroke groups exactly like a ribbon stroke");
    }

    // === 3. Legacy adoption: a pre-zones scene keeps loading, and keeps its node ==
    {
        Scene s;
        entt::registry& reg = s.Registry();
        // Exactly what the old code produced: a name-only node at the origin.
        const entt::entity legacy = s.CreateEntity(kUntaggedGroupName);
        reg.emplace<Transform>(legacy, Transform{});
        const entt::entity old = MakeStroke(s, glm::vec3(1.0f, 0.0f, 1.0f), "prim:plane", matRel);
        reg.emplace<Parent>(old, Parent{legacy});
        // A MESH that happens to share the name must NOT be hijacked into a group.
        const entt::entity decoy =
            MakeProp(s, kUntaggedGroupName, {5.0f, 0.0f, 5.0f}, glm::vec3(1.0f), kTagUntagged,
                     "prim:cube");

        const entt::entity fresh = MakeStroke(s, glm::vec3(2.0f, 0.0f, 2.0f), "prim:plane", matRel);
        expect(Attach(s, fresh, kTagUntagged), "attach into a pre-zones scene");
        expect(reg.get<Parent>(fresh).entity == legacy,
               "the legacy 'Paint Strokes' node is ADOPTED, not duplicated");
        expect(reg.all_of<StrokeGroup>(legacy) && GroupZone(reg, legacy) == kTagUntagged,
               "adoption stamps StrokeGroup on the existing node");
        expect(!reg.all_of<StrokeGroup>(decoy),
               "a MESH named 'Paint Strokes' is never adopted as a group");
        usize groups = 0;
        for (const entt::entity g : reg.view<const StrokeGroup>()) {
            (void)g;
            ++groups;
        }
        expect(groups == 1, "adoption creates no second group");
        expect(IsStroke(reg, old), "the pre-existing stroke is a stroke once its node is adopted");
    }

    // === 4. Save/load round trip, and a fixed point ===========================
    std::string firstSave;
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
        MakeProp(s, "Mill", {-100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tMill, "prim:cube");
        // A REAL generated ribbon `.uaf`, not another `prim:plane`. The drag path's
        // actual asset shape is a file written into Assets/Strokes/ and referenced as
        // "uaf:<rel>#0", and it is the only stroke shape that has to survive
        // StageAssets / SplitUafSource; a test made entirely of primitives never
        // touches that half. (The geometry is GeneratePlane, so the reloaded bounds
        // are bit-identical to MakeStroke's and the signature comparison below still
        // means what it says.)
        const std::string ribbonRel = "Strokes/ribbon_0.uaf";
        expect(uaf::WriteMesh(dir / ribbonRel, Model{mesh::GeneratePlane(1.0f, 1)}),
               "write a real ribbon .uaf, the shape the drag path produces");
        const entt::entity a =
            MakeStroke(s, {101.0f, 1.0f, 0.0f}, "uaf:" + ribbonRel + "#0", matRel);
        Attach(s, a, tCamp);
        const entt::entity m = MakeStroke(s, {-101.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        Attach(s, m, tMill);
        const entt::entity u = MakeStroke(s, {0.0f, 0.1f, 0.0f}, "prim:plane", matRel);
        Attach(s, u, kTagUntagged);
        const std::string sigA = StrokeSignature(s, a), sigM = StrokeSignature(s, m),
                          sigU = StrokeSignature(s, u);
        const u64 gA = reg.get<Guid>(a).value, gM = reg.get<Guid>(m).value,
                  gU = reg.get<Guid>(u).value;

        firstSave = scene::SaveSceneToString(s);
        scene::SceneData data;
        expect(scene::ParseSceneString(firstSave, data), "the saved scene re-parses");

        Renderer renderer; // device-less: uploads return invalid handles, no GPU
        Scene s2;
        scene::StagedAssets staged;
        scene::StageAssets(data, dir, staged);
        expect(staged.models.count(ribbonRel) == 1,
               "the ribbon .uaf STAGES - the generated-asset half of the drag path really "
               "does round-trip, not just a primitive name");
        scene::Instantiate(s2, renderer, data, staged, scene::LoadMode::Replace);

        const entt::entity a2 = ByGuid(s2, gA), m2 = ByGuid(s2, gM), u2 = ByGuid(s2, gU);
        expect(a2 != entt::null && m2 != entt::null && u2 != entt::null,
               "every stroke came back by guid");
        expect(StrokeSignature(s2, a2) == sigA && StrokeSignature(s2, m2) == sigM &&
                   StrokeSignature(s2, u2) == sigU,
               "grouping, zone tag, mesh, material and world position all round-trip");
        usize groups2 = 0;
        for (const entt::entity g : s2.Registry().view<const StrokeGroup>()) {
            (void)g;
            ++groups2;
        }
        expect(groups2 == 3, "all three group nodes (Camp, Mill, Untagged) round-trip");
        expect(scene::SaveSceneToString(s2) == firstSave,
               "save -> load -> save is a FIXED POINT (the --test-scenesave contract)");
    }

    // === 5. A stroke is never nav geometry ====================================
    // Asserted through IsStroke - the exact predicate NavBaker's geometry gather uses to
    // skip strokes - so the contract is checked without building a navmesh.
    {
        Scene s;
        const entt::entity floor = MakeProp(s, "Floor", {0.0f, 0.0f, 0.0f},
                                            glm::vec3(20.0f, 0.1f, 20.0f), kTagUntagged, "prim:plane");
        entt::registry& reg = s.Registry();
        expect(!IsStroke(reg, floor), "an ordinary floor prop IS nav geometry (not a stroke)");

        // A stroke lying on that floor is a decal, excluded from the navmesh.
        const entt::entity st = MakeStroke(s, {0.0f, 0.02f, 0.0f}, "prim:plane", matRel);
        Attach(s, st, kTagUntagged);
        expect(IsStroke(reg, st),
               "a paint stroke is excluded from the navmesh - strokes are decals, not collision");

        // The exclusion is about being a stroke, not the mesh source: an ordinary prop
        // with the SAME mesh is still nav geometry. Without this, a bug that rejected
        // every `prim:plane` would pass the assertion above.
        const entt::entity crate = MakeProp(s, "Crate", {3.0f, 0.0f, 3.0f}, glm::vec3(0.5f),
                                            kTagUntagged, "prim:plane");
        expect(!IsStroke(reg, crate),
               "...while a normal prop with the same mesh is still nav geometry");
    }

    // === 6a. THE DISCRIMINATING CASE: the OLD shape must fail ==================
    // Without this, "the new shape bakes with zero errors" measures nothing - a
    // single-zone level bakes cleanly either way, because one tag under an untagged
    // root is only a WARNING (tagshard promotes it). It takes TWO zones under one
    // global group to reach the Error branch, and that is exactly the level the old
    // code produced: every stroke in the world under one node at the origin.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
        MakeProp(s, "Mill", {-100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tMill, "prim:cube");
        // Verbatim the pre-zones shape: ONE untagged group node at the origin, with
        // per-zone tags on the leaves.
        const entt::entity legacy = s.CreateEntity(kUntaggedGroupName);
        reg.emplace<Transform>(legacy, Transform{});
        const entt::entity cs = MakeStroke(s, {101.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        reg.emplace<Parent>(cs, Parent{legacy});
        tags::Assign(reg, cs, tCamp);
        const entt::entity ms = MakeStroke(s, {-101.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        reg.emplace<Parent>(ms, Parent{legacy});
        tags::Assign(reg, ms, tMill);

        const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
        expect(rep.errors > 0,
               "the OLD shape (one global group, tags on the leaves) is a BAKE ERROR - "
               "so the zero-error assertion below is measuring the fix, not the status quo");
        expect(reg.get<Tag>(cs).shard < 0 && reg.get<Tag>(ms).shard < 0,
               "...and under the old shape neither stroke gets a shard: it stays RESIDENT "
               "and both tags are ignored, exactly as TagShard.cpp warns");
    }

    // === 6b. Bake, stream out, stream back in =================================
    {
        const fs::path file = dir / "Camp.hbscene";
        u64 gCampStroke = 0, gUnStroke = 0, gCampGroup = 0, gUnGroup = 0, gMillStroke = 0;
        std::string authoredSig;
        {
            Scene s;
            entt::registry& reg = s.Registry();
            MakeProp(s, "Floor", {0.0f, 0.0f, 0.0f}, glm::vec3(20.0f, 0.1f, 20.0f),
                     kTagUntagged, "prim:plane");
            MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
            MakeProp(s, "Mill", {-100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tMill, "prim:cube");
            // The SAME two-zone content as 6a, in the zoned shape.
            const entt::entity cs =
                MakeStroke(s, {101.0f, 1.0f, 0.5f}, "prim:plane", matRel);
            expect(Attach(s, cs, tCamp), "attach the Camp stroke");
            const entt::entity ms =
                MakeStroke(s, {-101.0f, 1.0f, 0.5f}, "prim:plane", matRel);
            expect(Attach(s, ms, tMill), "attach the Mill stroke");
            const entt::entity us = MakeStroke(s, {0.0f, 0.2f, 0.0f}, "prim:plane", matRel);
            expect(Attach(s, us, kTagUntagged), "attach the untagged stroke");
            gCampStroke = reg.get<Guid>(cs).value;
            gMillStroke = reg.get<Guid>(ms).value;
            gUnStroke = reg.get<Guid>(us).value;
            gCampGroup = reg.get<Guid>(reg.get<Parent>(cs).entity).value;
            gUnGroup = reg.get<Guid>(reg.get<Parent>(us).entity).value;

            const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
            // THE ASSERTION THIS WHOLE DESIGN EXISTS FOR - and 6a just proved the
            // old shape does NOT satisfy it.
            expect(rep.errors == 0,
                   "the zoned stroke groups bake with ZERO errors (no cross-shard parent)");
            expect(reg.get<Tag>(cs).shard >= 0 && reg.get<Tag>(ms).shard >= 0,
                   "...and both strokes really are baked into a shard");
            expect(scene::SaveScene(s, file, {}, SceneKind::Full, &rep.shards),
                   "save the baked scene");
            authoredSig = StrokeSignature(s, cs);
        }

        world::Get().Clear();
        world::SetCurrentArea({});

        Renderer renderer;
        Scene s;
        stream::Streamer st;
        // Guarded on THIS section's own precondition, not on the global `ok`: an
        // unrelated failure earlier must not silently skip the streaming half (which
        // is the half this task is actually about).
        const bool bound = st.BindLevel(s, renderer, file, dir, defs);
        expect(bound, "bind the baked level");
        if (bound) {
            const i32 iCamp = st.FindShard("Camp#0");
            expect(iCamp >= 0, "the Camp shard is addressable");
            expect(ByGuid(s, gCampStroke) == entt::null && ByGuid(s, gCampGroup) == entt::null,
                   "THE CAMP STROKE AND ITS GROUP ARE NOT RESIDENT AT BIND - they stream");
            expect(ByGuid(s, gUnStroke) != entt::null && ByGuid(s, gUnGroup) != entt::null,
                   "the UNTAGGED stroke and its group ARE resident (Untagged is pinned)");
            const usize nBind = LiveCount(s);
            const std::string unSig = StrokeSignature(s, ByGuid(s, gUnStroke));

            if (iCamp >= 0) {
                expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)), "spawn Camp#0");
                const entt::entity cs1 = ByGuid(s, gCampStroke);
                expect(cs1 != entt::null, "the Camp stroke spawned");
                const std::string sig1 = StrokeSignature(s, cs1);
                expect(sig1 == authoredSig,
                       "the spawned stroke matches what was authored - mesh, material, "
                       "bounds, world position, zone tag and group all intact");
                expect(IsStroke(s.Registry(), cs1),
                       "the spawned stroke rode its group node into the shard");
                expect(ByGuid(s, gMillStroke) == entt::null,
                       "spawning Camp does NOT drag the Mill zone's strokes in - the two "
                       "groups are independent streaming atoms");

                expect(st.DespawnShard(s, static_cast<u32>(iCamp)), "despawn Camp#0");
                expect(ByGuid(s, gCampStroke) == entt::null && ByGuid(s, gCampGroup) == entt::null,
                       "despawn takes the stroke AND its group node");
                expect(ByGuid(s, gUnStroke) != entt::null,
                       "the untagged stroke is untouched by the cycle - it STAYS RESIDENT");
                expect(LiveCount(s) == nBind, "despawn returns to exactly the bind population");

                expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)), "RESPAWN Camp#0");
                const entt::entity cs2 = ByGuid(s, gCampStroke);
                // The blocker-B3 shape: the SECOND spawn is where a cached-resource
                // path silently drops something.
                expect(StrokeSignature(s, cs2) == sig1,
                       "the RESPAWNED stroke is byte-identical to the first spawn");
                expect(StrokeSignature(s, ByGuid(s, gUnStroke)) == unSig,
                       "the resident untagged stroke is unchanged after two cycles");

                // A third cycle, to pin that nothing drifts or accumulates.
                expect(st.DespawnShard(s, static_cast<u32>(iCamp)) &&
                           st.SpawnShard(s, renderer, static_cast<u32>(iCamp)),
                       "a second full cycle runs");
                expect(StrokeSignature(s, ByGuid(s, gCampStroke)) == sig1,
                       "cycle 2 is identical to cycle 1 (no drift)");
            }
        }
        world::Get().Clear();
        world::SetCurrentArea({});
    }

    // === 7. Re-home: the remedy when a surface's tag changes afterwards ========
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity hut = MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(6.0f),
                                          kTagUntagged, "prim:cube");
        // Painted while the hut was still untagged -> lands in the untagged group.
        const entt::entity a = MakeStroke(s, {101.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        Attach(s, a, ZoneOfSurface(reg, hut));
        expect(!reg.all_of<Tag>(a), "the stroke starts untagged, like its surface");
        const glm::vec3 before = glm::vec3(s.WorldMatrix(a)[3]);

        // The author now tags the hut "Camp" and asks for a re-home.
        tags::Assign(reg, hut, tCamp);
        expect(Rehome(s) == 1, "one stroke re-homes into the zone it sits in");
        expect(reg.all_of<Tag>(a) && reg.get<Tag>(a).id == tCamp, "...and picks up the zone tag");
        const entt::entity g = reg.get<Parent>(a).entity;
        expect(reg.all_of<StrokeGroup>(g) && GroupZone(reg, g) == tCamp,
               "...under the Camp group");
        expect(glm::distance(glm::vec3(s.WorldMatrix(a)[3]), before) < 1e-4f,
               "re-homing does not move the stroke");
        expect(Rehome(s) == 0, "re-homing again is a no-op (idempotent)");
    }

    // === 8. Re-home must not DEMOTE a stroke painted on a zone's own boundary ==
    // Every stroke is created OFF its surface (a tap is lifted 0.02 + up to 0.012 m
    // along the hit normal), so paint on the outer face of the wall that DEFINES the
    // zone's combined box sits a few centimetres outside it. Under exact containment
    // Rehome answered "no zone", moved the stroke into the always-resident untagged
    // group, stripped its Tag - and reported success. The zone was then unrecoverable
    // without re-tagging, and a second run said "already in the right zones".
    {
        Scene s;
        entt::registry& reg = s.Registry();
        // The hut IS the zone: its box is exactly [96,104] on x.
        MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
        // Painted on the outer face at x = 104, lifted 2.5 cm along +X.
        const entt::entity a = MakeStroke(s, {104.025f, 1.0f, 0.0f}, "prim:plane", matRel);
        expect(Attach(s, a, tCamp), "attach a stroke painted on the zone's boundary face");
        expect(ZoneOfPosition(s, glm::vec3(s.WorldMatrix(a)[3])) == kTagUntagged,
               "EXACT containment really does miss it - so this section measures the fix");
        expect(ZoneOfPosition(s, glm::vec3(s.WorldMatrix(a)[3]), kStrokeLiftSlack) == tCamp,
               "...and the lift slack recovers the right answer");
        expect(Rehome(s) == 0, "re-home leaves a boundary stroke where it is");
        expect(reg.all_of<Tag>(a) && reg.get<Tag>(a).id == tCamp,
               "...with its zone tag intact (it was NOT demoted to Untagged)");
        expect(GroupZone(reg, reg.get<Parent>(a).entity) == tCamp,
               "...and still under the Camp group");

        // A stroke genuinely well outside every zone still demotes: the hysteresis is
        // a band, not a refusal.
        const entt::entity stray = MakeStroke(s, {160.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        reg.emplace_or_replace<Parent>(stray, Parent{reg.get<Parent>(a).entity});
        tags::Assign(reg, stray, tCamp);
        expect(Rehome(s) == 1, "a stroke far outside every zone DOES re-home");
        expect(!reg.all_of<Tag>(stray), "...into the untagged group");
    }

    // === 9. A pre-zones scene is REACHABLE: nav, the menu gate, and migration ==
    // Nothing stamps a StrokeGroup on load, so a scene authored before this file has a
    // plain "Paint Strokes" node and no marker anywhere. Keying everything off the
    // marker made the re-home menu item grey out on exactly those scenes, and left
    // every legacy stroke as nav geometry - a stroke on a wall was still a wall.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "Floor", {0.0f, 0.0f, 0.0f}, glm::vec3(20.0f, 0.1f, 20.0f), kTagUntagged,
                 "prim:plane");

        const entt::entity legacy = s.CreateEntity(kUntaggedGroupName);
        reg.emplace<Transform>(legacy, Transform{});
        const entt::entity old = MakeStroke(s, {1.0f, 0.02f, 1.0f}, "prim:plane", matRel);
        reg.emplace<Parent>(old, Parent{legacy});
        expect(!reg.all_of<StrokeGroup>(legacy),
               "the fixture really is unmarked (this is what a legacy scene looks like)");

        expect(HasAnyGroup(reg), "the re-home menu is ENABLED on a legacy scene");
        // IsStroke keys off the stroke marker, not the group node, so an unadopted legacy
        // stroke still reads as a stroke - which is what excludes it from the navmesh.
        expect(IsStroke(reg, old),
               "a legacy stroke reads as a stroke (so it is excluded from the navmesh too)");
        expect(AllStrokes(reg).size() == 1, "AllStrokes sees it, so Rehome has something to move");

        // The migration itself: tag the hut the stroke sits on, then re-home.
        MakeProp(s, "Hut", {1.0f, 0.0f, 1.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
        expect(Rehome(s) == 1, "the legacy stroke migrates into its zone");
        expect(reg.all_of<StrokeGroup>(legacy), "...and the legacy node is adopted, not orphaned");
        expect(GroupZone(reg, reg.get<Parent>(old).entity) == tCamp, "...under the Camp group");
    }

    // === 10. A RENAMED zone group is never hijacked as the untagged one ========
    // The header promises renaming a group in the Hierarchy is safe, because groups
    // are found by component. The legacy scan was a bare name match, so renaming
    // "Paint Strokes - Camp" back to "Paint Strokes" made the next untagged paint
    // re-stamp that node - which KEPT its Tag{Camp}. The untagged group was then
    // inside Camp's streaming atom, and paint on always-resident terrain despawned
    // with a zone.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "Hut", {100.0f, 0.0f, 0.0f}, glm::vec3(4.0f), tCamp, "prim:cube");
        const entt::entity c = MakeStroke(s, {101.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        expect(Attach(s, c, tCamp), "a Camp stroke makes the Camp group");
        const entt::entity gCamp = reg.get<Parent>(c).entity;
        reg.get<Name>(gCamp).value = kUntaggedGroupName; // the artist renames it

        const entt::entity u = MakeStroke(s, {0.0f, 0.1f, 0.0f}, "prim:plane", matRel);
        expect(Attach(s, u, kTagUntagged), "attach a stroke painted on untagged terrain");
        const entt::entity gUn = reg.get<Parent>(u).entity;
        expect(gUn != gCamp, "the renamed CAMP group is not adopted as the untagged one");
        expect(!reg.all_of<Tag>(gUn), "the untagged group has no Tag, so it stays resident");
        expect(reg.all_of<Tag>(gCamp) && GroupZone(reg, gCamp) == tCamp,
               "...and the renamed group still collects Camp");
    }

    // === 11. A group belongs to ONE scene FILE ================================
    // Groups are found across the whole registry, so with a second `.hbscene` streamed
    // in additively the first group found could belong to the OTHER file. Ctrl+S then
    // writes the active scene WITHOUT that group, EntityToJson drops the parent link
    // it cannot resolve, and the stroke reloads as a root at its group-LOCAL
    // transform - a silent teleport, the same one Editor::Reparent guards with
    // MoveToScene.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        // Scene B, streamed in additively: its content carries a SceneSource.
        const entt::entity bStroke = MakeStroke(s, {200.0f, 1.0f, 0.0f}, "prim:plane", matRel);
        reg.emplace<SceneSource>(bStroke, SceneSource{"B.hbscene"});
        expect(Attach(s, bStroke, kTagUntagged), "attach a stroke owned by scene B");
        const entt::entity gB = reg.get<Parent>(bStroke).entity;
        expect(SceneSrcOf(reg, gB) == "B.hbscene",
               "the group B needed was created INTO B, so Save All writes them together");

        // Now paint in the ACTIVE scene (no SceneSource).
        const entt::entity aStroke = MakeStroke(s, {0.0f, 0.1f, 0.0f}, "prim:plane", matRel);
        expect(Attach(s, aStroke, kTagUntagged), "attach a stroke in the active scene");
        const entt::entity gA = reg.get<Parent>(aStroke).entity;
        expect(gA != gB, "it does NOT join scene B's group");
        expect(SceneSrcOf(reg, gA).empty(), "...it gets the active scene's own group");
    }

    // === 12. An auto-sharded zone gets per-cell groups ========================
    // A root is ONE atom, and tagshard::Bake unions every grid cell an atom's box
    // touches into one component. One group per zone spans the whole zone as soon as
    // two strokes are painted at opposite ends, which collapses an autoShard tag into
    // a SINGLE shard and silently un-does per-shard streaming for every prop in it.
    //
    // ShardCellFor reads the live project, and a headless test has none - so this
    // section asserts the two halves it CAN reach: the default (no project) really is
    // one group per zone, and the cell split, when a cell is in force, puts distant
    // strokes in different nodes. CellOf/FindGroupFor is the mechanism both use.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        MakeProp(s, "CampWide", {0.0f, 0.0f, 0.0f}, glm::vec3(400.0f, 4.0f, 400.0f), tCamp,
                 "prim:cube");
        const glm::vec3 near0(-300.0f, 1.0f, -300.0f), far0(300.0f, 1.0f, 300.0f);
        const entt::entity s0 = MakeStroke(s, near0, "prim:plane", matRel);
        const entt::entity s1 = MakeStroke(s, far0, "prim:plane", matRel);
        expect(Attach(s, s0, tCamp) && Attach(s, s1, tCamp), "two far-apart Camp strokes");
        // Guarded, not asserted: a run that DOES have a project open (nothing stops
        // `--test-strokezones --project ...`) legitimately reports a cell here.
        if (ShardCellFor(tCamp) == 0.0f)
            expect(reg.get<Parent>(s0).entity == reg.get<Parent>(s1).entity,
                   "...share one group while the tag is not auto-sharded");

        // The split itself, exercised through the same lookup EnsureGroup uses.
        const f32 cell = 120.0f;
        expect(CellOf(near0, cell) != CellOf(far0, cell),
               "the two strokes really are in different shard cells");
        const entt::entity g0 = reg.get<Parent>(s0).entity;
        expect(FindGroupFor(s, tCamp, std::string(), near0, cell) == g0,
               "the existing group serves its OWN cell");
        expect(FindGroupFor(s, tCamp, std::string(), far0, cell) == entt::null,
               "...and does not serve a cell it is not in, so a second node is created there");
    }

    fs::remove_all(dir, ec);
    return ok;
}

} // namespace hbe::strokezone

#include "Construction/ConstructionDef.h"

#include "Core/Rng.h"

#include <algorithm>
#include <cmath>

namespace hbe::construction {

namespace {

template <typename T, usize N>
const char* NameOf(const char* (&table)[N], T v) {
    const auto i = static_cast<usize>(v);
    return i < N ? table[i] : "?";
}

const char* kKindNames[] = {
    "Unknown", "Building", "Foundation", "Floor", "FloorSurface", "Wall", "Ceiling", "Roof",
    "RoofSurface", "Column", "Beam", "Joist", "Rafter", "Truss", "Stud", "Plate", "Header",
    "Brace", "Opening", "Door", "Window", "Stair", "Siding", "Sheathing", "Detail"};
static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == static_cast<usize>(ComponentKind::Count),
              "ComponentKind and its name table must stay in lockstep");

const char* kRoleNames[] = {"None", "Foundation", "LoadBearing", "NonLoadBearing", "Transfer",
                            "Surface"};
static_assert(sizeof(kRoleNames) / sizeof(kRoleNames[0]) == static_cast<usize>(StructuralRole::Count),
              "StructuralRole and its name table must stay in lockstep");

const char* kMaterialNames[] = {"Unknown", "Brick",     "ConcreteBlock", "Stone",  "PouredConcrete",
                                "TimberFrame", "WoodPlank", "Plywood",   "OSB",    "WoodShingle",
                                "Drywall", "Plaster",   "Metal",         "CorrugatedMetal", "Glass", "Mortar"};
static_assert(sizeof(kMaterialNames) / sizeof(kMaterialNames[0]) ==
                  static_cast<usize>(MaterialKind::Count),
              "MaterialKind and its name table must stay in lockstep");

const char* kBondNames[] = {"Running", "Stack", "Flemish", "English"};
static_assert(sizeof(kBondNames) / sizeof(kBondNames[0]) == static_cast<usize>(BondPattern::Count),
              "BondPattern and its name table must stay in lockstep");

const char* kMemberNames[] = {"Stud", "Plate", "Joist", "Rafter", "Board", "Batten", "Shingle"};
static_assert(sizeof(kMemberNames) / sizeof(kMemberNames[0]) == static_cast<usize>(MemberRole::Count),
              "MemberRole and its name table must stay in lockstep");

const char* kDirNames[] = {"Horizontal", "Vertical", "Diagonal"};
static_assert(sizeof(kDirNames) / sizeof(kDirNames[0]) == static_cast<usize>(BoardDirection::Count),
              "BoardDirection and its name table must stay in lockstep");

const char* kProfileNames[] = {"Flush", "Clapboard", "Shiplap", "BoardAndBatten"};
static_assert(sizeof(kProfileNames) / sizeof(kProfileNames[0]) ==
                  static_cast<usize>(SidingProfile::Count),
              "SidingProfile and its name table must stay in lockstep");

const char* kEdgeNames[] = {"Bears", "Attaches", "Contains", "Occupies", "Braces"};
static_assert(sizeof(kEdgeNames) / sizeof(kEdgeNames[0]) == static_cast<usize>(EdgeKind::Count),
              "EdgeKind and its name table must stay in lockstep");

} // namespace

const char* ToString(ComponentKind k) { return NameOf(kKindNames, k); }
const char* ToString(StructuralRole r) { return NameOf(kRoleNames, r); }
const char* ToString(MaterialKind m) { return NameOf(kMaterialNames, m); }
const char* ToString(EdgeKind e) { return NameOf(kEdgeNames, e); }
const char* ToString(BondPattern b) { return NameOf(kBondNames, b); }

const char* ToString(MemberRole r) { return NameOf(kMemberNames, r); }
const char* ToString(BoardDirection d) { return NameOf(kDirNames, d); }
const char* ToString(SidingProfile p) { return NameOf(kProfileNames, p); }

namespace {
// Age is the master dial; poor maintenance multiplies it. Clamped so an artist cannot drive a
// wall past "every unit missing", which would generate nothing and read as a bug.
f32 Decay(f32 age, f32 maintenance, f32 scale) {
    const f32 neglect = 1.0f + (1.0f - std::clamp(maintenance, 0.0f, 1.0f)) * 2.0f;
    return std::clamp(std::clamp(age, 0.0f, 1.0f) * neglect * scale, 0.0f, 0.9f);
}
} // namespace

f32 WeatheringParams::EffectiveMissing() const {
    if (missingChance >= 0.0f) return std::clamp(missingChance, 0.0f, 0.9f);
    // Deliberately the SLOWEST of the three. A missing unit is a hole in the building, and a wall
    // that loses a tenth of its bricks at age 0.2 reads as bomb damage rather than as age.
    return Decay(age * age, maintenance, 0.25f);
}

f32 WeatheringParams::EffectiveChip() const {
    if (chipAmount >= 0.0f) return std::clamp(chipAmount, 0.0f, 0.9f);
    // Erosion is the FIRST thing to show and the most forgiving - it never opens a hole.
    return Decay(age, maintenance, 0.35f) * (0.5f + std::clamp(exposure, 0.0f, 1.0f) * 0.5f);
}

f32 WeatheringParams::EffectiveDisplace() const {
    if (displacement >= 0.0f) return std::clamp(displacement, 0.0f, 0.9f);
    // Frost heave and settlement: driven by moisture as much as by time.
    return Decay(age, maintenance, 0.30f) * (0.4f + std::clamp(moisture, 0.0f, 1.0f) * 0.6f);
}

f32 WeatheringParams::ColourShift() const {
    return std::clamp(std::clamp(age, 0.0f, 1.0f) *
                          (1.0f + (1.0f - std::clamp(maintenance, 0.0f, 1.0f))),
                      0.0f, 1.0f);
}

bool IsPlankMaterial(MaterialKind m) {
    // Sheet goods (Plywood, OSB) are laid as boards too - a sheet is just a very wide board, and
    // treating them separately would duplicate the whole layout for no behavioural difference.
    return m == MaterialKind::WoodPlank || m == MaterialKind::Plywood || m == MaterialKind::OSB;
}

bool IsMasonry(MaterialKind m) {
    // PouredConcrete is deliberately absent: it is placed as a continuous pour, has no units and
    // no bond, and laying it as bricks would be exactly the "different textures pretending to be
    // different construction" the brief rejects. It stays a solid massing block.
    return m == MaterialKind::Brick || m == MaterialKind::ConcreteBlock ||
           m == MaterialKind::Stone;
}

// ---------------------------------------------------------------------------
// DamageState
// ---------------------------------------------------------------------------

bool DamageState::IsDestroyed(ComponentId id) const {
    return std::binary_search(destroyed.begin(), destroyed.end(), id);
}

void DamageState::Destroy(ComponentId id) {
    if (id == kInvalidComponent) return;
    // Kept sorted so IsDestroyed is a binary search: a damage pass asks this once per component
    // per evaluation, and a linear scan turns a wall of a few hundred pieces into a quadratic.
    const auto at = std::lower_bound(destroyed.begin(), destroyed.end(), id);
    if (at == destroyed.end() || *at != id) destroyed.insert(at, id);
}

bool DamageState::IsEdgeBroken(ComponentId supported, ComponentId supporter) const {
    for (const EdgeBreak& b : brokenEdges)
        if (b.supported == supported && b.supporter == supporter) return true;
    return false;
}

void DamageState::BreakEdge(ComponentId supported, ComponentId supporter) {
    if (supported == kInvalidComponent || supporter == kInvalidComponent) return;
    if (!IsEdgeBroken(supported, supporter)) brokenEdges.push_back({supported, supporter});
}

void DamageState::Clear() {
    destroyed.clear();
    brokenEdges.clear();
}

// ---------------------------------------------------------------------------
// ConstructionDef
// ---------------------------------------------------------------------------

ComponentId ConstructionDef::AddComponent(ConstructionComponent c) {
    // MONOTONIC, NEVER RECYCLED. `nextId` is serialized with the definition precisely so that a
    // load followed by an add cannot hand out an id a deleted component once used - a stale
    // damage record naming a recycled id would damage an unrelated component, and nothing would
    // report it.
    c.id = nextId++;
    components.push_back(std::move(c));
    return components.back().id;
}

void ConstructionDef::RemoveComponent(ComponentId id) {
    if (id == kInvalidComponent) return;
    components.erase(std::remove_if(components.begin(), components.end(),
                                    [id](const ConstructionComponent& c) { return c.id == id; }),
                     components.end());
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [id](const SupportEdge& e) {
                                   return e.supported == id || e.supporter == id;
                               }),
                edges.end());
    // Children are deliberately NOT rehomed. An orphan is reported by Validate; silently
    // reparenting a stud into a different wall would be a worse outcome than an error.
}

void ConstructionDef::AddEdge(ComponentId supported, ComponentId supporter, EdgeKind kind,
                              f32 capacity) {
    if (supported == kInvalidComponent || supporter == kInvalidComponent) return;
    if (supported == supporter) return; // a component cannot support itself
    edges.push_back(SupportEdge{supported, supporter, kind, capacity});
}

const ConstructionComponent* ConstructionDef::Find(ComponentId id) const {
    for (const ConstructionComponent& c : components)
        if (c.id == id) return &c;
    return nullptr;
}

ConstructionComponent* ConstructionDef::Find(ComponentId id) {
    return const_cast<ConstructionComponent*>(
        static_cast<const ConstructionDef*>(this)->Find(id));
}

u64 ConstructionDef::SeedFor(ComponentId id) const {
    const ConstructionComponent* c = Find(id);
    const u64 salt = c ? c->seedSalt : 0;
    // Derived from the STABLE ID and the artist's salt - never from an index or a counter. Rng
    // is SplitMix64 with a defined float mapping, chosen over <random> because the standard
    // leaves distributions unspecified and they genuinely differ between STLs.
    return Rng(seed).Split(static_cast<u64>(id)).Split(salt).State();
}

u64 ConstructionDef::SeedFor(ComponentId id, u64 purpose) const {
    // An independent sub-stream. "mortar" and "warp" can be drawn in either order, on any
    // thread, in any quantity, without shifting each other - which is what lets a later phase
    // add a new consumer without reshuffling every building already authored.
    return Rng(SeedFor(id)).Split(purpose).State();
}

bool ConstructionDef::Validate(std::vector<std::string>& outErrors) const {
    const usize before = outErrors.size();

    auto idText = [](ComponentId id) { return std::to_string(static_cast<u64>(id)); };

    // Duplicate ids would make Find ambiguous and damage records unresolvable.
    std::vector<ComponentId> seen;
    seen.reserve(components.size());
    for (const ConstructionComponent& c : components) {
        if (c.id == kInvalidComponent) {
            outErrors.push_back("a component has the invalid id 0");
            continue;
        }
        if (c.id >= nextId)
            outErrors.push_back("component " + idText(c.id) +
                                " has an id at or past nextId - the mint counter is behind, so "
                                "the next AddComponent would collide with it");
        if (std::find(seen.begin(), seen.end(), c.id) != seen.end())
            outErrors.push_back("duplicate component id " + idText(c.id));
        else
            seen.push_back(c.id);
    }

    for (const ConstructionComponent& c : components) {
        if (c.parent != kInvalidComponent && !Find(c.parent))
            outErrors.push_back("component " + idText(c.id) + " has parent " + idText(c.parent) +
                                " which does not exist");
        if (c.parent == c.id)
            outErrors.push_back("component " + idText(c.id) + " is its own parent");
    }

    // A containment cycle would hang any subtree walk. Walk each chain with a bounded step count
    // rather than a visited set - cheaper, and the bound IS the cycle detector.
    for (const ConstructionComponent& c : components) {
        ComponentId walk = c.parent;
        usize steps = 0;
        while (walk != kInvalidComponent && steps++ <= components.size()) {
            if (walk == c.id) {
                outErrors.push_back("containment cycle through component " + idText(c.id));
                break;
            }
            const ConstructionComponent* p = Find(walk);
            walk = p ? p->parent : kInvalidComponent;
        }
    }

    for (const SupportEdge& e : edges) {
        if (!Find(e.supported))
            outErrors.push_back("support edge names missing supported component " +
                                idText(e.supported));
        if (!Find(e.supporter))
            outErrors.push_back("support edge names missing supporter component " +
                                idText(e.supporter));
        if (e.capacity < 0.0f)
            outErrors.push_back("support edge " + idText(e.supported) + " <- " +
                                idText(e.supporter) + " has negative capacity");
    }

    return outErrors.size() == before;
}

} // namespace hbe::construction

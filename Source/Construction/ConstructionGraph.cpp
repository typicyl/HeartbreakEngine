#include "Construction/ConstructionGraph.h"

#include <algorithm>

namespace hbe::construction {

namespace {

// ONLY `Bears` carries load. The others are relationships, not support: a wall CONTAINS a stud as
// an authoring fact, a window OCCUPIES an opening, and a BRACE resists lateral movement without
// carrying vertical load. Counting any of them as structural support would make every decorative
// component read as structure, and nothing in the building would ever come back unsupported - a
// failure that looks exactly like the system working.
bool CarriesLoad(EdgeKind k) { return k == EdgeKind::Bears; }

// WHAT HOLDS A COMPONENT UP - which is NOT the same question as what load it carries, and
// conflating the two is a bug this code had on its first run.
//
// Cladding (StructuralRole::Surface - siding, drywall, sheathing) bears no load and is bound to
// its host by an `Attaches` edge. Judged by load alone it has no path to ground, so a perfectly
// intact wall reported its own siding as unsupported. That is wrong in the way that matters: the
// siding is held up, by attachment, and it falls when its host falls and not one moment sooner.
//
// So the rule depends on the role of the component being asked about: a Surface is anchored
// through what it is attached to; everything else is anchored only through load. A Surface still
// never acts as a SUPPORTER for anything - hanging a beam off siding remains meaningless.
bool HoldsUp(EdgeKind k, StructuralRole supportedRole) {
    if (CarriesLoad(k)) return true;
    return supportedRole == StructuralRole::Surface && k == EdgeKind::Attaches;
}

} // namespace

void ConstructionGraph::Build(const ConstructionDef& def, const DamageState* damage) {
    nodes_.clear();
    edgeCount_ = 0;
    nodes_.reserve(def.components.size());

    for (const ConstructionComponent& c : def.components) {
        Node n;
        n.id = c.id;
        n.role = c.role;
        n.parent = c.parent;
        n.destroyed = damage && damage->IsDestroyed(c.id);
        nodes_.emplace(c.id, std::move(n));
    }

    for (const ConstructionComponent& c : def.components) {
        if (c.parent == kInvalidComponent) continue;
        auto p = nodes_.find(c.parent);
        if (p != nodes_.end()) p->second.children.push_back(c.id);
    }

    for (const SupportEdge& e : def.edges) {
        if (damage && damage->IsEdgeBroken(e.supported, e.supporter)) continue;
        auto sd = nodes_.find(e.supported);
        auto sr = nodes_.find(e.supporter);
        if (sd == nodes_.end() || sr == nodes_.end()) continue; // Validate reports these
        sd->second.supporters.push_back(Link{e.supporter, e.kind, e.capacity});
        sr->second.supported.push_back(Link{e.supported, e.kind, e.capacity});
        ++edgeCount_;
    }
}

const ConstructionGraph::Node* ConstructionGraph::Get(ComponentId id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

bool ConstructionGraph::Exists(ComponentId id) const {
    const Node* n = Get(id);
    return n && !n->destroyed;
}

std::vector<ComponentId> ConstructionGraph::Supporters(ComponentId id) const {
    std::vector<ComponentId> out;
    const Node* n = Get(id);
    if (!n) return out;
    for (const Link& l : n->supporters)
        if (CarriesLoad(l.kind) && Exists(l.other)) out.push_back(l.other);
    return out;
}

std::vector<ComponentId> ConstructionGraph::Supported(ComponentId id) const {
    std::vector<ComponentId> out;
    const Node* n = Get(id);
    if (!n) return out;
    for (const Link& l : n->supported)
        if (CarriesLoad(l.kind) && Exists(l.other)) out.push_back(l.other);
    return out;
}

std::vector<ComponentId> ConstructionGraph::Children(ComponentId id) const {
    const Node* n = Get(id);
    return n ? n->children : std::vector<ComponentId>{};
}

std::vector<ComponentId> ConstructionGraph::Dependents(ComponentId id) const {
    std::vector<ComponentId> out;
    if (!Exists(id)) return out;
    // Iterative, not recursive: a tall building's dependency chain is deep enough that recursion
    // is a stack-depth question nobody wants to answer at runtime.
    std::vector<ComponentId> stack{id};
    std::vector<ComponentId> seen{id};
    while (!stack.empty()) {
        const ComponentId cur = stack.back();
        stack.pop_back();
        for (ComponentId up : Supported(cur)) {
            if (std::find(seen.begin(), seen.end(), up) != seen.end()) continue;
            seen.push_back(up);
            stack.push_back(up);
            out.push_back(up);
        }
    }
    return out;
}

bool ConstructionGraph::AnchoredWith(ComponentId id, const std::vector<ComponentId>* blocked) const {
    auto isBlocked = [blocked](ComponentId c) {
        return blocked && std::find(blocked->begin(), blocked->end(), c) != blocked->end();
    };
    if (isBlocked(id)) return false;
    const Node* start = Get(id);
    if (!start || start->destroyed) return false;
    if (start->role == StructuralRole::Foundation) return true;

    // Reachability to any foundation, downward through load-bearing edges. This is deliberately
    // NOT statics - no moments, no shear, no material strength. The brief asks for a game-oriented
    // structural relationship system, and "is there a load path to the ground" is exactly that.
    std::vector<ComponentId> stack{id};
    std::vector<ComponentId> seen{id};
    while (!stack.empty()) {
        const ComponentId cur = stack.back();
        stack.pop_back();
        const Node* n = Get(cur);
        if (!n) continue;
        // Evaluated per CURRENT node, not once for the walk: the siding reaches its wall by
        // attachment, and the wall then reaches ground by load. Each hop answers for itself.
        for (const Link& l : n->supporters) {
            if (!HoldsUp(l.kind, n->role)) continue;
            if (isBlocked(l.other)) continue;
            const Node* s = Get(l.other);
            if (!s || s->destroyed) continue;
            if (s->role == StructuralRole::Foundation) return true;
            if (std::find(seen.begin(), seen.end(), l.other) != seen.end()) continue;
            seen.push_back(l.other);
            stack.push_back(l.other);
        }
    }
    return false;
}

bool ConstructionGraph::IsAnchored(ComponentId id) const { return AnchoredWith(id, nullptr); }

std::vector<ComponentId> ConstructionGraph::Unsupported() const {
    std::vector<ComponentId> out;
    for (const auto& [id, n] : nodes_) {
        if (n.destroyed) continue;
        if (n.role == StructuralRole::None) continue; // decorative: never "unsupported structure"
        if (!IsAnchored(id)) out.push_back(id);
    }
    std::sort(out.begin(), out.end()); // deterministic order - nodes_ is a hash map
    return out;
}

std::vector<ComponentId> ConstructionGraph::UnsupportedIfRemoved(
    const std::vector<ComponentId>& removed) const {
    std::vector<ComponentId> out;
    for (const auto& [id, n] : nodes_) {
        if (n.destroyed) continue;
        if (n.role == StructuralRole::None) continue;
        if (std::find(removed.begin(), removed.end(), id) != removed.end()) continue;
        // Only report a CHANGE. Something already unsupported is not news, and reporting it would
        // make an editor preview light up the whole building the moment anything is wrong.
        if (IsAnchored(id) && !AnchoredWith(id, &removed)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

f32 ConstructionGraph::Integrity(ComponentId id) const {
    const Node* n = Get(id);
    if (!n || n->destroyed) return 0.0f;
    if (n->role == StructuralRole::Foundation) return 1.0f;

    f32 total = 0.0f, surviving = 0.0f;
    for (const Link& l : n->supporters) {
        // Same rule as anchoring: cladding's integrity is a question about its attachment, so
        // judging it by load alone would report every intact panel in the building at zero.
        if (!HoldsUp(l.kind, n->role)) continue;
        const f32 cap = l.capacity > 0.0f ? l.capacity : 0.0f;
        total += cap;
        if (Exists(l.other)) surviving += cap;
    }
    // Normalised against AUTHORED capacity rather than trusting it to sum to 1 - authoring will
    // not be careful, and an artist writing three supports of 1.0 each means "three equal
    // supports", not "300%".
    if (total <= 0.0f) return 0.0f;
    return std::min(1.0f, surviving / total);
}

} // namespace hbe::construction

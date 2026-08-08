// Construction/ConstructionGraph.h - the structural relationship graph.
//
// WHY THIS IS A NEW CONCEPT AND NOT BUILT ON SOMETHING EXISTING. The engine has exactly two
// relational mechanisms and neither can express this:
//
//   * The scene's ONLY relational field is `Parent` (EntityData::parent, one int). One edge per
//     entity cannot say "this beam is supported by two walls".
//   * The only load-path solver in the tree is
//     `destruction::ComputeSupport(const FractureAsset&, ...)` - it takes a BAKED FRACTURE ASSET,
//     with adjacency living inside the .hbfrac, and fracturing is explicitly offline-only
//     (Fracture.h:1-6). It cannot answer anything about a building that has not been fractured.
//
// So the graph is native to the construction definition, and FRACTURE BECOMES A CONSUMER OF THE
// GRAPH RATHER THAN ITS OWNER. That inversion is the point: it lets structural questions be
// asked of an authored, intact, un-fractured building - which is what the editor needs at
// authoring time and what a future destruction pass needs at the moment of the first impact.
//
// SCOPE. This is a game-oriented reachability model, not an engineering solver, exactly as the
// brief asks. "Is there a load path to a foundation" is a graph reachability question; it is not
// statics, and it deliberately does not model moments, shear or material strength.
#pragma once

#include "Construction/ConstructionDef.h"

#include <unordered_map>
#include <vector>

namespace hbe::construction {

// A resolved, queryable view over a ConstructionDef + its DamageState.
//
// Build it once, query it many times. Rebuild it when the definition changes - it holds no
// pointers into the definition beyond the build call, so a rebuilt graph is always consistent.
class ConstructionGraph {
public:
    // `damage` may be null, meaning "intact". The graph copies what it needs; the caller may
    // destroy either argument afterwards.
    void Build(const ConstructionDef& def, const DamageState* damage = nullptr);

    // -- Relationship queries (brief SS10) -----------------------------------
    // All of these ignore destroyed components and broken edges, so they answer questions about
    // the CURRENT state of the structure rather than its authored state.

    // Who holds this component up (one hop).
    std::vector<ComponentId> Supporters(ComponentId id) const;
    // What this component holds up (one hop).
    std::vector<ComponentId> Supported(ComponentId id) const;
    // Everything that transitively rests on this component. This is "which components are
    // structurally affected if this one is removed" in its simplest form.
    std::vector<ComponentId> Dependents(ComponentId id) const;

    // Does a load path from this component reach a StructuralRole::Foundation?
    //
    // THIS IS THE WHOLE INTEGRITY MODEL. A component with no path to ground is unsupported, and
    // unsupported is what eventually triggers a destruction event. Components with role
    // Foundation are anchors and are trivially anchored.
    bool IsAnchored(ComponentId id) const;

    // Every component that still exists but has lost every load path to a foundation.
    std::vector<ComponentId> Unsupported() const;

    // THE QUERY THAT MATTERS (brief SS12). If `removed` were destroyed, which components would
    // lose their last load path? Does NOT mutate the graph - it answers a hypothetical, which is
    // what an editor preview and a damage pass both need before committing.
    //
    // Returns components that are currently anchored and would stop being anchored. The removed
    // components themselves are not included.
    std::vector<ComponentId> UnsupportedIfRemoved(const std::vector<ComponentId>& removed) const;

    // 0..1. The surviving share of a component's authored support capacity. 1 = fully supported,
    // 0 = nothing left holding it up. A Foundation is always 1.
    f32 Integrity(ComponentId id) const;

    // Containment (the `parent` field), NOT support. Separate on purpose.
    std::vector<ComponentId> Children(ComponentId id) const;

    bool Exists(ComponentId id) const;      // present in the definition AND not destroyed
    usize NodeCount() const { return nodes_.size(); }
    usize EdgeCount() const { return edgeCount_; }

private:
    struct Link {
        ComponentId other = kInvalidComponent;
        EdgeKind kind = EdgeKind::Bears;
        f32 capacity = 1.0f;
    };
    struct Node {
        ComponentId id = kInvalidComponent;
        StructuralRole role = StructuralRole::None;
        ComponentId parent = kInvalidComponent;
        bool destroyed = false;
        std::vector<Link> supporters; // edges where THIS is `supported`
        std::vector<Link> supported;  // edges where THIS is `supporter`
        std::vector<ComponentId> children;
    };

    const Node* Get(ComponentId id) const;

    // Shared reachability core. `blocked` components are treated as destroyed for this walk,
    // which is what makes UnsupportedIfRemoved a hypothetical rather than a mutation.
    bool AnchoredWith(ComponentId id, const std::vector<ComponentId>* blocked) const;

    std::unordered_map<ComponentId, Node> nodes_;
    usize edgeCount_ = 0;
};

// --test-construction. Covers determinism, seed independence, the graph queries and the damage
// model. Headless; no project, no window, no GPU.
bool SelfTest();

} // namespace hbe::construction

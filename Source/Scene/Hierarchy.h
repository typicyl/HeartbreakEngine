// Scene/Hierarchy.h - THE SIBLING-ORDER CONTRACT, and the one parent->children walk.
//
// THE CONTRACT, stated once:
//
//   The children of an entity are ordered by their HierarchyOrder, and that order
//   is the SAME everywhere - in the Hierarchy panel, in a saved `.hbscene`, in a
//   clipboard fragment, in a `.hbprefab`, and in whatever a paste produces.
//
// Before this file there were FOUR different answers to "what order are these
// siblings in", none of them stable:
//
//   * the Hierarchy panel sorted by the raw `entt::entity` value, which is
//     `index | (version << 20)` - so one delete anywhere in the session made every
//     recycled handle sort above every fresh one, and a Ctrl+D drew its whole
//     subtree from the free list and displayed it scrambled;
//   * BuildSubtreeJson emitted Parent-POOL insertion order (its LIFO stack and
//     entt's reverse-iterating view cancel out), which a single component erase
//     permutes via swap_and_pop;
//   * BuildSceneJson emitted entity-STORAGE order, which `swap_only` permutes on
//     every destroy - so deleting one unrelated object silently reordered siblings
//     in the file on disk.
//
// A FOURTH ANSWER IS STILL LIVE AND IS DELIBERATELY NOT THIS ONE: ui::BuildChildrenMap
// (UI/UISystem.cpp) feeds UILayoutGroup raw reverse pool order. That is not an
// oversight to be tidied up here - `.hbui` element order IS the pool order, by
// construction: Editor::UISwapOrder permutes UIElement/Parent pool positions and the
// DocumentInstance's saved list together, and CaptureDocument writes that list.
// HierarchyOrder is not consulted anywhere in a document's z-order or hit order, which
// is exactly why the Hierarchy panel REFUSES a drag-reorder on a document row instead
// of performing one that would silently revert on the next save.
//
// HierarchyOrder (Scene/Components.h) replaces the three above with authored data. It
// is minted monotonically by Scene::CreateEntity, serialized as "order", and
// renumbered densely inside one sibling group by ReorderSibling. Values are only
// ever COMPARED, and only against siblings.
//
// THE SECOND HALF of this header is performance. Every subtree walk in the tree was
// a full `view<Parent>` scan PER VISITED NODE - O(N x P) against the WORLD-WIDE
// Parent pool, not the subtree. Measured on the vendored entt: 1000 nodes = 5 ms (a
// dropped frame at 120 fps on a Ctrl+C), 4000 = 79 ms, 16000 = 1.26 SECONDS. The
// one-pass map below is linear and ~970x faster at 16k. It also gives the ordering
// rule a single place to live, which is why the two live in one header.
#pragma once

#include "Scene/Components.h"

#include <entt/entt.hpp>

#include <unordered_map>
#include <utility>
#include <vector>

namespace hbe {

class Scene;

namespace scene {

// parent (as u32 bits) -> its children, already sorted by the rule below. Keyed on
// the raw bits rather than entt::entity because that is what every existing caller
// (Editor::childrenByParent_) already stores.
using ChildrenMap = std::unordered_map<u32, std::vector<entt::entity>>;

// The sort key: (HierarchyOrder, entt::to_entity). `to_entity` STRIPS THE VERSION -
// sorting by the raw handle is the exact bug this file exists to remove - and is
// only ever a tiebreak for entities that carry no HierarchyOrder at all (the three
// transient runtime-UI entities Engine.cpp creates with a bare reg.create()). Those
// sort LAST, which keeps them out of authored content's way.
std::pair<i32, u32> OrderKey(const entt::registry& reg, entt::entity e);
bool OrderLess(const entt::registry& reg, entt::entity a, entt::entity b);

// Sorts a sibling list in place by OrderKey.
void SortSiblings(const entt::registry& reg, std::vector<entt::entity>& v);

// ONE pass over view<Parent>, bucketed by parent, each bucket sorted.
//
// NOTE FOR ANYONE TEMPTED TO SKIP THE SORT: entt views iterate a pool in REVERSE
// insertion order, so an unsorted one-pass bucket build yields children BACKWARDS.
// That is not a theoretical hazard - it is the exact reversal the old LIFO walk was
// accidentally cancelling out, and dropping the sort would introduce it at every
// call site at once.
//
// `keep` (optional) filters children BEFORE the sort - the Hierarchy panel uses it
// to drop system-generated terrain chunks, which on a big terrain would otherwise be
// the largest bucket in the map and re-sorted every frame for nothing.
using ChildFilter = bool (*)(const entt::registry&, entt::entity); // true = keep
void BuildChildrenMap(const entt::registry& reg, ChildrenMap& out,
                      ChildFilter keep = nullptr);
ChildrenMap BuildChildrenMap(const entt::registry& reg);

// Children of one entity, in order. For the callers that touch a single subtree and
// would rather not build a whole map (it is still one pool pass, not one per node).
std::vector<entt::entity> ChildrenOf(const entt::registry& reg, entt::entity parent);

// `root` and every descendant, pre-order depth-first, parents before children and
// siblings in order. This is the order a subtree serializes and pastes in.
std::vector<entt::entity> SubtreeInOrder(const entt::registry& reg, entt::entity root);
// Same walk against an already-built map (for callers with several subtrees).
std::vector<entt::entity> SubtreeInOrder(const ChildrenMap& kids, entt::entity root);

// Moves `moved` to sit immediately before/after `target` among their SHARED parent's
// children, then renumbers that sibling group densely. No-op unless both are live
// and share a parent (roots share the null parent, which counts). Returns true when
// something moved.
//
// Dense renumbering is deliberate: values are compared only within a group, so
// collapsing a group to 0..n-1 is free, and it keeps "order" small and diffable in
// the file. It also cannot starve the way gapped/float indices eventually do.
bool ReorderSibling(entt::registry& reg, entt::entity moved, entt::entity target,
                    bool before);

// --test-pasteorder: the headless proof of the contract at the top of this file.
// Copy/duplicate/prefab round-trips reproduce child order at every depth, deep and
// wide, clones still mint fresh guids, and the ORDER SURVIVES a registry whose pool
// order has been perturbed by deletes (the case the old walk fails).
bool PasteOrderSelfTest();

} // namespace scene
} // namespace hbe

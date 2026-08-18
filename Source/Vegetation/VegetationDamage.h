// Vegetation/VegetationDamage.h - damage, health + structural support (P10).
//
// Damage is a STATE change on the store, not a physics event: a tree carries health, a
// branch can be severed, and structural-support flood-fill decides what a break orphans
// (a limb whose parent is gone falls). This mirrors Game/DestructionSystem's INTACT ->
// BREAKING support model without a body per plant - only an actively falling limb would
// ever get a transient physics body (deferred). Pure CPU + deterministic.
#pragma once

#include "Vegetation/VegetationTypes.h"

#include <vector>

namespace hbe::veg {

struct VegetationStore;

// Reduces a tree's health by `amount` (result clamped to [0,1]); returns the new health.
// Health 0 = dead (ready to be felled / removed by the caller).
f32 DamageTree(VegetationStore& store, TreeId tree, f32 amount);

// Severs one branch (global index): descendants that reach the ground only through it lose
// support on the next ComputeBranchSupport.
void BreakBranch(VegetationStore& store, BranchId branch);

// Recomputes which of a tree's branches are structurally supported (reachable from a
// ground-connected base through unbroken parents). Returns the count of FALLEN branches;
// `outSupported`, if given, receives a per-branch flag over the tree's branch slice.
u32 ComputeBranchSupport(const VegetationStore& store, TreeId tree,
                         std::vector<u8>* outSupported = nullptr);

// --test-veglife: pins P10 life-cycle - incremental growth ADDS structure while preserving
// the earlier form, and damage/support (a break orphans its descendants). Headless.
bool LifeSelfTest();

} // namespace hbe::veg

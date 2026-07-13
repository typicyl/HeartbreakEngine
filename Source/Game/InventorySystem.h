// Game/InventorySystem.h - runtime inventory + item catalog + crafting.
//
// The inventory is a per-playthrough runtime singleton in game:: (like objectives
// and story flags): item id -> count plus the selected weapon, saved into .hbsave
// and cleared on new game. World pickups grant items via the Interactable GrantItem
// action; crafting turns recipe inputs into an output. Implemented in T1-Inventory.
#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace hbe::game {

// Filled in the inventory phase.
void ResetInventory();

} // namespace hbe::game

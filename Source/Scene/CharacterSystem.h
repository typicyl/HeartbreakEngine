// Scene/CharacterSystem.h - runtime assembly + outfit swapping for modular characters.
//
// A Character root entity (Scene/Components.h Character) references a .hbchar. This
// system loads + seam-welds it (cached process-wide), sets the root up to POSE the
// shared skeleton (Animator + MeshRef, no drawable MeshInstance), and spawns the
// active loadout's parts as child entities that BORROW the root's palette
// (SkinnedPartRef). Swapping an outfit = respawning parts; seams stay solid because
// every welded variant shares the canonical seam binding.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <string>

namespace hbe {

class Scene;
class Renderer;

namespace character {

// (Re)assembles the Character root `root`: ensures its Animator + skeleton MeshRef,
// loads/welds its .hbchar (cached), and spawns the active loadout's parts. Safe to
// call repeatedly (clears existing parts first). No-op without a Character component.
void Instantiate(Scene& scene, Renderer& renderer, entt::entity root,
                 const std::filesystem::path& assetsDir);

// Applies a named loadout (unknown/empty -> each slot's default variant). Respawns
// all parts. Records the choice in the Character component.
void SetLoadout(Scene& scene, Renderer& renderer, entt::entity root,
                const std::filesystem::path& assetsDir, const std::string& loadout);

// Sets one slot's active variant ("" hides the slot). Respawns just that slot.
void SetSlotVariant(Scene& scene, Renderer& renderer, entt::entity root,
                    const std::filesystem::path& assetsDir, const std::string& slot,
                    const std::string& variant);

// Destroys every live part child entity of `root` (leaves the root + its Animator).
void ClearParts(Scene& scene, entt::entity root);

// Drops the process-wide built-character cache (project switch / .hbchar re-saved).
void ClearCache();

} // namespace character
} // namespace hbe

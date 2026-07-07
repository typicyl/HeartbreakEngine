// Assets/CharacterAsset.h - .hbchar modular-character assets.
//
// A .hbchar is a small JSON file under the project's Assets/ describing a
// MODULAR CHARACTER: one shared skeleton (a .uaf rig) + a set of body-part SLOTS
// (head, torso, upperArmL, ...), one or more swappable VARIANTS per slot (each a
// skinned mesh part, e.g. bare / hoodie / armored), and named LOADOUTS that pick
// one variant per slot. Outfits are swapped by enabling/disabling parts.
//
// Seams between parts are made SOLID by a build step (see Assets/SeamWeld): it
// canonicalizes the boundary vertices shared by adjacent slots so every variant
// skins bit-identically at the seam. Because the weld is per slot-adjacency (not
// per-loadout), any combination of variants is automatically gap-free.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

// How a variant's boundary connects to its neighbours:
//  - Continuous: bare skin meeting bare skin (arm<->torso) - weld position AND
//    normal/tangent so the seam is invisible (no lighting discontinuity).
//  - Overlap: fabric/armor sitting over the body (a sleeve cuff over a wrist) -
//    weld position + skin binding only; keep per-part normals (a real edge).
enum class SeamMode : u8 { Continuous, Overlap };

struct CharacterVariant {
    std::string id;                       // unique variant id (loadouts reference this)
    std::string slot;                     // slot name this variant fills
    std::string mesh;                     // "uaf:<rel>#<n>" skinned part mesh
    std::string material;                 // .hbmat rel path ("" = the part's own material)
    SeamMode seamMode = SeamMode::Continuous;
    bool isDefault = false;               // the slot's default variant (initial loadout)
};

struct CharacterSlot {
    std::string name;                     // e.g. "torso", "upperArmL"
    // Adjacent slots this one shares a seam with (informational / drives the
    // editor overlay; the weld itself groups by geometric proximity across ALL
    // variants, so neighbours need not be exhaustive here).
    std::vector<std::string> seamNeighbors;
};

struct CharacterLoadout {
    std::string name;                     // e.g. "casual", "winter"
    // slot name -> variant id ("" or absent = slot empty this loadout).
    std::unordered_map<std::string, std::string> slots;
};

struct CharacterAsset {
    std::string skeleton;                 // "uaf:<rel>#<n>" skeleton + shared clips source
    std::vector<CharacterSlot> slots;
    std::vector<CharacterVariant> variants;
    std::vector<CharacterLoadout> loadouts;
    std::string weldCache;                // derived .hbcharcache rel path ("" = none yet)

    const CharacterVariant* FindVariant(const std::string& id) const {
        for (const CharacterVariant& v : variants)
            if (v.id == id) return &v;
        return nullptr;
    }
    const CharacterSlot* FindSlot(const std::string& name) const {
        for (const CharacterSlot& s : slots)
            if (s.name == name) return &s;
        return nullptr;
    }
    const CharacterLoadout* FindLoadout(const std::string& name) const {
        for (const CharacterLoadout& l : loadouts)
            if (l.name == name) return &l;
        return nullptr;
    }
    // The default variant id for `slot` (isDefault, else first, else ""), used to
    // build the initial loadout when none is named.
    std::string DefaultVariant(const std::string& slot) const {
        std::string first;
        for (const CharacterVariant& v : variants) {
            if (v.slot != slot) continue;
            if (v.isDefault) return v.id;
            if (first.empty()) first = v.id;
        }
        return first;
    }
};

namespace assets {

inline constexpr const char* kCharacterExtension = ".hbchar";

bool SaveCharacter(const std::filesystem::path& path, const CharacterAsset& c);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<CharacterAsset> LoadCharacter(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe

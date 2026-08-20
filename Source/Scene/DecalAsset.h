// Scene/DecalAsset.h - the `.hbdecal` reusable DECAL ASSET (a decal library / preset).
//
// Mirrors `.hbvfx` for particles: a `.hbdecal` is the AUTHORED half of a DecalComponent (box size,
// opacity, angle fade, channel-influence flags, projection cone, emissive, and the albedo/normal/MR
// texture refs) as a standalone versioned JSON asset, so an artist can save a "blood splat" / "grime"
// / "bullet impact" decal once and drop it anywhere from a library. Runtime state (resolved bindless
// handles) is never serialized. The field (de)serialization is the SINGLE source of truth: the scene
// serializer's inline `decal` block delegates to it (DecalAssetJson.h), so the two formats never drift.
#pragma once

#include "Scene/Components.h" // DecalComponent

#include <filesystem>
#include <optional>
#include <string>

// nlohmann-FREE (the executables link the engine lib with nlohmann PRIVATE): the JSON field mapping
// lives in DecalAssetJson.h, included only inside the engine library.
namespace hbe::decalasset {

inline constexpr const char* kDecalExtension = ".hbdecal";
inline constexpr int kDecalVersion = 1;

std::string DecalToString(const DecalComponent& d);
std::optional<DecalComponent> DecalFromString(const std::string& text);
bool SaveDecal(const std::filesystem::path& path, const DecalComponent& d);
std::optional<DecalComponent> LoadDecal(const std::filesystem::path& path);

// Headless round-trip self-test (`--test-hbdecal`).
bool SelfTest();

} // namespace hbe::decalasset

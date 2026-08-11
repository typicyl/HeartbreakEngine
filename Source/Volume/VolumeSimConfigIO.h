// Volume/VolumeSimConfigIO.h - read/write the `.hbvolsim` authoring asset.
//
// A `.hbvolsim` is a JSON file under Assets/ that stores a VolumeSimConfig verbatim: the
// model id, domain, timeline, physics coefficients, emitters/obstacles (with their
// data-driven curves), the bake-field list, and the open model-params bag. It is the
// EDITOR's source of truth for a volume; the offline baker turns it into a `.hbvol` that
// the shipped game streams. The runtime never reads `.hbvolsim` - only the editor and any
// headless bake tool do (that is the sim/runtime independence guarantee).
#pragma once

#include "Volume/VolumeSimConfig.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>

namespace hbe::volume {

inline constexpr const char* kVolumeSimExtension = ".hbvolsim";

// In-memory JSON <-> config. These are the core; the file IO below wraps them. The scene serializer
// also uses them to embed a VolumeSimConfig INLINE on a VolumeComponent (the config lives on the
// placed volume entity, not only in a separate .hbvolsim asset).
nlohmann::json ConfigToJson(const VolumeSimConfig& config);
// Fill `out` from `j`. Returns false (leaving `out` unchanged) on a malformed object; NEVER throws
// (a present-but-wrong-typed key degrades to the field default rather than an uncaught type_error).
// Missing fields fall back to VolumeSimConfig's defaults, so an older/partial object still loads.
bool ConfigFromJson(const nlohmann::json& j, VolumeSimConfig& out);

// Serialize `config` to `path` as pretty JSON. Returns false on write failure.
bool SaveVolumeSimConfig(const std::filesystem::path& path, const VolumeSimConfig& config);

// Parse `path` into `out`. Returns false (leaving `out` untouched) if the file is missing or malformed.
bool LoadVolumeSimConfig(const std::filesystem::path& path, VolumeSimConfig& out);

} // namespace hbe::volume

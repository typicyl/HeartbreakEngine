// Vegetation/BiomeAsset.h - .hbbiome load/save (data-driven distribution rules).
//
// A biome references species BY NAME; loading resolves those names against a
// SpeciesRegistry (rules naming an un-interned species are dropped with a warning).
// Uses the VFS read path (never std::ifstream) so it works identically in loose editor
// builds and packed shipping builds. JSON, versioned for forward-compat.
#pragma once

#include "Vegetation/Species.h"

#include <filesystem>
#include <string>

namespace hbe::veg {

class SpeciesRegistry;

// Parse a .hbbiome from an in-memory JSON string (no file IO; used by the loader and by
// tests). Returns false on a parse error; drops rules that name an un-interned species.
bool ParseBiomeJson(const std::string& text, const SpeciesRegistry& reg, Biome& out);

// Load a .hbbiome through the VFS. Returns false (and logs) on read/parse failure.
bool LoadBiome(const std::filesystem::path& path, const SpeciesRegistry& reg, Biome& out);

// Serialize a biome to a .hbbiome (species written by name). Returns false on IO failure.
bool SaveBiome(const std::filesystem::path& path, const Biome& b, const SpeciesRegistry& reg);

// The JSON text for a biome (species by name). Exposed for the round-trip self-test.
std::string BiomeToJson(const Biome& b, const SpeciesRegistry& reg);

} // namespace hbe::veg

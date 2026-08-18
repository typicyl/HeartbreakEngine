// Vegetation/SpeciesAsset.h - .hbspecies load/save (the data-driven species record).
//
// JSON, versioned, via the VFS read path (works identically loose + packed). No species
// is hardcoded in C++: a .hbspecies fully describes a plant's structure, foliage,
// environment preferences, dynamics and asset references.
#pragma once

#include "Vegetation/Species.h"

#include <filesystem>
#include <string>

namespace hbe::veg {

// Parse a .hbspecies from an in-memory JSON string (no file IO; used by the loader and by
// tests). Returns false on a parse error.
bool ParseSpeciesJson(const std::string& text, Species& out);

// Load a .hbspecies through the VFS. Returns false (and logs) on read/parse failure.
bool LoadSpecies(const std::filesystem::path& path, Species& out);

// Serialize a species to a .hbspecies. Returns false on IO failure.
bool SaveSpecies(const std::filesystem::path& path, const Species& s);

// The JSON text for a species. Exposed for the round-trip self-test.
std::string SpeciesToJson(const Species& s);

} // namespace hbe::veg

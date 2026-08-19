// Cinematics/SequenceAsset.h - .hbseq serialization (versioned JSON).
//
// The persistent form of a cinematic Sequence. JSON via nlohmann (the
// CutsceneAsset.cpp idiom): a top-level `version`, guids as 16-char hex strings,
// actors referenced by guid/name, assets by relative path, nested sequences by
// path. Enums serialize as ints, append-only. Reads go through vfs::ReadFile so a
// packed/shipped build loads .hbseq from the pack. Registered as a runtime-loaded
// engine asset in Assets/AssetFormats.cpp (RefScan::JsonScan pulls referenced
// dialogue/voiceline/mesh/sub-sequence assets into the shipping closure).
#pragma once

#include "Cinematics/Sequence.h"

#include <filesystem>
#include <optional>
#include <string>

namespace hbe::cine {

inline constexpr const char* kSequenceExtension = ".hbseq";
inline constexpr int kSequenceVersion = 1;

bool SaveSequence(const std::filesystem::path& path, const Sequence& seq);
std::optional<Sequence> LoadSequence(const std::filesystem::path& path);

// String round-trip (undo snapshots, embedding, tests). ToJson is pretty-printed.
std::string ToJson(const Sequence& seq);
std::optional<Sequence> FromJson(const std::string& json, const std::string& sourceLabel = "<memory>");

} // namespace hbe::cine

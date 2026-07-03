// Assets/DialogueAsset.h - .hbdialogue conversation assets.
//
// A dialogue is a small JSON file under the project's Assets/ directory: an
// ordered list of spoken lines, each with a speaker name, a caption, an
// optional `.uaf` voiceline clip, and a hold time. A schematic PlayDialogue
// node (or game code) runs it: each line shows its "Speaker: text" caption
// (which stacks with others) and plays its clip, advancing after the hold.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

struct DialogueLine {
    std::string speaker;     // character name (shown as "Speaker: text")
    std::string text;        // caption line; empty = fall back to the clip's baked caption
    std::string clip;        // optional `.uaf` Voiceline (relative to Assets/)
    f32 hold = 0.0f;         // seconds to hold before the next line (0 = auto from text)
};

struct DialogueAsset {
    std::vector<DialogueLine> lines;
};

namespace assets {

inline constexpr const char* kDialogueExtension = ".hbdialogue";

bool SaveDialogue(const std::filesystem::path& path, const DialogueAsset& d);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<DialogueAsset> LoadDialogue(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe

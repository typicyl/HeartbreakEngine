// Assets/DialogueAsset.cpp
#include "Assets/DialogueAsset.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hbe::assets {

using json = nlohmann::json;

bool SaveDialogue(const std::filesystem::path& path, const DialogueAsset& d) {
    json j;
    j["type"] = "dialogue";
    j["version"] = 1;
    json lines = json::array();
    for (const DialogueLine& l : d.lines) {
        lines.push_back({{"speaker", l.speaker},
                         {"text", l.text},
                         {"clip", l.clip},
                         {"hold", l.hold}});
    }
    j["lines"] = std::move(lines);

    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Dialogue: cannot write '{}'.", path.string());
        return false;
    }
    out << j.dump(4);
    return true;
}

std::optional<DialogueAsset> LoadDialogue(const std::filesystem::path& path) {
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Dialogue: cannot read '{}'.", path.string());
        return std::nullopt;
    }

    json j;
    try {
        j = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Dialogue: failed to parse '{}': {}", path.string(), e.what());
        return std::nullopt;
    }

    DialogueAsset d;
    if (const auto it = j.find("lines"); it != j.end() && it->is_array()) {
        for (const json& jl : *it) {
            DialogueLine l;
            l.speaker = jl.value("speaker", "");
            l.text = jl.value("text", "");
            l.clip = jl.value("clip", "");
            l.hold = jl.value("hold", 0.0f);
            d.lines.push_back(std::move(l));
        }
    }
    return d;
}

} // namespace hbe::assets

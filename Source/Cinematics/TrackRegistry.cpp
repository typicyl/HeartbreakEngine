// Cinematics/TrackRegistry.cpp - global track-kind table + built-in registration.
#include "Cinematics/TrackRegistry.h"

#include "Core/Log.h"

namespace hbe::cine {
namespace {
std::vector<TrackKind>& Table() {
    static std::vector<TrackKind> t;
    return t;
}
} // namespace

void RegisterTrackKind(const TrackKind& kind) {
    auto& t = Table();
    for (auto& k : t) {
        if (k.id == kind.id) { k = kind; return; } // replace in place
    }
    t.push_back(kind);
}

const TrackKind* FindTrackKind(const std::string& id) {
    for (const auto& k : Table())
        if (k.id == id) return &k;
    return nullptr;
}

const std::vector<TrackKind>& TrackKinds() { return Table(); }

// Defined in Tracks.cpp - registers every built-in kind into the table.
void RegisterBuiltinTrackKindsImpl();

void RegisterBuiltinTrackKinds() {
    static bool done = false;
    if (done) return;
    done = true;
    RegisterBuiltinTrackKindsImpl();
    HBE_INFO("Cinematics: registered {} track kinds", TrackKinds().size());
}

} // namespace hbe::cine

// Cinematics/Sequence.cpp - small value helpers for the sequence data model.
#include "Cinematics/Sequence.h"

#include <algorithm>

namespace hbe::cine {

f32 Section::Weight(f32 localT) const {
    if (localT < start || localT > End()) return 0.0f;
    f32 w = 1.0f;
    if (blendIn > 1e-5f && localT < start + blendIn) {
        w *= ease::Ease(blendEase, (localT - start) / blendIn);
    }
    if (blendOut > 1e-5f && localT > End() - blendOut) {
        w *= ease::Ease(blendEase, (End() - localT) / blendOut);
    }
    return std::clamp(w, 0.0f, 1.0f);
}

f32 Track::FloatParam(const std::string& key, f32 fallback) const {
    for (const auto& p : floatParams)
        if (p.first == key) return p.second;
    return fallback;
}

const std::string& Track::StringParam(const std::string& key, const std::string& fallback) const {
    for (const auto& p : stringParams)
        if (p.first == key) return p.second;
    return fallback;
}

f32 Sequence::Length() const {
    f32 len = duration;
    for (const auto& s : shots) len = std::max(len, s.End());
    return len;
}

const Binding* Sequence::FindBinding(int id) const {
    for (const auto& b : bindings)
        if (b.id == id) return &b;
    return nullptr;
}

Binding* Sequence::FindBinding(int id) {
    for (auto& b : bindings)
        if (b.id == id) return &b;
    return nullptr;
}

namespace {
bool TrackTreeHasCamera(const Track& t) {
    if (t.kind == "camera" || t.kind == "cameraCut") return true;
    for (const auto& c : t.children)
        if (TrackTreeHasCamera(c)) return true;
    return false;
}
} // namespace

bool HasCameraTrack(const Sequence& seq) {
    for (const auto& t : seq.tracks)
        if (TrackTreeHasCamera(t)) return true;
    return false;
}

} // namespace hbe::cine

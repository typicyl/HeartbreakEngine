// Assets/AcousticMaterial.cpp - the acoustic-material PRESET LIBRARY is SOURCED FROM the
// HDS-Resonance acoustics library (hdsr/acoustics.h). Heartbreak owns only the engine-side data
// representation (the AcousticMaterial struct, used by .hbmat serialization) and adapts the
// library's API shape; the acoustic MODEL - material tables, room acoustics, propagation - lives
// in the library so it is reusable by any engine. When the library is not built in
// (HBE_HAVE_RESONANCE=0) the preset list is empty and callers fall back to defaults.
#include "Assets/AcousticMaterial.h"

#if HBE_HAVE_RESONANCE
#include "hdsr/acoustics.h"
#endif

namespace hbe {

#if HBE_HAVE_RESONANCE
namespace {
// Translate a library material into Heartbreak's engine-side representation.
AcousticMaterial FromHdsr(const hdsr::AcousticMaterial& m) {
    AcousticMaterial out;
    for (int b = 0; b < kAcousticBands; ++b)
        out.absorption[static_cast<usize>(b)] = m.absorption[b];
    out.scattering = m.scattering;
    out.transmission = m.transmission;
    return out;
}
} // namespace
#endif

const std::vector<AcousticPreset>& AcousticPresets() {
    static const std::vector<AcousticPreset> kPresets = [] {
        std::vector<AcousticPreset> v;
#if HBE_HAVE_RESONANCE
        // Names are stable static strings owned by the library; the material is copied into the
        // engine representation.
        for (int i = 0; i < hdsr::MaterialCount(); ++i)
            v.push_back({hdsr::MaterialNameAt(i), FromHdsr(*hdsr::MaterialAt(i))});
#endif
        return v;
    }();
    return kPresets;
}

const AcousticMaterial* FindAcousticPreset(const std::string& name) {
    for (const AcousticPreset& p : AcousticPresets())
        if (name == p.name) return &p.material;
    return nullptr;
}

} // namespace hbe

// Assets/AcousticMaterial.cpp - the acoustic-material preset library.
#include "Assets/AcousticMaterial.h"

namespace hbe {

namespace {

// absorption bands are 31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k Hz. Values are drawn from
// common architectural-acoustics absorption tables (extended at the extreme low/high bands),
// with scattering + transmission authored for Heartbreak. Transmission is broadband: higher =
// more sound passes through the surface (drives occlusion / cross-room bleed).
const std::vector<AcousticPreset>& Table() {
    static const std::vector<AcousticPreset> kPresets = {
        {"Default",
         {{{0.10f, 0.10f, 0.10f, 0.11f, 0.12f, 0.13f, 0.14f, 0.15f, 0.16f}}, 0.20f, 0.12f}},
        // An opening (doorway/window gap): reflects nothing, passes everything.
        {"Open / Air",
         {{{1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f}}, 0.00f, 1.00f}},
        {"Concrete (sealed)",
         {{{0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f, 0.03f}}, 0.12f, 0.04f}},
        {"Brick (bare)",
         {{{0.02f, 0.02f, 0.03f, 0.03f, 0.03f, 0.04f, 0.05f, 0.07f, 0.07f}}, 0.20f, 0.05f}},
        {"Wood Panel",
         {{{0.19f, 0.19f, 0.28f, 0.22f, 0.17f, 0.09f, 0.10f, 0.11f, 0.11f}}, 0.20f, 0.15f}},
        {"Wood Floor",
         {{{0.10f, 0.10f, 0.15f, 0.11f, 0.10f, 0.07f, 0.06f, 0.07f, 0.07f}}, 0.15f, 0.10f}},
        {"Glass (window)",
         {{{0.30f, 0.30f, 0.35f, 0.25f, 0.18f, 0.12f, 0.07f, 0.05f, 0.05f}}, 0.05f, 0.22f}},
        {"Glass (thick)",
         {{{0.15f, 0.15f, 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f, 0.02f}}, 0.05f, 0.10f}},
        {"Metal",
         {{{0.02f, 0.02f, 0.02f, 0.03f, 0.03f, 0.03f, 0.04f, 0.04f, 0.05f}}, 0.10f, 0.08f}},
        {"Drywall / Sheetrock",
         {{{0.28f, 0.28f, 0.29f, 0.10f, 0.05f, 0.04f, 0.07f, 0.09f, 0.09f}}, 0.15f, 0.35f}},
        {"Plaster (rough)",
         {{{0.03f, 0.03f, 0.03f, 0.03f, 0.04f, 0.05f, 0.05f, 0.06f, 0.06f}}, 0.25f, 0.08f}},
        {"Acoustic Ceiling Tile",
         {{{0.40f, 0.45f, 0.50f, 0.55f, 0.65f, 0.75f, 0.80f, 0.85f, 0.85f}}, 0.35f, 0.40f}},
        {"Heavy Curtain",
         {{{0.10f, 0.12f, 0.14f, 0.35f, 0.55f, 0.72f, 0.70f, 0.65f, 0.65f}}, 0.45f, 0.55f}},
        {"Carpet",
         {{{0.02f, 0.03f, 0.03f, 0.06f, 0.14f, 0.37f, 0.60f, 0.65f, 0.65f}}, 0.30f, 0.20f}},
        {"Fiberglass Insulation",
         {{{0.20f, 0.35f, 0.50f, 0.70f, 0.85f, 0.95f, 0.98f, 0.98f, 0.98f}}, 0.50f, 0.55f}},
        {"Grass / Soil",
         {{{0.10f, 0.12f, 0.15f, 0.25f, 0.40f, 0.55f, 0.60f, 0.60f, 0.60f}}, 0.55f, 0.15f}},
        {"Water Surface",
         {{{0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f, 0.03f}}, 0.05f, 0.08f}},
        {"Marble / Polished Stone",
         {{{0.01f, 0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.02f}}, 0.05f, 0.04f}},
    };
    return kPresets;
}

} // namespace

const std::vector<AcousticPreset>& AcousticPresets() { return Table(); }

const AcousticMaterial* FindAcousticPreset(const std::string& name) {
    for (const AcousticPreset& p : Table())
        if (name == p.name) return &p.material;
    return nullptr;
}

} // namespace hbe

// Assets/AcousticMaterial.h - per-material acoustic properties for physically-informed audio.
//
// A small, engine-agnostic POD describing how a surface interacts with SOUND (independent of
// how it looks): how much energy it ABSORBS per octave band, how much it SCATTERS (diffuse vs
// specular reflection), and how much passes THROUGH it (transmission - the basis of occlusion
// and cross-room bleed). Stored on MaterialAsset (.hbmat) and resolved per surface at runtime
// (Audio/AcousticQuery). This is Heartbreak-side data; the spatial backend never sees this type
// - the integration layer translates it into the backend's room/source parameters.
//
// The 9 octave bands (31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k Hz) match the spatial audio
// backend's reverb band layout so per-band absorption maps 1:1 onto room reflection/reverb.
#pragma once

#include "Core/Types.h"

#include <array>
#include <string>
#include <vector>

namespace hbe {

// Octave-band count (31.25 Hz .. 8 kHz). Matches the Resonance reverb band count.
inline constexpr int kAcousticBands = 9;

struct AcousticMaterial {
    // Fraction of incident sound energy absorbed per octave band: 0 = perfect reflector,
    // 1 = fully absorbed (no reflection). Default = a generic hard-ish solid surface (most
    // hard surfaces absorb slightly more at high frequency).
    std::array<f32, kAcousticBands> absorption{
        {0.10f, 0.10f, 0.10f, 0.11f, 0.12f, 0.13f, 0.14f, 0.15f, 0.16f}};
    // Diffuse vs specular reflection: 0 = mirror-like, 1 = fully diffuse. Carried from P1;
    // consumed by later reflection modelling.
    f32 scattering = 0.20f;
    // Broadband fraction of energy passing THROUGH the surface: 0 = perfect blocker,
    // 1 = acoustically transparent. Drives occlusion / cross-room transmission. This is
    // Heartbreak's addition - the Resonance material model is absorption-only.
    f32 transmission = 0.12f;
};

// One named entry in the acoustic-material preset library.
struct AcousticPreset {
    const char* name;
    AcousticMaterial material;
};

// The preset library in stable order (index 0 is "Default", index 1 is "Open / Air").
// Broadly aligned to common architectural acoustics + the spatial backend's material set, plus
// a transmission value (which the backend's materials lack).
const std::vector<AcousticPreset>& AcousticPresets();

// Look up a preset's material by name (exact match). Returns nullptr when not found.
const AcousticMaterial* FindAcousticPreset(const std::string& name);

} // namespace hbe

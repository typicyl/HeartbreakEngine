// Scene/EffectAsset.h - the `.hbvfx` particle EFFECT ASSET (author once, reuse everywhere).
//
// The particle system is a rich module-stack simulation, but until now an effect could only exist
// as fields on a scene entity's ParticleEmitter - it could not be authored once, saved, shared,
// previewed standalone, or spawned by name. `.hbvfx` closes that gap: it is exactly the AUTHORED
// half of a ParticleEmitter (every serialized field, no runtime pool/GPU state) as a standalone
// versioned JSON asset. Gameplay says `game::SpawnEffect("Explosion", transform)`; the editor saves
// / loads / previews it; the cook/pack pipeline treats it like any other JSON asset.
//
// The field (de)serialization here is the SINGLE source of truth - SceneSerializer's inline
// ParticleEmitter block delegates to EmitterToJson/EmitterFromJson, so the scene format and the
// `.hbvfx` format can never drift, and every backward-compat default lives in one place.
#pragma once

#include "Scene/Components.h" // ParticleEmitter (the authored effect)

#include <filesystem>
#include <optional>
#include <string>

// NOTE: this header is deliberately nlohmann-FREE so the editor/runtime executables (which link the
// engine lib with nlohmann PRIVATE) can include it just for the std-typed API + SelfTest. The JSON
// field (de)serializers live in EffectAssetJson.h, included only inside the engine library.

namespace hbe::particle {

inline constexpr const char* kEffectExtension = ".hbvfx";
inline constexpr int kEffectVersion = 1;

// `.hbvfx` file (a `{version,type,emitter:{...}}` wrapper) <-> a ParticleEmitter.
std::string EffectToString(const ParticleEmitter& e);
std::optional<ParticleEmitter> EffectFromString(const std::string& text);
bool SaveEffect(const std::filesystem::path& path, const ParticleEmitter& e);
std::optional<ParticleEmitter> LoadEffect(const std::filesystem::path& path);

// Headless round-trip self-test (`--test-hbvfx`).
bool SelfTest();

} // namespace hbe::particle

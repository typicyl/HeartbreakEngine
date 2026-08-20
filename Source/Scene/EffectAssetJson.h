// Scene/EffectAssetJson.h - the ONE ParticleEmitter field (de)serializer (JSON).
//
// Split out of EffectAsset.h because it needs nlohmann, which the engine library links PRIVATE (so
// it must not appear in a header the executables include). Both the scene serializer and the
// `.hbvfx` asset delegate here, so the scene format and the effect-asset format can never drift and
// every backward-compat default lives in exactly one place. Include only inside the engine library.
#pragma once

#include "Scene/Components.h" // ParticleEmitter

#include <nlohmann/json.hpp>

namespace hbe::particle {

// The authored ParticleEmitter fields <-> a JSON object (runtime pool/stack/GPU state untouched).
nlohmann::json EmitterToJson(const ParticleEmitter& e);
void EmitterFromJson(const nlohmann::json& j, ParticleEmitter& e);

} // namespace hbe::particle

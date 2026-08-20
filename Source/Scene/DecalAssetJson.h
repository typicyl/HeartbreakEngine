// Scene/DecalAssetJson.h - the ONE DecalComponent field (de)serializer (JSON). Split from
// DecalAsset.h because it needs nlohmann (linked PRIVATE), so it must not appear in a header the
// executables include. Both the scene serializer and the `.hbdecal` asset delegate here. Engine-lib only.
#pragma once

#include "Scene/Components.h" // DecalComponent

#include <nlohmann/json.hpp>

namespace hbe::decalasset {

// Authored DecalComponent fields <-> a JSON object (runtime resolved handles untouched).
nlohmann::json DecalToJson(const DecalComponent& d);
void DecalFromJson(const nlohmann::json& j, DecalComponent& d);

} // namespace hbe::decalasset

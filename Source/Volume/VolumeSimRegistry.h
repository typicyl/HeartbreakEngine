// Source/Volume/VolumeSimRegistry.h - string-keyed factory of volume-simulation MODELS.
//
// Adding a new solver = a new IVolumeSimulation subclass + ONE registration line. The editor lists
// Types() in a dropdown, clones a type's defaultConfig, and Create()s an instance from the edited
// config. The baker + runtime never see the model id or the config - they consume VolumeFrame /
// .hbvol - so ExplosionSimulation, ImportedVdbSimulation, an OpenVDB source, etc. all coexist with
// ZERO downstream change. String-keyed on purpose (no enum) to dodge the catalog/enum-lockstep gotcha.
//
// Builtins are installed via EnsureBuiltins() (called lazily by Get()) rather than static-init
// self-registration, because static registrars in a STATIC library are stripped if nothing
// references their TU. The HBE_REGISTER_VOLUME_SIM macro is offered for out-of-tree/plugin models
// whose TU is guaranteed linked.
#pragma once

#include "Volume/IVolumeSimulation.h"
#include "Volume/VolumeSimConfig.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hbe::volume {

struct VolumeSimTypeInfo {
    std::string     id;            // registry key, e.g. "eulerian-smoke"
    std::string     displayName;   // editor label
    VolumeSimConfig defaultConfig; // a sensible starting config for this model
    std::function<std::unique_ptr<IVolumeSimulation>(const VolumeSimConfig&)> create;
};

class VolumeSimRegistry {
public:
    static VolumeSimRegistry& Get();

    void Register(VolumeSimTypeInfo info);                 // last registration of an id wins
    const std::vector<VolumeSimTypeInfo>& Types() const { return types_; }
    const VolumeSimTypeInfo* Find(const std::string& id) const;

    // Create the model named by config.model (nullptr if unknown). The returned sim is initialized
    // from `config`; call Reset() before stepping.
    std::unique_ptr<IVolumeSimulation> Create(const VolumeSimConfig& config) const;

private:
    VolumeSimRegistry() = default;
    std::vector<VolumeSimTypeInfo> types_;
};

// Optional convenience for out-of-tree models (TU guaranteed linked). Builtins do NOT use this.
struct VolumeSimRegistrar {
    explicit VolumeSimRegistrar(VolumeSimTypeInfo info) {
        VolumeSimRegistry::Get().Register(std::move(info));
    }
};
#define HBE_VOLSIM_CONCAT_(a, b) a##b
#define HBE_VOLSIM_CONCAT(a, b) HBE_VOLSIM_CONCAT_(a, b)
#define HBE_REGISTER_VOLUME_SIM(INFO_EXPR)                                                         \
    namespace {                                                                                     \
    const ::hbe::volume::VolumeSimRegistrar HBE_VOLSIM_CONCAT(hbeVolSimReg_, __LINE__){(INFO_EXPR)};\
    }

} // namespace hbe::volume

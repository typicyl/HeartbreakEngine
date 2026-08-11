// Source/Volume/VolumeSimRegistry.cpp - see the header.
#include "Volume/VolumeSimRegistry.h"

#include "Volume/EulerianSmokeSimulation.h"
#include "Volume/ProceduralVolumeSimulation.h"

namespace hbe::volume {
namespace {

// Install the in-tree models. Kept here (not static-init self-registration) so a static-library
// build cannot strip the registrations. Adding a builtin model = one Register() call here + the
// new class. Runs exactly once (guarded in Get()).
void EnsureBuiltins(VolumeSimRegistry& reg) {
    // eulerian-smoke: the real CPU reference fluid solver (density+temperature+velocity, advection,
    // buoyancy, vorticity confinement, dissipation, and pressure projection). The default config is
    // a hot plume rising out of a base emitter inside an 8-unit-tall box.
    {
        VolumeSimTypeInfo info;
        info.id          = "eulerian-smoke";
        info.displayName = "Eulerian Smoke / Fire";
        VolumeSimConfig& c = info.defaultConfig;
        c.model = "eulerian-smoke";
        c.bounds.worldMin = glm::vec3(-2.0f, 0.0f, -2.0f);
        c.bounds.worldMax = glm::vec3(2.0f, 8.0f, 2.0f);
        c.bounds.dim      = glm::ivec3(48, 96, 48);
        c.frameRate = 30.0f;
        c.substeps  = 2;
        c.buoyancyBeta = 8.0f;
        c.buoyancyAlpha = 0.5f;
        c.densityDissipation = 0.2f;
        c.temperatureCooling = 0.4f;
        c.vorticityStrength = 14.0f;
        c.pressureIterations = 40;
        {
            VolumeEmitter em;
            em.name = "base";
            em.shape.kind = VolumeShapeKind::Sphere;
            em.shape.center = glm::vec3(0.0f, 0.6f, 0.0f);
            em.shape.halfExtents = glm::vec3(0.75f);
            em.densityRate = 5.0f;
            em.temperatureRate = 8.0f;
            em.temperatureTarget = 1.0f;
            em.velocity = glm::vec3(0.0f, 2.0f, 0.0f); // seed the upward column
            c.emitters.push_back(em);
        }
        info.create = [](const VolumeSimConfig& cfg) -> std::unique_ptr<IVolumeSimulation> {
            return std::make_unique<EulerianSmokeSimulation>(cfg);
        };
        reg.Register(std::move(info));
    }

    // procedural-plume: the deterministic, GPU-free test fixture. It proves the whole
    // controller -> ReadbackFrame -> sink boundary + the baker's source-independence, and is the
    // stand-in until the real Eulerian solver lands (Phase 1).
    {
        VolumeSimTypeInfo info;
        info.id          = "procedural-plume";
        info.displayName = "Procedural Plume (test)";
        info.defaultConfig.model     = "procedural-plume";
        info.defaultConfig.bounds.worldMin = glm::vec3(-2.0f, 0.0f, -2.0f);
        info.defaultConfig.bounds.worldMax = glm::vec3(2.0f, 8.0f, 2.0f);
        info.defaultConfig.bounds.dim      = glm::ivec3(40, 80, 40);
        info.defaultConfig.frameRate = 30.0f;
        info.defaultConfig.substeps  = 1;
        info.create = [](const VolumeSimConfig& cfg) -> std::unique_ptr<IVolumeSimulation> {
            glm::ivec3 dim = cfg.bounds.dim;
            if (dim.x <= 0 || dim.y <= 0 || dim.z <= 0) dim = glm::ivec3(40, 80, 40);
            return std::make_unique<ProceduralVolumeSimulation>(dim);
        };
        reg.Register(std::move(info));
    }
}

} // namespace

VolumeSimRegistry& VolumeSimRegistry::Get() {
    static VolumeSimRegistry instance;
    static bool builtinsInstalled = false;
    if (!builtinsInstalled) {
        builtinsInstalled = true; // set BEFORE installing so a re-entrant Get() during install is safe
        EnsureBuiltins(instance);
    }
    return instance;
}

void VolumeSimRegistry::Register(VolumeSimTypeInfo info) {
    for (VolumeSimTypeInfo& t : types_) {
        if (t.id == info.id) { t = std::move(info); return; } // last-wins on duplicate id
    }
    types_.push_back(std::move(info));
}

const VolumeSimTypeInfo* VolumeSimRegistry::Find(const std::string& id) const {
    for (const VolumeSimTypeInfo& t : types_)
        if (t.id == id) return &t;
    return nullptr;
}

std::unique_ptr<IVolumeSimulation> VolumeSimRegistry::Create(const VolumeSimConfig& config) const {
    const VolumeSimTypeInfo* t = Find(config.model);
    if (!t || !t->create) return nullptr;
    return t->create(config);
}

} // namespace hbe::volume

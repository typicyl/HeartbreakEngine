// Source/Volume/VolumeSimConfig.h - the serializable authoring description of a volume simulation.
//
// ONE struct captures everything needed to (re)create + drive a sim deterministically: the domain,
// the timeline, shared physics coefficients, the emitters/obstacles, which fields to bake, and an
// open per-model params bag. It is the content of the `.hbvolsim` authoring asset. Crucially it is
// PURE DATA - no std::function, no GPU handles, no baker types - so it round-trips through the asset
// and reproduces the same VolumeFrame sequence on any machine (CPU) / same device (GPU).
//
// The baker + runtime NEVER see this struct: they consume VolumeFrame / .hbvol. Only the simulation
// layer (registry -> IVolumeSimulation) and the editor read it. That is the independence guarantee.
#pragma once

#include "Volume/VolumeFrame.h" // VolumeBounds, FieldType

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>
#include <string>
#include <vector>

namespace hbe::volume {

// ---------------------------------------------------------------------------------------------
// Data-driven animation curves (replace lambdas so emitters/obstacles are serializable + portable
// + deterministic). Time is SIM time in seconds. Piecewise-linear; empty => use `constant`.
// ---------------------------------------------------------------------------------------------
struct VolumeScalarKey { f32 time = 0.0f; f32 value = 0.0f; };

struct VolumeScalarCurve {
    std::vector<VolumeScalarKey> keys;   // sorted ascending by time
    f32                          constant = 0.0f; // used when keys is empty

    bool empty() const { return keys.empty(); }
    f32  Evaluate(f32 t) const {
        if (keys.empty()) return constant;
        if (t <= keys.front().time) return keys.front().value;
        if (t >= keys.back().time)  return keys.back().value;
        for (usize i = 1; i < keys.size(); ++i) {
            if (t <= keys[i].time) {
                const VolumeScalarKey& a = keys[i - 1];
                const VolumeScalarKey& b = keys[i];
                const f32 span = b.time - a.time;
                const f32 u = span > 1e-6f ? (t - a.time) / span : 0.0f;
                return a.value + (b.value - a.value) * u;
            }
        }
        return keys.back().value;
    }
};

struct VolumeVec3Key { f32 time = 0.0f; glm::vec3 value{0.0f}; };

struct VolumeVec3Curve {
    std::vector<VolumeVec3Key> keys;
    glm::vec3                  constant{0.0f};

    bool empty() const { return keys.empty(); }
    glm::vec3 Evaluate(f32 t) const {
        if (keys.empty()) return constant;
        if (t <= keys.front().time) return keys.front().value;
        if (t >= keys.back().time)  return keys.back().value;
        for (usize i = 1; i < keys.size(); ++i) {
            if (t <= keys[i].time) {
                const VolumeVec3Key& a = keys[i - 1];
                const VolumeVec3Key& b = keys[i];
                const f32 span = b.time - a.time;
                const f32 u = span > 1e-6f ? (t - a.time) / span : 0.0f;
                return a.value + (b.value - a.value) * u;
            }
        }
        return keys.back().value;
    }
};

// ---------------------------------------------------------------------------------------------
// Shape vocabulary shared by emitters AND obstacles, so a new solver needs zero new authoring
// types and one shared rasterizer (VolumeRasterize.h) voxelizes them identically. World-space.
// ---------------------------------------------------------------------------------------------
enum class VolumeShapeKind : u8 { Sphere = 0, Box = 1, Cone = 2, MeshVoxelized = 3 };

struct VolumeShape {
    VolumeShapeKind kind = VolumeShapeKind::Sphere;
    glm::vec3       center{0.0f};        // world-space center (Cone: center of the base)
    glm::vec3       halfExtents{0.5f};   // Box: half-size; Sphere/Cone: .x = radius
    glm::quat       rotation{1, 0, 0, 0};// orientation (Box/Cone; Cone axis = local +Y)
    f32             coneHeight = 1.0f;    // Cone only (narrows to a point at +coneHeight)
    f32             edgeSoftness = 0.15f; // 0 = hard edge, 1 = fully feathered (fraction of extent)
    u32             meshId = 0;           // MeshVoxelized (placeholder: treated as its AABB for now)
};

// ---------------------------------------------------------------------------------------------
// Emitter - a grid SOURCE term (field += rate*dt inside the shape), NOT a particle splat. Rates are
// per-second so Step(dt) integration is dt-independent. Continuous or one-shot Burst (a Burst with a
// radial inflow velocity + high fuel/heat is the explosion primitive). Optional data-driven anim.
// ---------------------------------------------------------------------------------------------
struct VolumeEmitter {
    std::string name = "emitter";
    VolumeShape shape;

    f32 densityRate       = 1.0f;  // smoke injected per second at full coverage
    f32 temperatureRate   = 3.0f;  // relaxation speed of T toward temperatureTarget (per second)
    f32 temperatureTarget = 1.0f;  // target temperature inside the emitter
    f32 fuelRate          = 0.0f;  // fuel injected per second (combustion; 0 = smoke-only)

    glm::vec3 velocity{0.0f};      // injected inflow velocity
    bool      worldVelocity = false; // true: `velocity` is world-space; false: rotated by shape.rotation

    enum class Mode : u8 { Continuous = 0, Burst = 1 } mode = Mode::Continuous;
    f32 startTime     = 0.0f;      // sim time the emitter turns on
    f32 endTime       = -1.0f;     // Continuous: off time (-1 = never); ignored for Burst
    f32 burstDuration = 0.1f;      // Burst: active window length from startTime

    // Optional animation (sim-time driven, serializable). Empty curves = static / no modulation.
    VolumeVec3Curve   translationCurve; // ADDED to shape.center over time (e.g. a moving torch)
    VolumeScalarCurve densityRateCurve; // MULTIPLIES densityRate over time (empty => x1)

    // A resolved snapshot at a given sim time - what the solver actually stamps this substep.
    struct Resolved {
        VolumeShape shape;
        f32         densityRate = 0.0f;
        f32         temperatureRate = 0.0f;
        f32         temperatureTarget = 0.0f;
        f32         fuelRate = 0.0f;
        glm::vec3   velocity{0.0f}; // already in world space
        bool        active = false;
        f32         strength = 0.0f; // 0..1 overall gate (Burst falloff etc.)
    };

    Resolved Resolve(f32 simTime) const {
        Resolved r;
        r.shape = shape;
        if (!translationCurve.empty()) r.shape.center += translationCurve.Evaluate(simTime);

        bool active = false;
        f32  strength = 0.0f;
        if (mode == Mode::Continuous) {
            active = simTime >= startTime && (endTime < 0.0f || simTime < endTime);
            strength = active ? 1.0f : 0.0f;
        } else { // Burst
            const f32 rel = simTime - startTime;
            active = rel >= 0.0f && rel < burstDuration;
            strength = active ? (1.0f - rel / glm::max(burstDuration, 1e-4f)) : 0.0f; // linear falloff
        }
        r.active = active;
        r.strength = strength;

        const f32 densMul = densityRateCurve.empty() ? 1.0f : densityRateCurve.Evaluate(simTime);
        r.densityRate       = densityRate * densMul * strength;
        r.temperatureRate   = temperatureRate * strength;
        r.temperatureTarget = temperatureTarget;
        r.fuelRate          = fuelRate * strength;
        r.velocity          = worldVelocity ? velocity : (shape.rotation * velocity);
        return r;
    }
};

// ---------------------------------------------------------------------------------------------
// Obstacle - a solid (no-flux) region, or a Sink that removes density. Static obstacles voxelize
// once at Reset(); moving obstacles (Phase 4) re-voxelize and carry a boundary velocity (which needs
// its own storage, so the v1 static path packs only a mask). The translation curve is sim-time
// driven so a "swung obstacle" re-simulates identically for scrub/bake.
// ---------------------------------------------------------------------------------------------
struct VolumeObstacle {
    std::string name = "obstacle";
    VolumeShape shape;
    enum class Kind : u8 { Solid = 0, Sink = 1 } kind = Kind::Solid;
    bool        moving = false;
    VolumeVec3Curve translationCurve; // ADDED to shape.center over time (moving obstacles)

    VolumeShape Resolve(f32 simTime) const {
        VolumeShape s = shape;
        if (!translationCurve.empty()) s.center += translationCurve.Evaluate(simTime);
        return s;
    }
};

// ---------------------------------------------------------------------------------------------
// The whole authoring description. `model` selects the solver via VolumeSimRegistry; a solver reads
// the shared coefficients it needs and ignores the rest. `modelParams` carries model-specific extras
// generically (so the editor + asset round-trip any model without compiling against it).
// ---------------------------------------------------------------------------------------------
struct VolumeSimConfig {
    std::string  model = "eulerian-smoke"; // registry id

    VolumeBounds bounds;                   // world domain + voxel resolution (fixed domain recommended)

    // Timeline (fixed timestep => reproducible scrub/bake).
    f32 frameRate = 30.0f;                 // baked frames per second
    int substeps  = 2;                     // sub-steps per frame (stability); fixed COUNT for the bake

    // Shared physics coefficients (Fedkiw-style smoke defaults). Temperature is normalized ~[0,1],
    // so buoyancy is an acceleration in world units/s^2 - it must be several units to lift a plume
    // across a multi-unit-tall domain in a second or two.
    f32       ambientTemperature = 0.0f;
    f32       buoyancyAlpha      = 0.6f;   // smoke weight (density pulls down)
    f32       buoyancyBeta       = 7.0f;   // thermal lift (temperature pushes up)
    f32       densityDissipation = 0.15f;  // per-second density fade (exp)
    f32       temperatureCooling = 0.4f;   // per-second cooling toward ambient (exp)
    f32       vorticityStrength  = 10.0f;  // vorticity-confinement epsilon
    int       pressureIterations = 40;     // projection solve sweeps (quality/perf knob)
    glm::vec3 gravity{0.0f, -1.0f, 0.0f};  // up = -normalize(gravity); default +Y up

    // Authoring.
    std::vector<VolumeEmitter>  emitters;
    std::vector<VolumeObstacle> obstacles;

    // Which fields are copied into VolumeFrame / baked (the exposure list; Q11). Names are the key.
    std::vector<std::string>    bakeFields{ "density", "temperature" };

    // Determinism / lifecycle.
    u32 seed             = 0;              // deterministic seed for any procedural detail
    u32 keyframeInterval = 0;             // 0 = scrub-back re-sims from Reset(); >0 = snapshot cadence

    // Model-specific extras (generic serialize/edit; a solver reads the keys it knows).
    std::map<std::string, f32> modelParams;

    // Convenience: world-space up direction implied by gravity.
    glm::vec3 up() const {
        const f32 len = glm::length(gravity);
        return len > 1e-6f ? -(gravity / len) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
};

} // namespace hbe::volume

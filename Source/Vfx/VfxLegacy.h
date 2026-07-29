// Vfx/VfxLegacy.h - bit-exact compatibility modules for pre-module-stack emitters.
//
// THE PROBLEM THIS SOLVES. Every ParticleEmitter authored before the module stack
// was simulated by one hand-fused loop in Scene/ParticleSystem.cpp. Moving those
// emitters onto the stack must not change a single pixel of an already-authored
// scene - an artist who saved a fire two months ago has to open it today and see the
// same fire. That is a stricter requirement than "looks similar": a particle system
// is chaotic, so a one-ulp difference in the first frame's random draw fans out into
// a visibly different effect within a second.
//
// So the migration is NOT a re-expression of the old emitter in terms of the new
// module catalog. `Update.Drag` is exp(-k*dt) and the old drag was (1 - k*dt);
// `Update.CurlNoiseForce` is divergence-free and the old turbulence was not;
// `Spawn.InitState` gives every particle a hashed stream and sub-frame birth, and the
// old spawn did neither. Each of those is an IMPROVEMENT, and each one changes the
// result. Rewriting an authored emitter in terms of them would silently re-tune it.
//
// Instead the old loop is preserved verbatim as four compound modules:
//   Legacy.InitState    - the old spawnOne(), including its shape/cone sampling
//   Legacy.Forces       - age, gravity, turbulence, buoyancy + vortex roll
//   Legacy.DragLinear   - the old framerate-DEPENDENT damping, on purpose
//   Legacy.Integrate    - position from velocity, rotation from the emitter spin
// They keep the old arithmetic, the old operation order, and the old RNG stream, so
// an untouched emitter is bit-identical. New modules then compose AROUND them: the
// artist opts into curl noise or exponential drag per emitter and only then does the
// behaviour change. That is what makes this a migration rather than a rewrite.
//
// These modules are CPU-ONLY and will never be ported to the GPU sim. The GPU path
// runs the real catalog; an emitter that wants to live on the GPU gets converted to
// real modules explicitly, by a human, who can then see what changed.
#pragma once

#include "Core/Types.h"
#include "Vfx/VfxStack.h"
#include "Vfx/VfxTypes.h"

#include <glm/glm.hpp>

namespace hbe {
namespace vfx {

// Mirrors ParticleEmitter::Shape value-for-value. Duplicated rather than included so
// Vfx does not depend on Scene; Scene/ParticleSystem.cpp holds the static_asserts
// that keep the two enums in lockstep.
enum class LegacyShape : u32 { Point = 0, Sphere, Hemisphere, Box, Disc, Cone, Count };

// The legacy emitter's parameter block, rebuilt from the component every frame and
// handed to the kernels through ParticleView::user. It does not live in ModuleParams
// because `basis` alone is 36 of the available 48 bytes - see the note on
// ParticleView about why that is acceptable for a CPU-only migration bridge.
struct LegacyParams {
    // Spawn (world transform + shape).
    glm::mat3 basis{1.0f};                // emitter rotation/scale
    glm::vec3 origin{0.0f};               // emitter world position; also the vortex axis
    glm::vec3 baseDir{0.0f, 1.0f, 0.0f};  // normalized world emit direction
    glm::vec3 boxHalfExtents{0.5f};
    LegacyShape shape = LegacyShape::Sphere;
    f32 emitRadius = 0.0f;
    f32 coneAngle = 25.0f; // degrees, half-angle
    f32 startSpeed = 1.5f;
    f32 speedVariance = 0.4f;
    f32 lifetime = 2.0f;
    f32 lifetimeVariance = 0.3f;
    f32 spread = 0.35f;

    // Motion.
    glm::vec3 gravity{0.0f, -0.6f, 0.0f};
    f32 turbulence = 0.0f;
    f32 turbulenceScale = 1.0f;
    f32 buoyancy = 0.0f;
    f32 vortex = 0.0f;
    f32 drag = 0.0f; // linear (legacy) damping coefficient
    f32 spin = 0.0f; // billboard rotation rate, rad/sec
};

// The four kernels. Declared here so the catalog in VfxStack.cpp can point at them;
// nothing else should call them directly.
void K_LegacyInitState(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt);
void K_LegacyForces(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt);
void K_LegacyDragLinear(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt);
void K_LegacyIntegrate(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt);

// The emitter RNG seed every pre-stack ParticleEmitter started from. Reproduced
// exactly: it is part of the authored look, not an implementation detail.
constexpr u32 kLegacyRngSeed = 0x1234567u;

} // namespace vfx
} // namespace hbe

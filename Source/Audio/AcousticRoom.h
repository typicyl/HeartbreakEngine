// Audio/AcousticRoom.h - engine-agnostic shoebox room acoustics (the integration<->backend POD).
//
// Computed from an AcousticSpace (AcousticWorld) and handed to the spatializer, which translates
// it into the backend's room properties INTERNALLY - no vraudio type ever crosses an engine
// header. Wall order and the 9 octave bands match the backend so the mapping is 1:1.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace hbe {

struct AcousticRoom {
    glm::vec3 position{0.0f};       // room center, world space
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // room orientation, world space
    glm::vec3 dimensions{1.0f};     // full box size (world space)
    // Per-wall reflection coefficient, world axes: [0]-x [1]+x [2]-y(floor) [3]+y(ceiling)
    // [4]-z [5]+z. 0 = fully absorbed (no reflection), 1 = perfect reflector.
    f32 reflectionCoeff[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    f32 reflectionCutoffHz = 800.0f; // low-pass applied to the early reflections
    f32 reflectionGain = 1.0f;       // uniform gain on early reflections
    f32 rt60[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // reverb decay per band (s)
    f32 reverbGain = 0.045f;         // late-reverb tail level
};

} // namespace hbe

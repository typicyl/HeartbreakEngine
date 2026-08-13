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

// One active acoustic ENVIRONMENT for the multi-environment reverb (hdsr::EnvironmentReverb): a
// room that contributes its own reverb tail to the listener, scaled by `coupling` (how much of it
// reaches the listener through portals/propagation; 1 = the listener is inside it). `id` is a
// stable identifier (e.g. an AcousticSpace entity). The library mixes ALL active environments up
// to its capacity - this is not a "pick the loudest room" selection.
//
// `coupling` is PER-BAND (9 octave bands): a distant room reached through walls/doorways couples
// its low frequencies more than its highs (transmission loss rises with frequency), so its tail
// bleeds in DARKENED, not merely quieter. The bands come from the room-to-room propagation graph
// (hdsr::SolvePropagation). A flat curve (all bands equal) is the "just quieter" special case.
struct AcousticEnvironment {
    int id = -1;
    f32 rt60[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    f32 coupling[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // per-band [0,1]
    f32 gain = 1.0f;
    f32 preDelaySec = 0.0f; // propagation delay of this room's tail to the listener (distance / c)
    f32 pan = 0.0f; // stereo pan [-1,+1]: the direction this room's reverb arrives from (0 = own room)
};

} // namespace hbe

// Audio/AcousticWorld.h - scene-driven acoustics: pick the listener's room, drive its reverb.
//
// The runtime "room graph" for physically-informed audio. Each frame it finds the highest-priority
// AcousticSpace region containing the listener and drives the spatial backend's room acoustics
// (early reflections + late reverb) from that space's dimensions + wall/floor/ceiling acoustic
// materials; outside all spaces the listener hears no room (dry / outdoors). P2 = listener-room
// reverb; portals + cross-room transmission arrive in P3. Integration-layer code (knows the scene
// + audio systems); the spatial backend never sees any of it.
#pragma once

#include "Assets/AcousticMaterial.h"
#include "Audio/AcousticQuery.h" // AcousticMaterialCache (reused by P3)
#include "Audio/AcousticRoom.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Scene;
class AudioSystem;
class PhysicsWorld;

class AcousticWorld {
public:
    // Drives the listener's room (early reflections + reverb via the single-room path) AND, when
    // `environmentReverbEnabled`, the MULTI-ENVIRONMENT reverb. It builds a room-to-room PROPAGATION
    // GRAPH - every enabled AcousticSpace is a node, plus a listener node - with edges carrying the
    // PER-BAND transmission through the intervening walls/portals (measured with `physics`), and asks
    // the library (hdsr::SolvePropagation) for each room's best-path per-band coupling to the
    // listener. Each room is then declared as an environment so the library mixes its own reverb
    // tail, DARKENED by that per-band coupling (a distant room bleeds in muffled, not merely
    // quieter). The listener's room also drives the single-room path (reflections; its late tail
    // comes from the environment reverb when enabled). `physics` may be null when the game is not
    // simulating (coupling then can't be measured, so only the listener's room contributes).
    void Update(Scene& scene, const glm::vec3& listenerPos, const glm::vec3& listenerForward,
                const std::filesystem::path& assetsDir, AudioSystem& audio,
                const PhysicsWorld* physics, bool environmentReverbEnabled);

    // Fraction of sound energy transmitted from `a` to `b` through intervening geometry: the
    // product of each hit wall's material transmission, with any AcousticPortal opening overriding
    // the wall it overlaps (a doorway lets sound through). 1 = clear path, 0 = fully blocked. This
    // is what the occlusion pass calls per ray; it uses the internal material cache.
    f32 SegmentTransmission(const PhysicsWorld& physics, const Scene& scene, const glm::vec3& a,
                            const glm::vec3& b, const std::filesystem::path& assetsDir);

    // Drop cached state (call on project/scene switch).
    void ClearCaches();

private:
    // Per-band sibling of SegmentTransmission: fills `out[9]` with the octave-band transmission from
    // `a` to `b` (each hit surface's frequency-dependent transmission, portal openings overriding
    // the wall they cover, combined per band by the library). This carries the spectrum that makes
    // cross-room bleed muffled rather than merely quiet; it feeds the propagation-graph edges.
    void SegmentTransmissionBands(const PhysicsWorld& physics, const Scene& scene, const glm::vec3& a,
                                  const glm::vec3& b, const std::filesystem::path& assetsDir,
                                  f32 out[9]);

    // Builds the room-to-room propagation graph (rooms + a listener node, edges = per-band
    // transmission), solves each room's per-band coupling to the listener via hdsr::SolvePropagation,
    // and declares the coupled rooms as environments (audio.SetEnvironments). `listenerRoom` is the
    // AcousticSpace the listener is in (entt::null outdoors). Only defined when the library is built
    // in (the environment reverb is a no-op otherwise).
    void BuildEnvironments(Scene& scene, const glm::vec3& listenerPos,
                           const glm::vec3& listenerForward,
                           const std::filesystem::path& assetsDir, AudioSystem& audio,
                           const PhysicsWorld* physics, entt::entity listenerRoom);

    AcousticMaterialCache matCache_; // per-surface material cache (used by P3 occlusion)
    AcousticRoom lastRoom_{};
    bool lastEnabled_ = false;
    bool hasPushed_ = false;
    entt::entity lastSpace_ = entt::null;
};

// Computes shoebox room acoustics from box geometry + per-face acoustic materials:
// per-wall reflection coefficients (sqrt(1 - mid-band absorption)) + Eyring RT60 per octave band.
// Exposed for the headless self-test.
AcousticRoom ComputeAcousticRoom(const glm::vec3& center, const glm::quat& rotation,
                                 const glm::vec3& dimensions, const AcousticMaterial& wall,
                                 const AcousticMaterial& floor, const AcousticMaterial& ceiling,
                                 f32 reflectionGain, f32 reverbGain, f32 reverbTime);

// Transmission of a portal opening: fully closed (openness 0) uses the closed material's
// transmission; fully open (1) passes everything. Exposed for the self-test.
f32 PortalTransmission(const AcousticMaterial& closedMaterial, f32 openness);

// A detected room (world AABB) from the acoustic bake.
struct AcousticCellCluster {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    int cellCount = 0;
};

// Groups interior cells (from the GI/IBL enclosure classifier, AutoPlaceProbes) into rooms: cells
// within ~spacing*1.75 of one another join one cluster. Returns one padded world AABB per cluster.
// This is the "auto-detect rooms" core of the acoustic bake; the editor turns each cluster into an
// AcousticSpace. Exposed for the self-test.
std::vector<AcousticCellCluster> ClusterAcousticCells(const std::vector<glm::vec3>& cells, f32 spacing);

// Headless self-test (part of --test-acoustics): the room-acoustics math sanity (hard room ->
// long RT60, open/absorptive room -> short RT60, reflection coefficients, geometry pass-through),
// plus the portal-transmission + occlusion-mapping math.
bool AcousticRoomSelfTest();

} // namespace hbe

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
    // Recomputes the listener's room from the highest-priority enabled AcousticSpace containing
    // `listenerPos` and pushes it to `audio` (only when it changed, so a static room does not
    // re-trigger the reverb network every frame). Outside all spaces -> room disabled (dry).
    void Update(Scene& scene, const glm::vec3& listenerPos,
                const std::filesystem::path& assetsDir, AudioSystem& audio);

    // Fraction of sound energy transmitted from `a` to `b` through intervening geometry: the
    // product of each hit wall's material transmission, with any AcousticPortal opening overriding
    // the wall it overlaps (a doorway lets sound through). 1 = clear path, 0 = fully blocked. This
    // is what the occlusion pass calls per ray; it uses the internal material cache.
    f32 SegmentTransmission(const PhysicsWorld& physics, const Scene& scene, const glm::vec3& a,
                            const glm::vec3& b, const std::filesystem::path& assetsDir);

    // Drop cached state (call on project/scene switch).
    void ClearCaches();

private:
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

// Construction/ConstructionSystem.h - turning a ProceduralBuilding into scene geometry.
//
// THE REGENERATOR. `kExclusions` in SceneSerializer.cpp names this function as the reason chunk
// entities may be left out of a .hbscene. That row is a CLAIM, and --test-scenesave checks it: if
// Sync stopped putting the children back, excluded entities would simply vanish from every level
// that contained a building.
//
// WHEN IT RUNS, and why not every frame. The RHI has no mesh destroy - `DestroyGpuBuffer` (compute
// buffers) is the only destroy in the whole interface - so every regeneration abandons its
// previous vertex and index buffers for the process lifetime. Regenerating on a slider drag would
// leak an entire building per frame. So a building carries a REVISION, Sync only rebuilds when it
// has moved, and the editor bumps it on a deliberate action rather than on every parameter edit.
// Terrain already lives under the same constraint and made the same choice.
#pragma once

#include "Scene/Scene.h"

namespace hbe {
class Renderer;
}

namespace hbe::construction {

// Rebuilds the chunk entities of every ProceduralBuilding whose revision has moved.
//
// MAIN THREAD ONLY: it creates and destroys entities and uploads meshes, and this engine permits
// neither from a job worker. Returns the number of buildings rebuilt this call (0 in the common
// case, which is why it is cheap to call every frame).
u32 Sync(Scene& scene, Renderer& renderer);

// Drops the generated children of one building without touching its definition. Used before a
// rebuild and when the component is removed.
void ClearGenerated(Scene& scene, entt::entity owner);

} // namespace hbe::construction

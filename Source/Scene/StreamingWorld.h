// Scene/StreamingWorld.h - distance-based world streaming (world partition).
//
// A world is a set of cells, each backed by a .hbscene. As a focus point
// (usually the camera) moves, cells within `loadRadius` are loaded and cells
// past `unloadRadius` are unloaded - so an open world larger than memory keeps
// only the neighbourhood resident. Loads are asynchronous on the job system
// (parse + asset IO on workers); only the bounded GPU instantiate of a finished
// load touches the main thread, so streaming never stalls the frame.
#pragma once

#include "Core/Types.h"
#include "Scene/SceneSerializer.h"

#include <atomic>
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace hbe {

class Scene;
class Renderer;

// One streamable region of the world. `scene` is either a single .hbscene or a
// LEVEL (its static + dynamic layers stream together) - a level is detected from
// a .static/.dynamic member path, a level base path, or `level: true`.
struct StreamingCell {
    std::string name;
    std::string scene;        // .hbscene, or a level member/base path
    bool        level = false; // force level interpretation (load static+dynamic)
    glm::vec3   center{0.0f};  // world point the focus distance is measured to
    glm::vec3   offset{0.0f};  // translation applied to the cell's ROOT entities
    f32         loadRadius   = 120.0f;
    f32         unloadRadius = 160.0f; // must exceed loadRadius (hysteresis)
};

class StreamingWorld : public NonCopyable {
public:
    StreamingWorld();
    ~StreamingWorld();

    // Loads a `.hbworld` manifest (JSON list of cells). `assetsDir` is the
    // project's Assets/ root that relative cell scene paths resolve against.
    bool LoadManifest(const std::filesystem::path& worldFile,
                      const std::filesystem::path& assetsDir);

    // Installs cells directly (programmatic worlds / tests).
    void SetCells(std::vector<StreamingCell> cells,
                  const std::filesystem::path& assetsDir);

    bool Active() const { return !cells_.empty(); }
    void SetMaxConcurrentLoads(u32 n) { maxConcurrentLoads_ = n ? n : 1; }

    // Distance streaming gate. When disabled, Update() is a no-op so a manually
    // loaded set of cells (e.g. the editor's "Load All") stays resident instead
    // of being unloaded by distance. On by default.
    void SetEnabled(bool on) { enabled_ = on; }
    bool Enabled() const { return enabled_; }

    // Per-frame tick: kicks async loads for cells the focus has entered, and
    // unloads cells it has left. Non-blocking. No-op when disabled.
    void Update(Scene& scene, Renderer& renderer, const glm::vec3& focus);

    // Loads EVERY cell synchronously, ignoring distance (editor: edit the whole
    // world at once). Disables distance streaming so Update won't unload them.
    void LoadAll(Scene& scene, Renderer& renderer);

    // Destroys every streamed entity and resets all cells to Unloaded. Waits
    // for any in-flight load jobs first (safe to call before scene teardown).
    void UnloadAll(Scene& scene);

    struct Stats {
        u32 cells = 0;
        u32 loaded = 0;
        u32 loading = 0;
        u32 entities = 0;
    };
    Stats GetStats() const;

private:
    // Worker job state, published to the main thread via `state` (acquire/release).
    enum class State : int { Unloaded, Loading, Ready, Failed, Loaded };

    // A cell loads one or more layers: a single .hbscene = one Full layer; a
    // level = its static + dynamic layers (each tagged with its own file).
    struct Layer {
        SceneKind kind = SceneKind::Full;
        std::filesystem::path path; // resolved absolute path (stable)
        scene::SceneData data;      // worker-filled scratch
        scene::StagedAssets staged;
    };

    struct Cell {
        StreamingCell desc;
        std::atomic<int> state{static_cast<int>(State::Unloaded)};
        std::vector<entt::entity> entities; // created on instantiate (main thread)
        std::vector<Layer> layers;          // 1 (scene) or 2 (level static+dynamic)
        std::filesystem::path assetsDir;
    };

    static void RunLoadJob(void* arg); // worker: parse + stage into the cell

    void Finalize(Cell& c, Scene& scene, Renderer& renderer); // main: instantiate
    void Unload(Cell& c, Scene& scene);

    std::vector<std::unique_ptr<Cell>> cells_;
    std::filesystem::path assetsDir_;
    u32 maxConcurrentLoads_ = 4;
    bool enabled_ = true;
};

} // namespace hbe

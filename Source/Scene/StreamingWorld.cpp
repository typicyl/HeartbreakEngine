// Scene/StreamingWorld.cpp
#include "Scene/StreamingWorld.h"

#include "Assets/VFS.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <thread>

namespace hbe {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
glm::vec3 JsonVec3(const json& j, const char* key, glm::vec3 def = glm::vec3(0.0f)) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return def;
    return {(*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>()};
}
} // namespace

StreamingWorld::StreamingWorld() = default;
StreamingWorld::~StreamingWorld() = default;

void StreamingWorld::SetCells(std::vector<StreamingCell> cells, const fs::path& assetsDir) {
    cells_.clear();
    assetsDir_ = assetsDir;
    for (StreamingCell& cd : cells) {
        auto c = std::make_unique<Cell>();
        c->desc = std::move(cd);
        c->assetsDir = assetsDir;
        const fs::path sp = c->desc.scene;
        const fs::path resolved = sp.is_absolute() ? sp : (assetsDir / sp);

        // A cell is a LEVEL when flagged, when its path is a .static/.dynamic
        // member, or when a base path has a static/dynamic sibling on disk; then
        // it streams both layers. Otherwise it's a single .hbscene.
        scene::LevelPaths lp = scene::ResolveLevel(resolved);
        bool asLevel = c->desc.level || scene::IsLevelMember(resolved);
        if (!asLevel && resolved.extension() != ".hbscene") {
            asLevel = vfs::Exists(lp.Member(SceneKind::Static)) ||
                      vfs::Exists(lp.Member(SceneKind::Dynamic));
        }
        if (asLevel) {
            for (const SceneKind k : {SceneKind::Static, SceneKind::Dynamic}) {
                const fs::path m = lp.Member(k);
                if (!vfs::Exists(m)) continue;
                Layer L;
                L.kind = k;
                L.path = m;
                c->layers.push_back(std::move(L));
            }
        }
        if (c->layers.empty()) { // single scene (or a level with no layers found)
            Layer L;
            L.kind = SceneKind::Full;
            L.path = resolved;
            c->layers.push_back(std::move(L));
        }
        cells_.push_back(std::move(c));
    }
}

bool StreamingWorld::LoadManifest(const fs::path& worldFile, const fs::path& assetsDir) {
    std::ifstream f(worldFile);
    if (!f) {
        HBE_WARN("StreamingWorld: cannot open '{}'.", worldFile.string());
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        HBE_ERROR("StreamingWorld: parse error in '{}': {}", worldFile.string(), e.what());
        return false;
    }

    std::vector<StreamingCell> cells;
    if (const auto it = j.find("cells"); it != j.end() && it->is_array()) {
        for (const json& jc : *it) {
            StreamingCell c;
            c.name = jc.value("name", std::string());
            c.scene = jc.value("scene", std::string());
            c.level = jc.value("level", false); // load both static+dynamic layers
            c.center = JsonVec3(jc, "center");
            c.offset = JsonVec3(jc, "offset");
            c.loadRadius = jc.value("loadRadius", 120.0f);
            c.unloadRadius = jc.value("unloadRadius", c.loadRadius * 1.35f);
            if (c.unloadRadius <= c.loadRadius)
                c.unloadRadius = c.loadRadius * 1.25f + 1.0f; // enforce hysteresis
            if (!c.scene.empty()) cells.push_back(std::move(c));
        }
    }
    SetCells(std::move(cells), assetsDir);
    HBE_INFO("StreamingWorld: manifest '{}' -> {} cells.", worldFile.string(),
             static_cast<u32>(cells_.size()));
    return true;
}

// Worker thread: pure CPU/file work (parse + asset staging), then publishes the
// result by advancing `state` with release semantics.
void StreamingWorld::RunLoadJob(void* arg) {
    Cell* c = static_cast<Cell*>(arg);
    for (Layer& L : c->layers) {
        if (!scene::ParseSceneFile(L.path, L.data)) {
            c->state.store(static_cast<int>(State::Failed), std::memory_order_release);
            return;
        }
        scene::StageAssets(L.data, c->assetsDir, L.staged);
        L.data.kind = L.kind; // the file path is the source of truth for the layer
    }
    c->state.store(static_cast<int>(State::Ready), std::memory_order_release);
}

void StreamingWorld::Finalize(Cell& c, Scene& scene, Renderer& renderer) {
    c.entities.clear();
    // Instantiate each layer, tagging its entities with that layer's file path so
    // the hierarchy groups them and a save writes them back to the right file.
    // Instantiate overwrites its createdOut, so accumulate across layers here.
    for (Layer& L : c.layers) {
        std::vector<entt::entity> created;
        scene::Instantiate(scene, renderer, L.data, L.staged, scene::LoadMode::Additive,
                           &created, L.path.string());
        c.entities.insert(c.entities.end(), created.begin(), created.end());
    }

    // Place the cell by translating its ROOT entities (children inherit it
    // through the Parent chain in Scene::WorldMatrix).
    if (c.desc.offset != glm::vec3(0.0f)) {
        auto& reg = scene.Registry();
        for (const entt::entity e : c.entities) {
            if (reg.all_of<Parent>(e)) continue;
            if (Transform* t = reg.try_get<Transform>(e)) t->position += c.desc.offset;
        }
    }

    // NavigationAgents path on GridNav, which auto-rebuilds from static geometry -
    // a streamed cell's walkable surface is picked up automatically, no nav step here.

    for (Layer& L : c.layers) { // staging memory is now resident; release it
        L.data = {};
        L.staged = {};
    }
    c.state.store(static_cast<int>(State::Loaded), std::memory_order_release);
    HBE_INFO("StreamingWorld: loaded cell '{}' ({} entities).", c.desc.name,
             static_cast<u32>(c.entities.size()));
}

void StreamingWorld::Unload(Cell& c, Scene& scene) {
    auto& reg = scene.Registry();
    for (const entt::entity e : c.entities)
        if (reg.valid(e)) reg.destroy(e);
    const u32 n = static_cast<u32>(c.entities.size());
    c.entities.clear();
    c.state.store(static_cast<int>(State::Unloaded), std::memory_order_relaxed);
    HBE_INFO("StreamingWorld: unloaded cell '{}' ({} entities).", c.desc.name, n);
}

void StreamingWorld::Update(Scene& scene, Renderer& renderer, const glm::vec3& focus) {
    if (!enabled_) return; // pinned (e.g. editor "Load All"): no distance load/unload
    // 1) Finalize completed loads / clear failures (main thread).
    for (std::unique_ptr<Cell>& up : cells_) {
        const State s = static_cast<State>(up->state.load(std::memory_order_acquire));
        if (s == State::Ready) {
            Finalize(*up, scene, renderer);
        } else if (s == State::Failed) {
            HBE_WARN("StreamingWorld: cell '{}' failed to load.", up->desc.name);
            up->state.store(static_cast<int>(State::Unloaded), std::memory_order_relaxed);
        }
    }

    // 2) Throttle: count in-flight loads.
    u32 loading = 0;
    for (std::unique_ptr<Cell>& up : cells_)
        if (static_cast<State>(up->state.load(std::memory_order_relaxed)) == State::Loading)
            ++loading;

    // 3) Distance test: enter -> load, leave -> unload.
    for (std::unique_ptr<Cell>& up : cells_) {
        Cell& c = *up;
        const State s = static_cast<State>(c.state.load(std::memory_order_relaxed));
        const f32 dist = glm::distance(focus, c.desc.center);

        if (s == State::Unloaded && dist <= c.desc.loadRadius) {
            if (loading >= maxConcurrentLoads_) continue; // try again next frame
            c.state.store(static_cast<int>(State::Loading), std::memory_order_relaxed);
            if (jobs::IsInitialized()) {
                jobs::RunDetached(&StreamingWorld::RunLoadJob, &c);
            } else {
                RunLoadJob(&c); // synchronous fallback (e.g. tools before init)
            }
            ++loading;
        } else if (s == State::Loaded && dist > c.desc.unloadRadius) {
            Unload(c, scene);
        }
    }
}

void StreamingWorld::UnloadAll(Scene& scene) {
    // Drain in-flight loads so no worker writes a cell while we reset it.
    for (std::unique_ptr<Cell>& up : cells_) {
        while (static_cast<State>(up->state.load(std::memory_order_acquire)) ==
               State::Loading) {
            std::this_thread::yield();
        }
    }
    for (std::unique_ptr<Cell>& up : cells_) {
        const State s = static_cast<State>(up->state.load(std::memory_order_relaxed));
        if (s == State::Loaded) {
            Unload(*up, scene);
        } else {
            for (Layer& L : up->layers) {
                L.data = {};
                L.staged = {};
            }
            up->state.store(static_cast<int>(State::Unloaded), std::memory_order_relaxed);
        }
    }
}

void StreamingWorld::LoadAll(Scene& scene, Renderer& renderer) {
    enabled_ = false; // pin everything loaded; Update won't unload by distance
    for (std::unique_ptr<Cell>& up : cells_) {
        Cell& c = *up;
        if (static_cast<State>(c.state.load(std::memory_order_acquire)) != State::Unloaded)
            continue;
        RunLoadJob(&c); // synchronous parse + stage on the calling thread
        if (static_cast<State>(c.state.load(std::memory_order_acquire)) == State::Ready)
            Finalize(c, scene, renderer);
    }
    HBE_INFO("StreamingWorld: loaded all {} cells (streaming paused for editing).",
             static_cast<u32>(cells_.size()));
}

StreamingWorld::Stats StreamingWorld::GetStats() const {
    Stats st;
    st.cells = static_cast<u32>(cells_.size());
    for (const std::unique_ptr<Cell>& up : cells_) {
        const State s = static_cast<State>(up->state.load(std::memory_order_relaxed));
        if (s == State::Loaded) {
            ++st.loaded;
            st.entities += static_cast<u32>(up->entities.size());
        } else if (s == State::Loading || s == State::Ready) {
            ++st.loading;
        }
    }
    return st;
}

bool StreamingWorld::IsSettled(const glm::vec3& focus) const {
    for (const std::unique_ptr<Cell>& up : cells_) {
        const State s = static_cast<State>(up->state.load(std::memory_order_acquire));
        // A load in flight (or finished-but-not-yet-instantiated) is never settled.
        if (s == State::Loading || s == State::Ready) return false;
        // An in-range cell that hasn't started loading will pop in once revealed.
        // (State::Failed is left as-is: a broken cell won't appear, so waiting on it
        // would hang the loading screen forever.)
        if (s == State::Unloaded &&
            glm::distance(focus, up->desc.center) <= up->desc.loadRadius)
            return false;
    }
    return true;
}

} // namespace hbe

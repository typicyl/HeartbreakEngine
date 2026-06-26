// Scene/Level.cpp
#include "Scene/Level.h"

#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <vector>

namespace hbe {
namespace scene {
namespace {

namespace fs = std::filesystem;

fs::path WithSuffix(const fs::path& base, const char* suffix) {
    if (base.empty()) return {};
    fs::path p = base;
    p += suffix; // fs::path += concatenates (no separator)
    return p;
}

// Loads one layer file additively, tagged with its path so it can be unloaded
// in isolation. The static layer (applyEnv) also pushes the file's environment
// onto the scene. No-op (returns false) when the layer file is absent.
bool LoadLayer(Scene& scene, Renderer& renderer, const fs::path& assetsDir, SceneKind kind,
               const fs::path& file, bool applyEnv) {
    if (file.empty() || !vfs::Exists(file)) return false; // a layer may be absent
    SceneData data;
    if (!ParseSceneFile(file, data)) return false;
    data.kind = kind; // the filename is the source of truth for the layer
    if (applyEnv) {
        scene.Environment().ambientIntensity = data.ambientIntensity;
        scene.Environment().exposure = data.exposure;
        scene.Environment().post = data.post;
    }
    StagedAssets staged;
    StageAssets(data, assetsDir, staged);
    Instantiate(scene, renderer, data, staged, LoadMode::Additive, nullptr, file.string());
    return true;
}

void DestroyBySceneSource(Scene& scene, const std::string& tag) {
    if (tag.empty()) return;
    auto& reg = scene.Registry();
    std::vector<entt::entity> kill;
    for (const entt::entity e : reg.view<SceneSource>())
        if (reg.get<SceneSource>(e).scene == tag) kill.push_back(e);
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e);
}

} // namespace

fs::path Level::StaticScene() const { return WithSuffix(base_, ".static.hbscene"); }
fs::path Level::DynamicScene() const { return WithSuffix(base_, ".dynamic.hbscene"); }

bool Level::Load(Scene& scene, Renderer& renderer, const fs::path& assetsDir) {
    if (loaded_) Unload(scene);
    // Static first so it owns the environment, then dynamic stacks on top.
    bool any = LoadLayer(scene, renderer, assetsDir, SceneKind::Static, StaticScene(), true);
    any |= LoadLayer(scene, renderer, assetsDir, SceneKind::Dynamic, DynamicScene(), false);
    loaded_ = any;
    if (any) {
        HBE_INFO("Level: loaded '{}' (static + dynamic).", Name());
    } else {
        HBE_WARN("Level: '{}' has no layer files to load.", Name());
    }
    return any;
}

void Level::Unload(Scene& scene) {
    DestroyBySceneSource(scene, StaticScene().string());
    DestroyBySceneSource(scene, DynamicScene().string());
    loaded_ = false;
}

bool Level::ReloadDynamic(Scene& scene, Renderer& renderer, const fs::path& assetsDir) {
    DestroyBySceneSource(scene, DynamicScene().string());
    return LoadLayer(scene, renderer, assetsDir, SceneKind::Dynamic, DynamicScene(), false);
}

} // namespace scene
} // namespace hbe

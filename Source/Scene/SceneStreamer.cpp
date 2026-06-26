// Scene/SceneStreamer.cpp
#include "Scene/SceneStreamer.h"

#include "Core/Log.h"
#include "Scene/Scene.h"

namespace hbe {

SceneStreamer::~SceneStreamer() {
    Join();
}

void SceneStreamer::Join() {
    if (worker_.joinable()) worker_.join();
}

bool SceneStreamer::BeginLoad(const std::filesystem::path& scenePath,
                              const std::filesystem::path& assetsDir,
                              scene::LoadMode mode) {
    if (state_.load() == State::Loading) return false;
    Join();

    path_ = scenePath;
    mode_ = mode;
    data_ = {};
    staged_ = {};
    state_.store(State::Loading);

    worker_ = std::thread([this, scenePath, assetsDir]() {
        scene::SceneData data;
        if (!scene::ParseSceneFile(scenePath, data)) {
            state_.store(State::Failed);
            return;
        }
        scene::StagedAssets staged;
        scene::StageAssets(data, assetsDir, staged);
        data_ = std::move(data);
        staged_ = std::move(staged);
        state_.store(State::Staged); // hand-off to Pump (main thread)
    });
    HBE_INFO("SceneStreamer: streaming '{}'...", scenePath.string());
    return true;
}

bool SceneStreamer::Pump(Scene& scene, Renderer& renderer) {
    const State s = state_.load();
    if (s == State::Failed) {
        Join();
        state_.store(State::Idle);
        HBE_WARN("SceneStreamer: failed to load '{}'.", path_.string());
        return false;
    }
    if (s != State::Staged) return false;

    Join();
    // Tag additively-streamed entities with the scene PATH so the hierarchy
    // groups them and a save writes them back to that scene file.
    const std::string tag =
        mode_ == scene::LoadMode::Additive ? path_.string() : std::string();
    scene::Instantiate(scene, renderer, data_, staged_, mode_, nullptr, tag);
    data_ = {};
    staged_ = {};
    state_.store(State::Idle);
    return true;
}

} // namespace hbe

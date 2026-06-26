// Scene/SceneStreamer.h - asynchronous scene loading.
//
// BeginLoad() parses the .hbscene and reads every referenced asset on a worker
// thread (pure CPU/file IO); Pump() — called once per frame on the main
// thread — instantiates the finished load (GPU uploads + entity creation).
// This is the streaming path: the frame loop never blocks on scene file IO.
#pragma once

#include "Scene/SceneSerializer.h"

#include <atomic>
#include <filesystem>
#include <thread>

namespace hbe {

class Scene;
class Renderer;

class SceneStreamer : public NonCopyable {
public:
    ~SceneStreamer();

    // Starts loading on a worker thread. Returns false while another load is
    // still in flight.
    bool BeginLoad(const std::filesystem::path& scenePath,
                   const std::filesystem::path& assetsDir,
                   scene::LoadMode mode = scene::LoadMode::Additive);

    // Finalizes a completed load (main thread). Returns true the frame the
    // scene's entities were instantiated.
    bool Pump(Scene& scene, Renderer& renderer);

    bool Busy() const { return state_.load() == State::Loading; }
    const std::filesystem::path& CurrentPath() const { return path_; }

private:
    enum class State : int { Idle, Loading, Staged, Failed };

    void Join();

    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::filesystem::path path_;
    scene::LoadMode mode_ = scene::LoadMode::Additive;
    scene::SceneData data_;
    scene::StagedAssets staged_;
};

} // namespace hbe

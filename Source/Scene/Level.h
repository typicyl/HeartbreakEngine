// Scene/Level.h - a playable level = its static + dynamic scene halves.
//
// Naughty-Dog-style split: a Level owns the STATIC layer (non-moving world
// geometry; the navmesh + baked-lighting source, stays resident) and the
// DYNAMIC layer (actors, physics, scripted/skeletal objects that move or
// reload). UI is deliberately NOT part of a level - main menus and the HUD are
// their own standalone scenes, loaded independently and kept resident across
// level switches.
//
// A Level loads its two layers ADDITIVELY (the static layer also applies the
// scene environment) and tags every spawned entity with its source file, so
// Unload destroys exactly this level's entities and leaves UI / other scenes
// untouched. Switching levels = Unload the old one, Load the new one.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace hbe {

class Scene;
class Renderer;

namespace scene {

class Level {
public:
    Level() = default;

    // <dir>/<name> with no suffix; the layer files derive from it.
    void SetBase(std::filesystem::path base) { base_ = std::move(base); }
    const std::filesystem::path& Base() const { return base_; }
    std::string Name() const { return base_.empty() ? std::string() : base_.filename().string(); }
    std::filesystem::path StaticScene() const;  // <base>.static.hbscene
    std::filesystem::path DynamicScene() const; // <base>.dynamic.hbscene
    bool Loaded() const { return loaded_; }

    // Loads the static (env-owning) then dynamic layer additively into `scene`.
    // Unloads this level first if already loaded. `assetsDir` resolves the
    // referenced meshes/textures/materials. Returns true if any layer loaded.
    bool Load(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir);

    // Destroys exactly this level's entities (static + dynamic), by their source
    // tag, leaving UI and any other loaded scenes alone.
    void Unload(Scene& scene);

    // Reloads only the dynamic layer (keeps static) - e.g. a checkpoint restart.
    bool ReloadDynamic(Scene& scene, Renderer& renderer,
                       const std::filesystem::path& assetsDir);

private:
    std::filesystem::path base_;
    bool loaded_ = false;
};

} // namespace scene
} // namespace hbe

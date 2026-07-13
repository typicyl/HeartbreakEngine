// Schematic/SchematicSystem.h - runs SchematicComponents over the ECS.
//
// Each frame, while the simulation plays, every SchematicComponent's graph gets
// its On Start event fired once, then On Update every frame. A graph runs one of
// two ways:
//   * COMPILED  - the graph was transpiled to native C++ and baked into the exe
//                 (see SchematicTranspile); the runtime calls it directly.
//   * INTERPRETED - the .hbschem is loaded + walked by the VM in the .cpp (the
//                 editor/dev path, and the runtime fallback for un-baked graphs).
#pragma once

#include "Core/Input.h"          // Input, Key
#include "Core/Types.h"
#include "Schematic/Schematic.h" // Value, PinType, NodeType

#include <entt/entt.hpp>

#include <string>

namespace hbe {

class Scene;
struct SchematicComponent; // defined in Scene/Components.h

namespace schematic {

// Ticks every SchematicComponent when `playing` (editor play mode / runtime).
// On Start fires on the rising edge per entity (component.started); On Update each
// frame thereafter. Uses the compiled version when one is registered for the
// component's asset, else interprets the loaded graph.
void Update(Scene& scene, Input& input, f32 dt, bool playing);

// Drops the cached graphs (project switch / a `.hbschem` was re-saved in the editor).
void ClearCache();

// --- Compiled (transpiled-to-C++) schematics --------------------------------
// The same context the interpreter VM sees, handed to a baked graph function.
struct CompiledContext {
    Scene& scene;
    Input& input;
    entt::entity self;
    f32 dt;
    SchematicComponent& inst; // per-instance vars / Delay timers / started flag
    // UI event payload (set only while firing EventUIClicked/EventUIChanged; the
    // fields are DEFAULTED so existing aggregate-init call sites + previously baked
    // units stay source-compatible).
    const std::string* eventAction = nullptr; // the widget's UIElement.action id
    f32 eventValue = 0.0f;                    // slider value at the change
    bool eventToggled = false;                // toggle state at the change
    f32 eventSelected = 0.0f;                 // selector index at the change
    // OnDeath event payload (set only while firing NodeType::OnDeath; defaulted so
    // previously baked units + aggregate-init sites stay source-compatible).
    const std::string* eventDeathTag = nullptr; // Health.deathTag (or entity Name)
    entt::entity eventInstigator = entt::null;   // who dealt the killing blow
    // OnSpotPlayer payload (set only while firing NodeType::OnSpotPlayer).
    entt::entity eventSpotter = entt::null;      // the AI that spotted someone
    entt::entity eventSpotTarget = entt::null;   // who it spotted
};
using CompiledFn = void (*)(CompiledContext& ctx, NodeType evt);

// Registers a baked graph under its asset key ("Schematics/foo.hbschem"). Called
// by the generated RegisterBakedSchematics().
void RegisterCompiled(const char* asset, CompiledFn fn);
// The baked function for `asset`, or null when the graph was not baked.
CompiledFn FindCompiled(const std::string& asset);

// Defined by the per-executable baked translation unit: the generated file in a
// baked runtime, an empty stub everywhere else. Engine init calls it once.
void RegisterBakedSchematics();

// --- Value helpers shared by the interpreter AND the generated C++ -----------
// Float view of a Value (bools read as 0/1), matching the VM's input coercion.
inline f32 BakedF(const Value& v) { return v.type == PinType::Bool ? (v.b ? 1.0f : 0.0f) : v.f; }
// Bool view of a Value (non-zero floats are true).
inline bool BakedB(const Value& v) { return v.type == PinType::Bool ? v.b : (v.f != 0.0f); }
// Entity an Entity-typed Value refers to; an unset/invalid one resolves to `self`.
inline entt::entity BakedEnt(const Value& v, entt::entity self) {
    return (v.type == PinType::Entity && v.entity != 0xFFFFFFFFu)
               ? static_cast<entt::entity>(v.entity)
               : self;
}
// Maps a key name ("W", "Space", "Left", ...) to a Key (KeyDown nodes / generated code).
Key KeyFromName(const std::string& s);

} // namespace schematic
} // namespace hbe

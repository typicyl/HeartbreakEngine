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
// Entity an Entity-typed Value refers to. An UNSET pin resolves to `self` (an
// unconnected Entity input operates on the graph's own entity, which is the
// documented behaviour). A pin that IS set but names an entity that no longer exists
// resolves to entt::null - never to `self`, because "kill the target" must not become
// "kill myself" the moment the target despawns.
//
// WHY THE REGISTRY IS A PARAMETER. `Value::entity` is raw u32 handle bits with no
// version discipline of its own, and it can be carried across frames (a stored
// variable, an OnDeath instigator payload). EnTT bumps an id's version on destroy, so
// the stale handle reads as INVALID rather than as a different object - but only if
// somebody asks. entt::registry::try_get on an invalid handle is an ENTT_ASSERT in
// Debug and an out-of-bounds sparse-set index in Release. Despawning shards turns
// that from a latent hazard into a routine one, so the check lives here, at the one
// place both the interpreter and the transpiled C++ resolve a pin.
inline entt::entity BakedEnt(const entt::registry& reg, const Value& v, entt::entity self) {
    if (v.type == PinType::Entity && v.entity != 0xFFFFFFFFu) {
        const entt::entity e = static_cast<entt::entity>(v.entity);
        return reg.valid(e) ? e : entt::null;
    }
    return self;
}

// Component lookup that tolerates a null/dead handle. Every schematic node that
// touches a component goes through this rather than raw try_get, for the reason
// above. Both overloads, because the interpreter reads through a const registry for
// its pure nodes and writes through a mutable one for its exec nodes.
template <typename T>
inline T* BakedGet(entt::registry& reg, entt::entity e) {
    return (e != entt::null && reg.valid(e)) ? reg.try_get<T>(e) : nullptr;
}
template <typename T>
inline const T* BakedGet(const entt::registry& reg, entt::entity e) {
    return (e != entt::null && reg.valid(e)) ? reg.try_get<T>(e) : nullptr;
}
// Maps a key name ("W", "Space", "Left", ...) to a Key (KeyDown nodes / generated code).
Key KeyFromName(const std::string& s);

} // namespace schematic
} // namespace hbe

// Scene/WorldState.cpp
#include "Scene/WorldState.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <vector>

namespace hbe::world {
namespace {
using json = nlohmann::json;

State g_state;

// An entity takes part in world state only when it is NAMED (the stable key) and
// carries at least one component whose runtime state a player can change. Purely
// decorative geometry is skipped, which keeps a captured area small.
bool Persistable(const entt::registry& reg, entt::entity e) {
    if (!reg.all_of<Name>(e)) return false;
    return reg.any_of<Interactable, TriggerVolume, Health>(e);
}

// Destroys `e` and its whole subtree. A looted pickup or a killed NPC is usually
// a parent with mesh/collider children, so destroying just the root would leave
// the visible part behind. Mirrors Editor::DestroyRecursive.
void DestroySubtree(entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return;
    // Copy the child list first: destroying mutates the Parent storage we scan.
    std::vector<entt::entity> children;
    for (const entt::entity c : reg.view<Parent>())
        if (reg.get<Parent>(c).entity == e) children.push_back(c);
    for (const entt::entity c : children) DestroySubtree(reg, c);
    if (reg.valid(e)) reg.destroy(e);
}

} // namespace

bool State::Visited(const std::string& area) const {
    const AreaState* a = Find(area);
    return a && a->visits > 0;
}

u32 State::VisitCount(const std::string& area) const {
    const AreaState* a = Find(area);
    return a ? a->visits : 0u;
}

void State::SetVar(const std::string& area, const std::string& name, f32 value) {
    if (area.empty() || name.empty()) return;
    Area(area).vars[name] = value;
}

f32 State::GetVar(const std::string& area, const std::string& name) const {
    const AreaState* a = Find(area);
    if (!a) return 0.0f;
    const auto it = a->vars.find(name);
    return it != a->vars.end() ? it->second : 0.0f;
}

AreaState& State::Area(const std::string& area) { return areas_[area]; }

const AreaState* State::Find(const std::string& area) const {
    const auto it = areas_.find(area);
    return it != areas_.end() ? &it->second : nullptr;
}

void State::Clear() { areas_.clear(); }

std::string State::Serialize() const {
    json root = json::object();
    json jareas = json::object();
    for (const auto& [name, a] : areas_) {
        json ja;
        ja["visits"] = a.visits;
        ja["captured"] = a.captured;
        if (a.captured) {
            json present = json::array();
            for (const std::string& n : a.present) present.push_back(n);
            ja["present"] = std::move(present);
        }
        if (!a.vars.empty()) {
            json vars = json::object();
            for (const auto& [k, v] : a.vars) vars[k] = v;
            ja["vars"] = std::move(vars);
        }
        if (!a.entities.empty()) {
            json ents = json::object();
            for (const auto& [k, es] : a.entities) {
                json je;
                if (es.interacted) je["interacted"] = true;
                if (es.triggered) je["triggered"] = true;
                if (es.hasHealth) {
                    je["health"] = es.health;
                    je["alive"] = es.alive;
                }
                if (!je.empty()) ents[k] = std::move(je);
            }
            if (!ents.empty()) ja["entities"] = std::move(ents);
        }
        jareas[name] = std::move(ja);
    }
    root["areas"] = std::move(jareas);
    return root.dump();
}

void State::Deserialize(const std::string& text) {
    areas_.clear();
    if (text.empty()) return;
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        HBE_WARN("WorldState: could not parse saved world state; starting fresh.");
        return;
    }
    const auto ait = root.find("areas");
    if (ait == root.end() || !ait->is_object()) return;
    for (const auto& [name, ja] : ait->items()) {
        AreaState a;
        a.visits = ja.value("visits", 0u);
        a.captured = ja.value("captured", false);
        if (const auto p = ja.find("present"); p != ja.end() && p->is_array())
            for (const json& n : *p)
                if (n.is_string()) a.present.insert(n.get<std::string>());
        if (const auto v = ja.find("vars"); v != ja.end() && v->is_object())
            for (const auto& [k, jv] : v->items())
                if (jv.is_number()) a.vars[k] = jv.get<f32>();
        if (const auto e = ja.find("entities"); e != ja.end() && e->is_object()) {
            for (const auto& [k, je] : e->items()) {
                EntityState es;
                es.interacted = je.value("interacted", false);
                es.triggered = je.value("triggered", false);
                if (const auto h = je.find("health"); h != je.end() && h->is_number()) {
                    es.hasHealth = true;
                    es.health = h->get<f32>();
                    es.alive = je.value("alive", true);
                }
                a.entities[k] = es;
            }
        }
        areas_[name] = std::move(a);
    }
}

State& Get() { return g_state; }

namespace {
std::string g_currentArea;
} // namespace

void SetCurrentArea(const std::string& area) { g_currentArea = area; }
const std::string& CurrentArea() { return g_currentArea; }

const std::string& ResolveArea(const std::string& area) {
    return area.empty() ? g_currentArea : area;
}

std::string AreaIdFromPath(const std::filesystem::path& p) {
    std::string s = p.stem().string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void CaptureArea(const Scene& scene, const std::string& area) {
    if (area.empty()) return;
    AreaState& a = g_state.Area(area);
    a.present.clear();
    a.entities.clear();
    a.captured = true;

    const entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<const Name>()) {
        if (!Persistable(reg, e)) continue;
        const std::string& name = reg.get<const Name>(e).value;
        if (name.empty()) continue;
        a.present.insert(name);

        EntityState es;
        if (const Interactable* ia = reg.try_get<const Interactable>(e)) es.interacted = ia->fired;
        if (const TriggerVolume* tv = reg.try_get<const TriggerVolume>(e)) es.triggered = tv->fired;
        if (const Health* h = reg.try_get<const Health>(e)) {
            es.hasHealth = true;
            es.health = h->current;
            es.alive = h->alive;
        }
        // Only store rows that actually carry a change worth replaying.
        if (es.interacted || es.triggered || es.hasHealth) a.entities[name] = es;
    }
    HBE_INFO("WorldState: captured area '{}' ({} tracked, {} with deltas).", area,
             a.present.size(), a.entities.size());
}

void RestoreArea(Scene& scene, const std::string& area) {
    if (area.empty()) return;
    SetCurrentArea(area); // script nodes can now pass "" to mean "here"
    AreaState& a = g_state.Area(area);
    ++a.visits;
    if (!a.captured) return; // first visit: nothing to replay

    entt::registry& reg = scene.Registry();

    // Pass 1: collect the freshly loaded persistable entities by name. Doing this
    // up front means the destroy pass below cannot invalidate the view mid-walk.
    std::vector<std::pair<std::string, entt::entity>> loaded;
    for (const entt::entity e : reg.view<const Name>()) {
        if (!Persistable(reg, e)) continue;
        const std::string& name = reg.get<const Name>(e).value;
        if (!name.empty()) loaded.emplace_back(name, e);
    }

    usize destroyed = 0, restored = 0;
    for (const auto& [name, e] : loaded) {
        // Anything the authored area has that was NOT alive at capture time was
        // destroyed during the visit (looted, killed, consumed) - destroy it again
        // so the world stays as the player left it.
        if (!a.present.count(name)) {
            if (reg.valid(e)) {
                DestroySubtree(reg, e); // takes mesh/collider children with it
                ++destroyed;
            }
            continue;
        }
        const auto it = a.entities.find(name);
        if (it == a.entities.end()) continue;
        const EntityState& es = it->second;
        if (Interactable* ia = reg.try_get<Interactable>(e)) ia->fired = es.interacted;
        if (TriggerVolume* tv = reg.try_get<TriggerVolume>(e)) {
            tv->fired = es.triggered;
            tv->inside = false; // re-arm the enter edge; the player is not inside yet
        }
        if (Health* h = reg.try_get<Health>(e); h && es.hasHealth) {
            h->current = glm::clamp(es.health, 0.0f, h->max);
            h->alive = es.alive;
            // A corpse restored as dead must not re-run its one-shot death
            // reactions (flags/objectives/OnDeath) a second time on revisit.
            h->deathDispatched = !es.alive;
        }
        ++restored;
    }
    HBE_INFO("WorldState: restored area '{}' (visit {}; {} re-destroyed, {} re-applied).", area,
             a.visits, destroyed, restored);
}

} // namespace hbe::world

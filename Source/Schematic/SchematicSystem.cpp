// Schematic/SchematicSystem.cpp - the visual-script interpreter + ECS driver.
#include "Schematic/SchematicSystem.h"

#include "Core/Input.h"
#include "Core/Log.h"
#include "Game/CombatSystem.h"
#include "Game/GameSystems.h"
#include "Project/Project.h"
#include "Scene/Components.h"
#include "Scene/FacialSystem.h"
#include "Scene/Scene.h"
#include "Scene/WorldState.h" // area visit counts + per-area vars (World nodes)
#include "Schematic/Schematic.h"

#include <glm/glm.hpp>

#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace hbe::schematic {

// --- Compiled-schematic registry (baked native graphs) ----------------------
namespace {
std::unordered_map<std::string, CompiledFn> g_compiled;
}
void RegisterCompiled(const char* asset, CompiledFn fn) {
    if (asset && fn) g_compiled[asset] = fn;
}
CompiledFn FindCompiled(const std::string& asset) {
    auto it = g_compiled.find(asset);
    return it != g_compiled.end() ? it->second : nullptr;
}

Key KeyFromName(const std::string& s) {
    if (s.size() == 1) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        if (c >= 'A' && c <= 'Z') return static_cast<Key>(static_cast<u8>(Key::A) + (c - 'A'));
        if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<u8>(Key::Num0) + (c - '0'));
    }
    std::string u;
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "SPACE") return Key::Space;
    if (u == "ENTER" || u == "RETURN") return Key::Enter;
    if (u == "ESC" || u == "ESCAPE") return Key::Escape;
    if (u == "SHIFT") return Key::Shift;
    if (u == "CTRL") return Key::Ctrl;
    if (u == "ALT") return Key::Alt;
    if (u == "TAB") return Key::Tab;
    if (u == "LEFT") return Key::Left;
    if (u == "RIGHT") return Key::Right;
    if (u == "UP") return Key::Up;
    if (u == "DOWN") return Key::Down;
    return Key::Unknown;
}

namespace {

// Asset path -> loaded graph (shared, read-only at runtime). Misses cache as empty.
std::unordered_map<std::string, Graph> g_cache;

const Graph* GetGraph(const std::string& asset) {
    if (asset.empty()) return nullptr;
    if (auto it = g_cache.find(asset); it != g_cache.end()) return &it->second;
    Graph g;
    const std::filesystem::path path = Project::HasActive()
                                           ? Project::Active().AssetsDir() / asset
                                           : std::filesystem::path(asset);
    LoadGraph(path, g); // empty graph on failure (cached so we don't re-hit disk)
    auto [ins, ok] = g_cache.emplace(asset, std::move(g));
    return &ins->second;
}

// One UI interaction this frame (a clicked button / a changed widget), collected
// once from the UIElements and fired into every schematic as a UI event.
struct UIEventRec {
    std::string action; // the widget's UIElement.action id
    bool clicked = false;
    bool changed = false;
    f32 value = 0.0f;
    bool toggled = false;
    f32 selected = 0.0f;
};

// Evaluates + executes one graph for one entity for one frame.
class VM {
public:
    VM(const Graph& g, Scene& scene, Input& input, entt::entity self, f32 dt,
       SchematicComponent& inst)
        : g_(g), scene_(scene), input_(input), self_(self), dt_(dt), inst_(inst) {}

    void RunEvent(NodeType evt) {
        dataCache_.clear();
        for (const Node& n : g_.nodes)
            if (n.type == evt) { Follow(n.id, 0, 0); break; }
    }

    // UI events fire EVERY matching event node (unlike RunEvent's first-only): a
    // graph can listen to many widgets. A node fires when its Action filter (input
    // pin 0 literal/wire) is empty or equals the event's action id.
    void RunEventUI(NodeType evt, const UIEventRec& rec) {
        evtAction_ = rec.action;
        evtValue_ = rec.value;
        evtToggled_ = rec.toggled;
        evtSelected_ = rec.selected;
        for (const Node& n : g_.nodes) {
            if (n.type != evt) continue;
            dataCache_.clear(); // per firing: the event outputs feed fresh values
            const std::string filter = EvalInput(n.id, 0, 0).s;
            if (filter.empty() || filter == rec.action) Follow(n.id, 0, 0);
        }
    }

    // Combat deaths fire EVERY OnDeath node whose Tag filter matches (empty = any),
    // mirroring RunEventUI. The Tag + Instigator data outputs read the payload below.
    void RunEventDeath(const game::DeathRec& rec) {
        evtDeathTag_ = rec.tag;
        evtInstigator_ = rec.instigator;
        for (const Node& n : g_.nodes) {
            if (n.type != NodeType::OnDeath) continue;
            dataCache_.clear();
            const std::string filter = EvalInput(n.id, 0, 0).s;
            if (filter.empty() || filter == rec.tag) Follow(n.id, 0, 0);
        }
    }

    // Fires every OnSpotPlayer node (the engine only invokes this on the spotter's
    // own graph, so no filter is needed). Spotter + Target read the payload below.
    void RunEventSpotted(const game::SpottedRec& rec) {
        evtSpotter_ = rec.spotter;
        evtSpotTarget_ = rec.target;
        for (const Node& n : g_.nodes) {
            if (n.type != NodeType::OnSpotPlayer) continue;
            dataCache_.clear();
            Follow(n.id, 0, 0);
        }
    }

private:
    const Graph& g_;
    Scene& scene_;
    Input& input_;
    entt::entity self_;
    f32 dt_;
    SchematicComponent& inst_;
    std::unordered_map<u64, Value> dataCache_;
    // Payload of the UI event currently firing (RunEventUI).
    std::string evtAction_;
    f32 evtValue_ = 0.0f;
    bool evtToggled_ = false;
    f32 evtSelected_ = 0.0f;
    // Payload of the death event currently firing (RunEventDeath).
    std::string evtDeathTag_;
    u32 evtInstigator_ = 0xFFFFFFFFu;
    // Payload of the spotted event currently firing (RunEventSpotted).
    u32 evtSpotter_ = 0xFFFFFFFFu;
    u32 evtSpotTarget_ = 0xFFFFFFFFu;

    // An UNSET Entity input operates on this entity; a SET one that names a dead
    // entity resolves to entt::null, never to self_ (see BakedEnt in the header - a
    // Kill aimed at a despawned target must not turn into a Kill aimed at the
    // caller). Every component read below goes through Get<T>, which tolerates null.
    entt::entity Resolve(const Value& v) const {
        return schematic::BakedEnt(scene_.Registry(), v, self_);
    }
    // Component lookup that tolerates a null/dead handle (shared with the transpiled
    // C++ so the two paths cannot disagree).
    template <typename T>
    T* Get(entt::entity e) const {
        return schematic::BakedGet<T>(scene_.Registry(), e);
    }

    // The value feeding an input pin: the wired output, else the literal default.
    Value EvalInput(u32 nodeId, u32 inPin, int depth) {
        for (const Link& l : g_.links)
            if (l.toNode == nodeId && l.toPin == inPin)
                return EvalOutput(l.fromNode, l.fromPin, depth + 1);
        const Node* n = g_.Find(nodeId);
        if (n && inPin < n->literals.size()) return n->literals[inPin];
        return {};
    }
    f32 InF(u32 id, u32 p, int d) {
        Value v = EvalInput(id, p, d);
        return v.type == PinType::Bool ? (v.b ? 1.0f : 0.0f) : v.f;
    }
    bool InB(u32 id, u32 p, int d) {
        Value v = EvalInput(id, p, d);
        return v.type == PinType::Bool ? v.b : (v.f != 0.0f);
    }

    Value EvalOutput(u32 nodeId, u32 outPin, int depth) {
        if (depth > 256) return {};
        const u64 key = (static_cast<u64>(nodeId) << 32) | outPin;
        if (auto it = dataCache_.find(key); it != dataCache_.end()) return it->second;
        Value out;
        if (const Node* n = g_.Find(nodeId)) out = Compute(*n, outPin, depth);
        dataCache_[key] = out;
        return out;
    }

    Value Compute(const Node& n, u32 outPin, int d) {
        switch (n.type) {
            case NodeType::LiteralFloat:  return Value::Float(InF(n.id, 0, d));
            case NodeType::LiteralBool:   return Value::Bool(InB(n.id, 0, d));
            case NodeType::LiteralString: return EvalInput(n.id, 0, d);
            case NodeType::LiteralVec3:   return Value::Vec3({InF(n.id, 0, d), InF(n.id, 1, d), InF(n.id, 2, d)});
            case NodeType::Add:      return Value::Float(InF(n.id, 0, d) + InF(n.id, 1, d));
            case NodeType::Subtract: return Value::Float(InF(n.id, 0, d) - InF(n.id, 1, d));
            case NodeType::Multiply: return Value::Float(InF(n.id, 0, d) * InF(n.id, 1, d));
            case NodeType::Divide:   { f32 b = InF(n.id, 1, d); return Value::Float(b != 0.0f ? InF(n.id, 0, d) / b : 0.0f); }
            case NodeType::Greater:  return Value::Bool(InF(n.id, 0, d) >  InF(n.id, 1, d));
            case NodeType::Less:     return Value::Bool(InF(n.id, 0, d) <  InF(n.id, 1, d));
            case NodeType::EqualF:   return Value::Bool(InF(n.id, 0, d) == InF(n.id, 1, d));
            case NodeType::AndB:     return Value::Bool(InB(n.id, 0, d) && InB(n.id, 1, d));
            case NodeType::OrB:      return Value::Bool(InB(n.id, 0, d) || InB(n.id, 1, d));
            case NodeType::NotB:     return Value::Bool(!InB(n.id, 0, d));
            case NodeType::MakeVec3: return Value::Vec3({InF(n.id, 0, d), InF(n.id, 1, d), InF(n.id, 2, d)});
            case NodeType::BreakVec3: {
                glm::vec3 v = EvalInput(n.id, 0, d).v3;
                return Value::Float(outPin == 0 ? v.x : outPin == 1 ? v.y : v.z);
            }
            case NodeType::Self:         return Value::Ent(static_cast<u32>(self_));
            case NodeType::GetDeltaTime: return Value::Float(dt_);
            case NodeType::EventUpdate:  return Value::Float(dt_); // Delta data output
            case NodeType::KeyDown:      return Value::Bool(input_.IsKeyDown(KeyFromName(EvalInput(n.id, 0, d).s)));
            case NodeType::GetVar: {
                auto it = inst_.vars.find(EvalInput(n.id, 0, d).s);
                return it != inst_.vars.end() ? it->second : Value::Float(0.0f);
            }
            case NodeType::GetPosition: {
                entt::entity e = Resolve(EvalInput(n.id, 0, d));
                const Transform* t = Get<Transform>(e);
                return Value::Vec3(t ? t->position : glm::vec3(0.0f));
            }
            case NodeType::EventUIClicked: // Action data output (valid while firing)
                return Value::Str(evtAction_);
            case NodeType::EventUIChanged: // Value / Toggled / Selected outputs
                return outPin == 1   ? Value::Float(evtValue_)
                       : outPin == 2 ? Value::Bool(evtToggled_)
                                     : Value::Float(evtSelected_);
            case NodeType::GetUIValue: { // live widget state, addressed by action id
                const std::string name = EvalInput(n.id, 0, d).s;
                Value out = Value::Float(0.0f);
                bool found = false;
                if (!name.empty()) {
                    scene_.Registry().view<UIElement>().each([&](const UIElement& el) {
                        if (found || el.action != name) return; // first match wins
                        found = true;
                        out = outPin == 0   ? Value::Float(el.value)
                              : outPin == 1 ? Value::Bool(el.toggled)
                                            : Value::Float(static_cast<f32>(el.selected));
                    });
                }
                return out;
            }
            case NodeType::IsAlive:
                return Value::Bool(combat::IsAlive(scene_, Resolve(EvalInput(n.id, 0, d))));
            case NodeType::GetHealth: {
                const Health* h = Get<Health>(Resolve(EvalInput(n.id, 0, d)));
                return Value::Float(h ? (outPin == 1 ? h->max : h->current) : 0.0f);
            }
            case NodeType::OnDeath: // Tag(pin1) + Instigator(pin2) payload (while firing)
                return outPin == 2 ? Value::Ent(evtInstigator_) : Value::Str(evtDeathTag_);
            case NodeType::IsPlayerVisible: {
                const AIPerception* p =
                    Get<AIPerception>(Resolve(EvalInput(n.id, 0, d)));
                return Value::Bool(p && p->canSeeTarget);
            }
            case NodeType::GetAwareness: {
                const AIPerception* p =
                    Get<AIPerception>(Resolve(EvalInput(n.id, 0, d)));
                return Value::Float(p ? p->awareness : 0.0f);
            }
            case NodeType::OnSpotPlayer: // Spotter(pin1) + Target(pin2) payload
                return outPin == 2 ? Value::Ent(evtSpotTarget_) : Value::Ent(evtSpotter_);
            case NodeType::AliveCount: {
                const std::string id = EvalInput(n.id, 0, d).s;
                f32 count = 0.0f;
                for (auto ee : scene_.Registry().view<Encounter>())
                    if (scene_.Registry().get<Encounter>(ee).id == id) {
                        count = static_cast<f32>(scene_.Registry().get<Encounter>(ee).aliveCount);
                        break;
                    }
                return Value::Float(count);
            }
            case NodeType::HasItem:
                return Value::Bool(game::HasItem(EvalInput(n.id, 0, d).s,
                                                 static_cast<u32>(glm::max(0.0f, InF(n.id, 1, d)))));
            case NodeType::ItemCount:
                return Value::Float(static_cast<f32>(game::ItemCount(EvalInput(n.id, 0, d).s)));
            case NodeType::AreaVisitCount: {
                const u32 visits =
                    world::Get().VisitCount(world::ResolveArea(EvalInput(n.id, 0, d).s));
                // Pin 1 "First Visit" is true DURING the first visit (visits == 1),
                // which is what revisit scripting actually asks.
                return outPin == 1 ? Value::Bool(visits <= 1)
                                   : Value::Float(static_cast<f32>(visits));
            }
            case NodeType::GetAreaVar:
                return Value::Float(world::Get().GetVar(world::ResolveArea(EvalInput(n.id, 0, d).s),
                                                        EvalInput(n.id, 1, d).s));
            default: return {};
        }
    }

    // Follow the exec wire out of (nodeId, outExecPin) and run the target node.
    void Follow(u32 nodeId, u32 outPin, int depth) {
        if (depth > 4096) return; // runaway guard (e.g. an exec cycle)
        for (const Link& l : g_.links)
            if (l.fromNode == nodeId && l.fromPin == outPin) {
                ExecNode(l.toNode, depth + 1);
                return;
            }
    }

    void ExecNode(u32 nodeId, int depth) {
        const Node* np = g_.Find(nodeId);
        if (!np) return;
        const Node& n = *np;
        switch (n.type) {
            case NodeType::Print:
                HBE_INFO("[Schematic] {}", EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::Branch:
                Follow(n.id, InB(n.id, 1, 0) ? 0u : 1u, depth);
                break;
            case NodeType::Sequence:
                Follow(n.id, 0, depth);
                Follow(n.id, 1, depth);
                break;
            case NodeType::SetVar:
                inst_.vars[EvalInput(n.id, 1, 0).s] = EvalInput(n.id, 2, 0);
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetPosition: {
                entt::entity e = Resolve(EvalInput(n.id, 1, 0));
                if (Transform* t = Get<Transform>(e))
                    t->position = EvalInput(n.id, 2, 0).v3;
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::Translate: {
                entt::entity e = Resolve(EvalInput(n.id, 1, 0));
                if (Transform* t = Get<Transform>(e))
                    t->position += EvalInput(n.id, 2, 0).v3;
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::Delay: {
                // Cooldown gate: passes exec, then blocks for `Seconds` (per node).
                f32& timer = inst_.timers[n.id];
                if (timer <= 0.0f) {
                    timer = glm::max(0.01f, EvalInput(n.id, 1, 0).f);
                    Follow(n.id, 0, depth);
                }
                break;
            }
            case NodeType::ReachCheckpoint:
                game::ReachCheckpoint(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetObjective:
                game::SetObjective(EvalInput(n.id, 1, 0).s, EvalInput(n.id, 2, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::CompleteObjective:
                game::CompleteObjective(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::ApplyDamage: {
                combat::DamageEvent ev;
                ev.target = Resolve(EvalInput(n.id, 1, 0));
                ev.instigator = self_;
                ev.amount = InF(n.id, 2, 0);
                combat::ApplyDamage(scene_, ev, nullptr); // no knockback from script
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::Kill:
                combat::Kill(scene_, Resolve(EvalInput(n.id, 1, 0)));
                Follow(n.id, 0, depth);
                break;
            case NodeType::Heal:
                combat::Heal(scene_, Resolve(EvalInput(n.id, 1, 0)), InF(n.id, 2, 0));
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetHealth: {
                if (Health* h = Get<Health>(Resolve(EvalInput(n.id, 1, 0)))) {
                    h->current = glm::clamp(InF(n.id, 2, 0), 0.0f, h->max);
                    if (h->current <= 0.0f) h->alive = false;
                    else if (!h->alive) { h->alive = true; h->deathDispatched = false; } // revive
                }
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::SetInvulnerable: {
                if (Health* h = Get<Health>(Resolve(EvalInput(n.id, 1, 0))))
                    h->invincible = InB(n.id, 2, 0);
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::SetAIState: {
                if (AIBehavior* b = Get<AIBehavior>(Resolve(EvalInput(n.id, 1, 0))))
                    b->state = AIStateFromName(EvalInput(n.id, 2, 0).s);
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::SetAlert: {
                if (AIPerception* p =
                        Get<AIPerception>(Resolve(EvalInput(n.id, 1, 0))))
                    p->awareness = glm::clamp(InF(n.id, 2, 0), 0.0f, 1.0f);
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::SpawnGroup: {
                const std::string id = EvalInput(n.id, 1, 0).s;
                for (auto se : scene_.Registry().view<Spawner>())
                    if (scene_.Registry().get<Spawner>(se).spawnerId == id)
                        scene_.Registry().get<Spawner>(se).spawnRequested = true;
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::DespawnAll: {
                const std::string id = EvalInput(n.id, 1, 0).s;
                for (auto se : scene_.Registry().view<Spawner>())
                    if (scene_.Registry().get<Spawner>(se).encounterId == id)
                        scene_.Registry().get<Spawner>(se).despawnRequested = true;
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::GrantItem:
                game::AddItem(EvalInput(n.id, 1, 0).s,
                              static_cast<u32>(glm::max(0.0f, InF(n.id, 2, 0))));
                Follow(n.id, 0, depth);
                break;
            case NodeType::RemoveItem:
                game::RemoveItem(EvalInput(n.id, 1, 0).s,
                                 static_cast<u32>(glm::max(0.0f, InF(n.id, 2, 0))));
                Follow(n.id, 0, depth);
                break;
            case NodeType::EquipWeapon:
                game::EquipWeapon(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetMorphWeight: {
                if (MorphState* ms =
                        facial::ResolveMorphTarget(scene_, Resolve(EvalInput(n.id, 1, 0)))) {
                    const std::string name = EvalInput(n.id, 2, 0).s;
                    if (!name.empty()) ms->weights[name] = glm::clamp(InF(n.id, 3, 0), 0.0f, 1.0f);
                }
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::PlayFacialExpression: {
                if (FacialAnimator* fa =
                        Get<FacialAnimator>(Resolve(EvalInput(n.id, 1, 0)))) {
                    fa->expression = EvalInput(n.id, 2, 0).s;
                    fa->expressionWeight = glm::clamp(InF(n.id, 3, 0), 0.0f, 1.0f);
                }
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::SetAreaVar:
                world::Get().SetVar(world::ResolveArea(EvalInput(n.id, 1, 0).s),
                                    EvalInput(n.id, 2, 0).s, InF(n.id, 3, 0));
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetMusicState:
                game::SetMusicState(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::SetMusicParam:
                game::SetMusicParameter(EvalInput(n.id, 1, 0).s, EvalInput(n.id, 2, 0).f);
                Follow(n.id, 0, depth);
                break;
            case NodeType::PlayStinger:
                game::PlayStinger(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::PlayVoiceline:
                game::PlayVoiceline(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::PlayDialogue:
                game::PlayDialogue(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::PlayCutscene:
                game::PlayCutscene(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::PlaySequence:
                game::PlaySequence(EvalInput(n.id, 1, 0).s);
                Follow(n.id, 0, depth);
                break;
            case NodeType::SpawnEffect:
                game::SpawnEffect(EvalInput(n.id, 1, 0).s, EvalInput(n.id, 2, 0).v3);
                Follow(n.id, 0, depth);
                break;
            // Panel ops are deferred (the engine owns the UIManager); element setters
            // write the registry directly (SetPosition precedent) - schematics run
            // before BuildVertices, so the change shows the same frame.
            case NodeType::UIShowPanel:
                game::QueueUICommand({game::UICommand::Op::Show, EvalInput(n.id, 1, 0).s});
                Follow(n.id, 0, depth);
                break;
            case NodeType::UIPushPanel:
                game::QueueUICommand({game::UICommand::Op::Push, EvalInput(n.id, 1, 0).s});
                Follow(n.id, 0, depth);
                break;
            case NodeType::UIPopPanel:
                game::QueueUICommand({game::UICommand::Op::Pop, {}});
                Follow(n.id, 0, depth);
                break;
            case NodeType::UISetText: {
                const std::string name = EvalInput(n.id, 1, 0).s;
                const std::string text = EvalInput(n.id, 2, 0).s;
                if (!name.empty())
                    scene_.Registry().view<UIElement>().each([&](UIElement& el) {
                        if (el.action == name) el.text = text; // template: tokens still apply
                    });
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::UISetVisible: {
                const std::string name = EvalInput(n.id, 1, 0).s;
                const bool vis = InB(n.id, 2, 0);
                if (!name.empty())
                    scene_.Registry().view<UIElement>().each([&](UIElement& el) {
                        if (el.action == name) el.visible = vis; // hides its subtree too
                    });
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::UISetValue: {
                // Type-aware: one node drives slider (value), toggle (>0.5), selector
                // (index). Does NOT raise `changed` - no event feedback loop.
                const std::string name = EvalInput(n.id, 1, 0).s;
                const f32 v = InF(n.id, 2, 0);
                if (!name.empty())
                    scene_.Registry().view<UIElement>().each([&](UIElement& el) {
                        if (el.action != name) return;
                        el.value = glm::clamp(v, 0.0f, 1.0f);
                        el.toggled = v > 0.5f;
                        if (!el.options.empty())
                            el.selected = glm::clamp(static_cast<int>(v), 0,
                                                     static_cast<int>(el.options.size()) - 1);
                    });
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::UIPlayAnim: {
                // Restart Manual-trigger animators on matching elements (mirrors the
                // UIManager's OnShow restart).
                const std::string name = EvalInput(n.id, 1, 0).s;
                if (!name.empty()) {
                    auto& reg = scene_.Registry();
                    reg.view<UIElement, UIAnimator>().each(
                        [&](UIElement& el, UIAnimator& an) {
                            if (el.action != name ||
                                an.trigger != UIAnimator::Trigger::Manual)
                                return;
                            an.time = 0.0f;
                            an.playing = true;
                            an.captured = false; // re-capture the base offset
                        });
                }
                Follow(n.id, 0, depth);
                break;
            }
            default:
                Follow(n.id, 0, depth);
                break;
        }
    }
};

} // namespace

void Update(Scene& scene, Input& input, f32 dt, bool playing) {
    if (!playing) return;
    auto& reg = scene.Registry();

    // UI interactions this frame (flags were set by ui::UpdateInteraction, which
    // runs earlier in the frame): collected ONCE, then fired into every schematic
    // below as On UI Clicked / On UI Changed events.
    std::vector<UIEventRec> uiEvents;
    reg.view<UIElement>().each([&](const UIElement& el) {
        if (el.action.empty() || (!el.clicked && !el.changed)) return;
        UIEventRec rec;
        rec.action = el.action;
        rec.clicked = el.clicked;
        rec.changed = el.changed;
        rec.value = el.value;
        rec.toggled = el.toggled;
        rec.selected = static_cast<f32>(el.selected);
        uiEvents.push_back(std::move(rec));
    });

    // Combat deaths flagged since the last tick (combat::Update queues them): drain
    // ONCE here, then fan each out to every graph's OnDeath event below. Draining
    // unconditionally means the queue can't grow when no graph is listening.
    std::vector<game::DeathRec> deaths;
    { game::DeathRec d; while (game::ConsumeDeath(d)) deaths.push_back(std::move(d)); }
    // AI "spotted the player" edges: drained once, fired only on the SPOTTER's graph.
    std::vector<game::SpottedRec> spots;
    { game::SpottedRec s; while (game::ConsumeSpotted(s)) spots.push_back(s); }

    for (const entt::entity e : reg.view<SchematicComponent>()) {
        SchematicComponent& sc = reg.get<SchematicComponent>(e);
        for (auto& [id, t] : sc.timers)
            if (t > 0.0f) t = glm::max(0.0f, t - dt); // tick Delay cooldowns

        // Compiled (transpiled-to-C++) graph: run it natively, no interpreter.
        if (CompiledFn fn = FindCompiled(sc.asset)) {
            CompiledContext ctx{scene, input, e, dt, sc};
            if (!sc.started) {
                sc.started = true;
                fn(ctx, NodeType::EventStart);
            }
            fn(ctx, NodeType::EventUpdate);
            for (const UIEventRec& rec : uiEvents) { // UI events, payload in ctx
                ctx.eventAction = &rec.action;
                ctx.eventValue = rec.value;
                ctx.eventToggled = rec.toggled;
                ctx.eventSelected = rec.selected;
                if (rec.clicked) fn(ctx, NodeType::EventUIClicked);
                if (rec.changed) fn(ctx, NodeType::EventUIChanged);
            }
            ctx.eventAction = nullptr;
            for (const game::DeathRec& d : deaths) { // OnDeath, payload in ctx
                ctx.eventDeathTag = &d.tag;
                ctx.eventInstigator = d.instigator != 0xFFFFFFFFu
                                          ? static_cast<entt::entity>(d.instigator)
                                          : entt::null;
                fn(ctx, NodeType::OnDeath);
            }
            ctx.eventDeathTag = nullptr;
            for (const game::SpottedRec& sp : spots) { // OnSpotPlayer on the spotter only
                if (sp.spotter != static_cast<u32>(e)) continue;
                ctx.eventSpotter = static_cast<entt::entity>(sp.spotter);
                ctx.eventSpotTarget = sp.target != 0xFFFFFFFFu
                                          ? static_cast<entt::entity>(sp.target)
                                          : entt::null;
                fn(ctx, NodeType::OnSpotPlayer);
            }
            continue;
        }

        // Interpreter fallback (editor/dev, or a graph that wasn't baked).
        const Graph* g = GetGraph(sc.asset);
        if (!g || g->nodes.empty()) continue;
        VM vm(*g, scene, input, e, dt, sc);
        if (!sc.started) {
            sc.started = true;
            vm.RunEvent(NodeType::EventStart);
        }
        vm.RunEvent(NodeType::EventUpdate);
        for (const UIEventRec& rec : uiEvents) {
            if (rec.clicked) vm.RunEventUI(NodeType::EventUIClicked, rec);
            if (rec.changed) vm.RunEventUI(NodeType::EventUIChanged, rec);
        }
        for (const game::DeathRec& d : deaths) vm.RunEventDeath(d);
        for (const game::SpottedRec& sp : spots)
            if (sp.spotter == static_cast<u32>(e)) vm.RunEventSpotted(sp);
    }
}

void ClearCache() { g_cache.clear(); }

} // namespace hbe::schematic

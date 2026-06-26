// Schematic/SchematicSystem.cpp - the visual-script interpreter + ECS driver.
#include "Schematic/SchematicSystem.h"

#include "Core/Input.h"
#include "Core/Log.h"
#include "Game/GameSystems.h"
#include "Project/Project.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
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

private:
    const Graph& g_;
    Scene& scene_;
    Input& input_;
    entt::entity self_;
    f32 dt_;
    SchematicComponent& inst_;
    std::unordered_map<u64, Value> dataCache_;

    entt::entity Resolve(const Value& v) const {
        if (v.type == PinType::Entity && v.entity != 0xFFFFFFFFu)
            return static_cast<entt::entity>(v.entity);
        return self_; // unconnected/invalid Entity inputs operate on this entity
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
                const Transform* t = scene_.Registry().try_get<Transform>(e);
                return Value::Vec3(t ? t->position : glm::vec3(0.0f));
            }
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
                if (Transform* t = scene_.Registry().try_get<Transform>(e))
                    t->position = EvalInput(n.id, 2, 0).v3;
                Follow(n.id, 0, depth);
                break;
            }
            case NodeType::Translate: {
                entt::entity e = Resolve(EvalInput(n.id, 1, 0));
                if (Transform* t = scene_.Registry().try_get<Transform>(e))
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
    }
}

void ClearCache() { g_cache.clear(); }

} // namespace hbe::schematic

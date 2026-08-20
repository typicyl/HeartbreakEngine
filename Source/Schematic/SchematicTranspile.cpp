// Schematic/SchematicTranspile.cpp - Graph -> native C++ source generator.
#include "Schematic/SchematicTranspile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace hbe::schematic {

namespace {

// Float -> a valid C++ float literal that round-trips. "%g" drops the decimal
// point for whole numbers ("0", "1"), and `0f` is ill-formed, so force a fraction.
std::string Fnum(f32 v) {
    if (!std::isfinite(v)) return "0.0f";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0"; // 0 -> 0.0, 5 -> 5.0
    s += "f";
    return s;
}

// Escapes a string for a C++ "..." literal.
std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:   o += c; break;
        }
    }
    return o;
}

// A literal Value default -> a C++ expression of type Value.
std::string LiteralExpr(const Value& v) {
    switch (v.type) {
        case PinType::Bool:   return v.b ? "Value::Bool(true)" : "Value::Bool(false)";
        case PinType::Vec3:   return "Value::Vec3(glm::vec3(" + Fnum(v.v3.x) + ", " +
                                     Fnum(v.v3.y) + ", " + Fnum(v.v3.z) + "))";
        case PinType::String: return "Value::Str(\"" + Escape(v.s) + "\")";
        case PinType::Entity: return "Value::Ent(0xFFFFFFFFu)";
        case PinType::Exec:   return "Value{}";
        default:              return "Value::Float(" + Fnum(v.f) + ")";
    }
}

// Generates code for one graph. Data outputs become Value expressions (recursively
// inlined, mirroring the VM's EvalInput/Compute); the exec chain becomes nested
// statements. Cycles are broken structurally (data: depth cap; exec: active-stack).
struct Gen {
    const Graph& g;

    const Link* InLink(u32 node, u32 pin) const {
        for (const Link& l : g.links)
            if (l.toNode == node && l.toPin == pin) return &l;
        return nullptr;
    }

    // The Value feeding input pin (node,pin): the wired output, else the literal.
    std::string EmitInput(u32 node, u32 pin, int depth) const {
        if (depth > 256) return "Value{}";
        if (const Link* l = InLink(node, pin)) return EmitOutput(l->fromNode, l->fromPin, depth);
        const Node* n = g.Find(node);
        if (n && pin < n->literals.size()) return LiteralExpr(n->literals[pin]);
        return "Value{}";
    }

    // The Value produced by output pin (node,outPin) (a pure-node computation).
    std::string EmitOutput(u32 node, u32 outPin, int depth) const {
        if (depth > 256) return "Value{}";
        const Node* n = g.Find(node);
        if (!n) return "Value{}";
        auto IN = [&](u32 p) { return EmitInput(node, p, depth + 1); };
        switch (n->type) {
            case NodeType::LiteralFloat:  return "Value::Float(BakedF(" + IN(0) + "))";
            case NodeType::LiteralBool:   return "Value::Bool(BakedB(" + IN(0) + "))";
            case NodeType::LiteralString: return IN(0);
            case NodeType::LiteralVec3:   return "Value::Vec3(glm::vec3(BakedF(" + IN(0) +
                                                 "), BakedF(" + IN(1) + "), BakedF(" + IN(2) + ")))";
            case NodeType::Add:      return "Value::Float(BakedF(" + IN(0) + ") + BakedF(" + IN(1) + "))";
            case NodeType::Subtract: return "Value::Float(BakedF(" + IN(0) + ") - BakedF(" + IN(1) + "))";
            case NodeType::Multiply: return "Value::Float(BakedF(" + IN(0) + ") * BakedF(" + IN(1) + "))";
            case NodeType::Divide:   return "([&]{ float _b = BakedF(" + IN(1) +
                                            "); return Value::Float(_b != 0.0f ? BakedF(" + IN(0) +
                                            ") / _b : 0.0f); }())";
            case NodeType::Greater:  return "Value::Bool(BakedF(" + IN(0) + ") > BakedF(" + IN(1) + "))";
            case NodeType::Less:     return "Value::Bool(BakedF(" + IN(0) + ") < BakedF(" + IN(1) + "))";
            case NodeType::EqualF:   return "Value::Bool(BakedF(" + IN(0) + ") == BakedF(" + IN(1) + "))";
            case NodeType::AndB:     return "Value::Bool(BakedB(" + IN(0) + ") && BakedB(" + IN(1) + "))";
            case NodeType::OrB:      return "Value::Bool(BakedB(" + IN(0) + ") || BakedB(" + IN(1) + "))";
            case NodeType::NotB:     return "Value::Bool(!BakedB(" + IN(0) + "))";
            case NodeType::MakeVec3: return "Value::Vec3(glm::vec3(BakedF(" + IN(0) +
                                            "), BakedF(" + IN(1) + "), BakedF(" + IN(2) + ")))";
            case NodeType::BreakVec3: {
                const char* comp = outPin == 0 ? "x" : outPin == 1 ? "y" : "z";
                return std::string("Value::Float(([&]{ glm::vec3 _v = (") + IN(0) + ").v3; return _v." +
                       comp + "; })())";
            }
            case NodeType::Self:         return "Value::Ent(static_cast<u32>(ctx.self))";
            case NodeType::GetDeltaTime: return "Value::Float(ctx.dt)";
            case NodeType::EventUpdate:  return "Value::Float(ctx.dt)"; // Delta output
            case NodeType::KeyDown:      return "Value::Bool(ctx.input.IsKeyDown(KeyFromName((" + IN(0) + ").s)))";
            case NodeType::GetVar:       return "([&]() -> Value { auto _it = ctx.inst.vars.find((" + IN(0) +
                                                ").s); return _it != ctx.inst.vars.end() ? _it->second : Value::Float(0.0f); }())";
            case NodeType::GetPosition:  return "([&]() -> Value { entt::entity _e = BakedEnt(ctx.scene.Registry(), " + IN(0) +
                                                ", ctx.self); const Transform* _t = BakedGet<Transform>(ctx.scene.Registry(), _e); "
                                                "return Value::Vec3(_t ? _t->position : glm::vec3(0.0f)); }())";
            case NodeType::EventUIClicked: // Action payload (valid while the event fires)
                return "(ctx.eventAction ? Value::Str(*ctx.eventAction) : Value::Str(\"\"))";
            case NodeType::EventUIChanged:
                return outPin == 1   ? "Value::Float(ctx.eventValue)"
                       : outPin == 2 ? "Value::Bool(ctx.eventToggled)"
                                     : "Value::Float(ctx.eventSelected)";
            case NodeType::GetUIValue: {
                const char* expr = outPin == 0   ? "Value::Float(_el.value)"
                                   : outPin == 1 ? "Value::Bool(_el.toggled)"
                                                 : "Value::Float(static_cast<f32>(_el.selected))";
                return "([&]() -> Value { Value _o = Value::Float(0.0f); bool _f = false; "
                       "const std::string _n = (" + IN(0) + ").s; if (!_n.empty()) "
                       "ctx.scene.Registry().view<UIElement>().each([&](const UIElement& _el) { "
                       "if (_f || _el.action != _n) return; _f = true; _o = " + std::string(expr) +
                       "; }); return _o; }())";
            }
            case NodeType::IsAlive:
                return "Value::Bool(combat::IsAlive(ctx.scene, BakedEnt(ctx.scene.Registry(), " + IN(0) + ", ctx.self)))";
            case NodeType::GetHealth:
                return "([&]() -> Value { const Health* _h = BakedGet<Health>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " +
                       IN(0) + ", ctx.self)); return Value::Float(_h ? (" +
                       (outPin == 1 ? std::string("_h->max") : std::string("_h->current")) +
                       ") : 0.0f); }())";
            case NodeType::OnDeath:
                return outPin == 2
                           ? "(ctx.eventInstigator != entt::null ? "
                             "Value::Ent(static_cast<u32>(ctx.eventInstigator)) : Value::Ent(0xFFFFFFFFu))"
                           : "(ctx.eventDeathTag ? Value::Str(*ctx.eventDeathTag) : Value::Str(\"\"))";
            case NodeType::IsPlayerVisible:
                return "([&]() -> Value { const AIPerception* _p = "
                       "BakedGet<AIPerception>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " + IN(0) +
                       ", ctx.self)); return Value::Bool(_p && _p->canSeeTarget); }())";
            case NodeType::GetAwareness:
                return "([&]() -> Value { const AIPerception* _p = "
                       "BakedGet<AIPerception>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " + IN(0) +
                       ", ctx.self)); return Value::Float(_p ? _p->awareness : 0.0f); }())";
            case NodeType::OnSpotPlayer:
                return outPin == 2
                           ? "(ctx.eventSpotTarget != entt::null ? "
                             "Value::Ent(static_cast<u32>(ctx.eventSpotTarget)) : Value::Ent(0xFFFFFFFFu))"
                           : "(ctx.eventSpotter != entt::null ? "
                             "Value::Ent(static_cast<u32>(ctx.eventSpotter)) : Value::Ent(0xFFFFFFFFu))";
            case NodeType::AliveCount:
                return "([&]() -> Value { f32 _c = 0.0f; const std::string _id = (" + IN(0) +
                       ").s; for (auto _e : ctx.scene.Registry().view<Encounter>()) "
                       "if (ctx.scene.Registry().get<Encounter>(_e).id == _id) { "
                       "_c = static_cast<f32>(ctx.scene.Registry().get<Encounter>(_e).aliveCount); break; } "
                       "return Value::Float(_c); }())";
            case NodeType::HasItem:
                return "Value::Bool(game::HasItem((" + IN(0) +
                       ").s, static_cast<u32>(glm::max(0.0f, BakedF(" + IN(1) + ")))))";
            case NodeType::ItemCount:
                return "Value::Float(static_cast<f32>(game::ItemCount((" + IN(0) + ").s)))";
            case NodeType::AreaVisitCount:
                // Pin 1 = "First Visit" (visits <= 1), pin 0 = the raw count.
                return outPin == 1
                           ? "Value::Bool(world::Get().VisitCount(world::ResolveArea((" + IN(0) +
                                 ").s)) <= 1u)"
                           : "Value::Float(static_cast<f32>(world::Get().VisitCount("
                             "world::ResolveArea((" + IN(0) + ").s))))";
            case NodeType::GetAreaVar:
                return "Value::Float(world::Get().GetVar(world::ResolveArea((" + IN(0) + ").s), (" +
                       IN(1) + ").s))";
            default: return "Value{}";
        }
    }

    // Follows the exec wire out of (node,outPin) and emits the target node.
    void EmitExec(std::string& out, u32 node, u32 outPin, std::vector<u32>& stack, int depth) const {
        if (depth > 1024) return;
        for (const Link& l : g.links)
            if (l.fromNode == node && l.fromPin == outPin) {
                EmitExecNode(out, l.toNode, stack, depth + 1);
                return;
            }
    }

    // Emits one exec node's side effect, then follows its exec output(s).
    void EmitExecNode(std::string& out, u32 node, std::vector<u32>& stack, int depth) const {
        if (depth > 1024) return;
        const Node* np = g.Find(node);
        if (!np) return;
        if (std::find(stack.begin(), stack.end(), node) != stack.end()) return; // break exec cycle
        stack.push_back(node);
        auto IN = [&](u32 p) { return EmitInput(node, p, 0); };
        switch (np->type) {
            case NodeType::Print:
                out += "        HBE_INFO(\"[Schematic] {}\", (" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::Branch:
                out += "        if (BakedB(" + IN(1) + ")) {\n";
                EmitExec(out, node, 0, stack, depth);
                out += "        } else {\n";
                EmitExec(out, node, 1, stack, depth);
                out += "        }\n";
                break;
            case NodeType::Sequence:
                EmitExec(out, node, 0, stack, depth);
                EmitExec(out, node, 1, stack, depth);
                break;
            case NodeType::SetVar:
                out += "        ctx.inst.vars[(" + IN(1) + ").s] = (" + IN(2) + ");\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetPosition:
                out += "        { entt::entity _e = BakedEnt(ctx.scene.Registry(), " + IN(1) +
                       ", ctx.self); if (Transform* _t = BakedGet<Transform>(ctx.scene.Registry(), _e)) "
                       "_t->position = (" + IN(2) + ").v3; }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::Translate:
                out += "        { entt::entity _e = BakedEnt(ctx.scene.Registry(), " + IN(1) +
                       ", ctx.self); if (Transform* _t = BakedGet<Transform>(ctx.scene.Registry(), _e)) "
                       "_t->position += (" + IN(2) + ").v3; }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::Delay:
                out += "        { f32& _t = ctx.inst.timers[" + std::to_string(node) +
                       "u]; if (_t <= 0.0f) { _t = glm::max(0.01f, (" + IN(1) + ").f);\n";
                EmitExec(out, node, 0, stack, depth);
                out += "        } }\n";
                break;
            case NodeType::ReachCheckpoint:
                out += "        game::ReachCheckpoint((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetObjective:
                out += "        game::SetObjective((" + IN(1) + ").s, (" + IN(2) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::CompleteObjective:
                out += "        game::CompleteObjective((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::ApplyDamage:
                out += "        { combat::DamageEvent _ev; _ev.target = BakedEnt(ctx.scene.Registry(), " + IN(1) +
                       ", ctx.self); _ev.instigator = ctx.self; _ev.amount = BakedF(" + IN(2) +
                       "); combat::ApplyDamage(ctx.scene, _ev, nullptr); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::Kill:
                out += "        combat::Kill(ctx.scene, BakedEnt(ctx.scene.Registry(), " + IN(1) + ", ctx.self));\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::Heal:
                out += "        combat::Heal(ctx.scene, BakedEnt(ctx.scene.Registry(), " + IN(1) + ", ctx.self), BakedF(" +
                       IN(2) + "));\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetHealth:
                out += "        { if (Health* _h = BakedGet<Health>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " +
                       IN(1) + ", ctx.self))) { _h->current = glm::clamp(BakedF(" + IN(2) +
                       "), 0.0f, _h->max); if (_h->current <= 0.0f) _h->alive = false; "
                       "else if (!_h->alive) { _h->alive = true; _h->deathDispatched = false; } } }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetInvulnerable:
                out += "        { if (Health* _h = BakedGet<Health>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " +
                       IN(1) + ", ctx.self))) _h->invincible = BakedB(" + IN(2) + "); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetAIState:
                out += "        { if (AIBehavior* _b = BakedGet<AIBehavior>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " +
                       IN(1) + ", ctx.self))) _b->state = AIStateFromName((" + IN(2) + ").s); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetAlert:
                out += "        { if (AIPerception* _p = "
                       "BakedGet<AIPerception>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " + IN(1) +
                       ", ctx.self))) _p->awareness = glm::clamp(BakedF(" + IN(2) + "), 0.0f, 1.0f); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SpawnGroup:
                out += "        { const std::string _id = (" + IN(1) +
                       ").s; for (auto _e : ctx.scene.Registry().view<Spawner>()) "
                       "if (ctx.scene.Registry().get<Spawner>(_e).spawnerId == _id) "
                       "ctx.scene.Registry().get<Spawner>(_e).spawnRequested = true; }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::DespawnAll:
                out += "        { const std::string _id = (" + IN(1) +
                       ").s; for (auto _e : ctx.scene.Registry().view<Spawner>()) "
                       "if (ctx.scene.Registry().get<Spawner>(_e).encounterId == _id) "
                       "ctx.scene.Registry().get<Spawner>(_e).despawnRequested = true; }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::GrantItem:
                out += "        game::AddItem((" + IN(1) +
                       ").s, static_cast<u32>(glm::max(0.0f, BakedF(" + IN(2) + "))));\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::RemoveItem:
                out += "        game::RemoveItem((" + IN(1) +
                       ").s, static_cast<u32>(glm::max(0.0f, BakedF(" + IN(2) + "))));\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::EquipWeapon:
                out += "        game::EquipWeapon((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetMorphWeight:
                out += "        { if (MorphState* _m = facial::ResolveMorphTarget(ctx.scene, BakedEnt(ctx.scene.Registry(), " +
                       IN(1) + ", ctx.self))) { const std::string _n = (" + IN(2) +
                       ").s; if (!_n.empty()) _m->weights[_n] = glm::clamp(BakedF(" + IN(3) +
                       "), 0.0f, 1.0f); } }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlayFacialExpression:
                out += "        { if (FacialAnimator* _f = "
                       "BakedGet<FacialAnimator>(ctx.scene.Registry(), BakedEnt(ctx.scene.Registry(), " + IN(1) +
                       ", ctx.self))) { _f->expression = (" + IN(2) +
                       ").s; _f->expressionWeight = glm::clamp(BakedF(" + IN(3) + "), 0.0f, 1.0f); } }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetAreaVar:
                out += "        world::Get().SetVar(world::ResolveArea((" + IN(1) + ").s), (" +
                       IN(2) + ").s, BakedF(" + IN(3) + "));\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetMusicState:
                out += "        game::SetMusicState((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SetMusicParam:
                out += "        game::SetMusicParameter((" + IN(1) + ").s, (" + IN(2) + ").f);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlayStinger:
                out += "        game::PlayStinger((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlayVoiceline:
                out += "        game::PlayVoiceline((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlayDialogue:
                out += "        game::PlayDialogue((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlayCutscene:
                out += "        game::PlayCutscene((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::PlaySequence:
                out += "        game::PlaySequence((" + IN(1) + ").s);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::SpawnEffect:
                out += "        game::SpawnEffect((" + IN(1) + ").s, (" + IN(2) + ").v3);\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UIShowPanel:
                out += "        game::QueueUICommand({game::UICommand::Op::Show, (" + IN(1) +
                       ").s});\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UIPushPanel:
                out += "        game::QueueUICommand({game::UICommand::Op::Push, (" + IN(1) +
                       ").s});\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UIPopPanel:
                out += "        game::QueueUICommand({game::UICommand::Op::Pop, std::string()});\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UISetText:
                out += "        { const std::string _n = (" + IN(1) + ").s; const std::string _tx = (" +
                       IN(2) + ").s; if (!_n.empty()) ctx.scene.Registry().view<UIElement>().each("
                       "[&](UIElement& _el) { if (_el.action == _n) _el.text = _tx; }); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UISetVisible:
                out += "        { const std::string _n = (" + IN(1) + ").s; const bool _v = BakedB(" +
                       IN(2) + "); if (!_n.empty()) ctx.scene.Registry().view<UIElement>().each("
                       "[&](UIElement& _el) { if (_el.action == _n) _el.visible = _v; }); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UISetValue:
                out += "        { const std::string _n = (" + IN(1) + ").s; const f32 _v = BakedF(" +
                       IN(2) + "); if (!_n.empty()) ctx.scene.Registry().view<UIElement>().each("
                       "[&](UIElement& _el) { if (_el.action != _n) return; "
                       "_el.value = glm::clamp(_v, 0.0f, 1.0f); _el.toggled = _v > 0.5f; "
                       "if (!_el.options.empty()) _el.selected = glm::clamp(static_cast<int>(_v), 0, "
                       "static_cast<int>(_el.options.size()) - 1); }); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::UIPlayAnim:
                out += "        { const std::string _n = (" + IN(1) + ").s; if (!_n.empty()) "
                       "ctx.scene.Registry().view<UIElement, UIAnimator>().each("
                       "[&](UIElement& _el, UIAnimator& _an) { "
                       "if (_el.action != _n || _an.trigger != UIAnimator::Trigger::Manual) return; "
                       "_an.time = 0.0f; _an.playing = true; _an.captured = false; }); }\n";
                EmitExec(out, node, 0, stack, depth);
                break;
            case NodeType::EventStart:
            case NodeType::EventUpdate:
            case NodeType::EventUIClicked:
            case NodeType::EventUIChanged:
            default:
                EmitExec(out, node, 0, stack, depth);
                break;
        }
        stack.pop_back();
    }
};

} // namespace

std::string TranspileGraph(const Graph& g, const std::string& fnName) {
    const Gen gen{g};
    std::string startBody, updateBody;
    std::vector<u32> stack;
    for (const Node& n : g.nodes)
        if (n.type == NodeType::EventStart) { gen.EmitExec(startBody, n.id, 0, stack, 0); break; }
    stack.clear();
    for (const Node& n : g.nodes)
        if (n.type == NodeType::EventUpdate) { gen.EmitExec(updateBody, n.id, 0, stack, 0); break; }

    // UI events: EVERY matching event node fires (a graph can listen to many
    // widgets), each gated on its Action filter against the event payload.
    const auto uiBody = [&](NodeType evt) {
        std::string body;
        for (const Node& n : g.nodes) {
            if (n.type != evt) continue;
            stack.clear();
            std::string chain;
            gen.EmitExec(chain, n.id, 0, stack, 0);
            const std::string filter = gen.EmitInput(n.id, 0, 0);
            body += "        { const std::string _flt = (" + filter +
                    ").s; if (ctx.eventAction && (_flt.empty() || *ctx.eventAction == _flt)) {\n";
            body += chain;
            body += "        } }\n";
        }
        return body;
    };
    const std::string clickedBody = uiBody(NodeType::EventUIClicked);
    const std::string changedBody = uiBody(NodeType::EventUIChanged);

    // Combat deaths: every OnDeath node fires, gated on its Tag filter vs the payload.
    std::string deathBody;
    for (const Node& n : g.nodes) {
        if (n.type != NodeType::OnDeath) continue;
        stack.clear();
        std::string chain;
        gen.EmitExec(chain, n.id, 0, stack, 0);
        const std::string filter = gen.EmitInput(n.id, 0, 0);
        deathBody += "        { const std::string _flt = (" + filter +
                     ").s; if (ctx.eventDeathTag && (_flt.empty() || *ctx.eventDeathTag == _flt)) {\n";
        deathBody += chain;
        deathBody += "        } }\n";
    }

    // OnSpotPlayer: no filter (the engine only invokes it on the spotter's graph).
    std::string spotBody;
    for (const Node& n : g.nodes) {
        if (n.type != NodeType::OnSpotPlayer) continue;
        stack.clear();
        gen.EmitExec(spotBody, n.id, 0, stack, 0);
    }

    std::string s;
    s += "static void " + fnName + "(CompiledContext& ctx, NodeType evt) {\n";
    s += "    (void)ctx;\n";
    s += "    switch (evt) {\n";
    s += "    case NodeType::EventStart: {\n";
    s += startBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    case NodeType::EventUpdate: {\n";
    s += updateBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    case NodeType::EventUIClicked: {\n";
    s += clickedBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    case NodeType::EventUIChanged: {\n";
    s += changedBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    case NodeType::OnDeath: {\n";
    s += deathBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    case NodeType::OnSpotPlayer: {\n";
    s += spotBody;
    s += "        break;\n";
    s += "    }\n";
    s += "    default: break;\n";
    s += "    }\n";
    s += "}\n";
    return s;
}

std::string TranspileUnit(const std::vector<std::pair<std::string, Graph>>& graphs) {
    std::string s;
    s += "// GENERATED by HeartbreakBaker - DO NOT EDIT.\n";
    s += "// Each project schematic (.hbschem) compiled to native C++; linked into the\n";
    s += "// runtime so graphs run as machine code instead of the interpreter.\n";
    s += "#include \"Schematic/SchematicSystem.h\"\n";
    s += "#include \"Game/GameSystems.h\"\n";
    s += "#include \"Game/CombatSystem.h\"\n";
    s += "#include \"Scene/Components.h\"\n";
    s += "#include \"Scene/FacialSystem.h\"\n";
    s += "#include \"Scene/Scene.h\"\n";
    s += "#include \"Scene/WorldState.h\"\n";
    s += "#include \"Core/Input.h\"\n";
    s += "#include \"Core/Log.h\"\n";
    s += "\n";
    s += "#include <entt/entt.hpp>\n";
    s += "#include <glm/glm.hpp>\n";
    s += "\n";
    s += "namespace hbe::schematic {\n\n";

    std::vector<std::string> fnNames;
    fnNames.reserve(graphs.size());
    for (std::size_t i = 0; i < graphs.size(); ++i) {
        const std::string fn = "Baked_" + std::to_string(i);
        fnNames.push_back(fn);
        s += "// " + graphs[i].first + "\n";
        s += TranspileGraph(graphs[i].second, fn);
        s += "\n";
    }

    s += "void RegisterBakedSchematics() {\n";
    for (std::size_t i = 0; i < graphs.size(); ++i)
        s += "    RegisterCompiled(\"" + Escape(graphs[i].first) + "\", &" + fnNames[i] + ");\n";
    s += "}\n\n";
    s += "} // namespace hbe::schematic\n";
    return s;
}

} // namespace hbe::schematic

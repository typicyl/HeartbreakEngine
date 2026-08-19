// Material/MaterialGraphCompiler.cpp - see MaterialGraphCompiler.h.
#include "Material/MaterialGraphCompiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hbe::mat {

namespace {

// ---- Deterministic value noise / voronoi (hash-based, portable) --------------------------
u32 Hash2(i32 x, i32 y, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= static_cast<u32>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<u32>(y) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}
f32 Hash01(i32 x, i32 y, u32 seed) { return static_cast<f32>(Hash2(x, y, seed) & 0xFFFFFFu) / 16777215.0f; }

f32 ValueNoise(glm::vec2 p, u32 seed) {
    const i32 x0 = static_cast<i32>(std::floor(p.x));
    const i32 y0 = static_cast<i32>(std::floor(p.y));
    const f32 fx = p.x - static_cast<f32>(x0);
    const f32 fy = p.y - static_cast<f32>(y0);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx);
    const f32 uy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = Hash01(x0, y0, seed), b = Hash01(x0 + 1, y0, seed);
    const f32 c = Hash01(x0, y0 + 1, seed), d = Hash01(x0 + 1, y0 + 1, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

f32 VoronoiF1(glm::vec2 p, u32 seed) {
    const i32 cx = static_cast<i32>(std::floor(p.x));
    const i32 cy = static_cast<i32>(std::floor(p.y));
    f32 best = 8.0f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 gx = cx + dx, gy = cy + dy;
            const glm::vec2 feat(static_cast<f32>(gx) + Hash01(gx, gy, seed),
                                 static_cast<f32>(gy) + Hash01(gx, gy, seed ^ 0x1234u));
            const glm::vec2 d = feat - p;
            best = std::min(best, glm::dot(d, d));
        }
    return std::clamp(std::sqrt(best), 0.0f, 1.0f);
}

glm::vec3 SpacePos(const SampleContext& ctx, Space s) {
    switch (s) {
        case Space::UV0: return {ctx.uv0.x, ctx.uv0.y, 0.0f};
        case Space::UV1: return {ctx.uv1.x, ctx.uv1.y, 0.0f};
        case Space::Object: return ctx.objectPos;
        case Space::World: return ctx.worldPos;
        case Space::Triplanar: return ctx.worldPos;
        default: return {ctx.uv0.x, ctx.uv0.y, 0.0f};
    }
}
glm::vec2 SpaceCoord2(const SampleContext& ctx, Space s) {
    const glm::vec3 p = SpacePos(ctx, s);
    if (s == Space::World || s == Space::Object || s == Space::Triplanar) return {p.x, p.z};
    return {p.x, p.y};
}

f32 SampleRamp(const std::vector<RampStop>& ramp, f32 t) {
    (void)t;
    return 0.0f; // unused (ramp returns color) - see EvalRamp
}
glm::vec4 EvalRamp(const std::vector<RampStop>& ramp, f32 t) {
    if (ramp.empty()) return glm::vec4(t, t, t, 1.0f);
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= ramp.front().pos) return ramp.front().color;
    if (t >= ramp.back().pos) return ramp.back().color;
    for (usize i = 1; i < ramp.size(); ++i) {
        if (t <= ramp[i].pos) {
            const f32 span = ramp[i].pos - ramp[i - 1].pos;
            const f32 f = span > 1e-6f ? (t - ramp[i - 1].pos) / span : 0.0f;
            return glm::mix(ramp[i - 1].color, ramp[i].color, f);
        }
    }
    return ramp.back().color;
}

// Evaluate one op given its resolved input values. `has[k]` = input pin k is connected.
glm::vec4 EvalOp(const Op& op, const glm::vec4 in[3], const bool has[3], const SampleContext& ctx,
                 const TextureProvider& tex) {
    switch (op.type) {
        case NodeType::Constant:
        case NodeType::Color:
        case NodeType::Float:
        case NodeType::Vector:
            return op.constant;

        case NodeType::UV: return {ctx.uv0.x, ctx.uv0.y, 0.0f, 0.0f};
        case NodeType::WorldPosition: return {ctx.worldPos, 1.0f};
        case NodeType::ObjectPosition: return {ctx.objectPos, 1.0f};
        case NodeType::Normal: return {ctx.normal, 0.0f};
        case NodeType::VertexColor: return ctx.vertexColor;

        case NodeType::Texture: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            if (tex) return tex(op.texture, uv, NodeType::Texture);
            return op.constant.x != 0.0f || op.constant.y != 0.0f ? op.constant : glm::vec4(1.0f);
        }
        case NodeType::NormalMap: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const glm::vec4 e = tex ? tex(op.texture, uv, NodeType::NormalMap) : glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
            return {e.x * 2.0f - 1.0f, e.y * 2.0f - 1.0f, e.z * 2.0f - 1.0f, 1.0f};
        }
        case NodeType::Height: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const f32 h = tex ? tex(op.texture, uv, NodeType::Height).x : op.constant.x;
            return glm::vec4(h);
        }
        case NodeType::Mask: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const f32 m = tex ? tex(op.texture, uv, NodeType::Mask).x : op.constant.x;
            return glm::vec4(m);
        }

        case NodeType::Multiply: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(1.0f);
            const glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            return a * b;
        }
        case NodeType::Add: return (has[0] ? in[0] : glm::vec4(0.0f)) + (has[1] ? in[1] : glm::vec4(0.0f));
        case NodeType::Subtract: return (has[0] ? in[0] : glm::vec4(0.0f)) - (has[1] ? in[1] : glm::vec4(0.0f));
        case NodeType::Divide: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(0.0f);
            glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            for (int i = 0; i < 4; ++i) b[i] = std::abs(b[i]) < 1e-8f ? 1.0f : b[i];
            return a / b;
        }
        case NodeType::Lerp: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(0.0f);
            const glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            const f32 t = has[2] ? in[2].x : op.constant.x;
            return glm::mix(a, b, std::clamp(t, 0.0f, 1.0f));
        }
        case NodeType::Clamp: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            return glm::clamp(x, glm::vec4(op.constant.x), glm::vec4(op.constant.y));
        }
        case NodeType::Remap: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 inSpan = op.constant.y - op.constant.x;
            const f32 outSpan = op.constant.w - op.constant.z;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) {
                const f32 t = std::abs(inSpan) < 1e-8f ? 0.0f : (x[i] - op.constant.x) / inSpan;
                r[i] = op.constant.z + t * outSpan;
            }
            return r;
        }
        case NodeType::Power: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 e = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) r[i] = std::pow(std::max(x[i], 0.0f), e);
            return r;
        }
        case NodeType::Smoothstep: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 e0 = op.constant.x, e1 = op.constant.y;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) {
                const f32 span = e1 - e0;
                const f32 t = std::abs(span) < 1e-8f ? (x[i] >= e1 ? 1.0f : 0.0f)
                                                     : std::clamp((x[i] - e0) / span, 0.0f, 1.0f);
                r[i] = t * t * (3.0f - 2.0f * t);
            }
            return r;
        }
        case NodeType::OneMinus: return glm::vec4(1.0f) - (has[0] ? in[0] : glm::vec4(0.0f));

        case NodeType::Noise: {
            const glm::vec2 c = has[0] ? glm::vec2(in[0]) : SpaceCoord2(ctx, op.space);
            const f32 scale = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const u32 seed = static_cast<u32>(op.constant.y) + 1u;
            return glm::vec4(ValueNoise(c * scale, seed));
        }
        case NodeType::Voronoi: {
            const glm::vec2 c = has[0] ? glm::vec2(in[0]) : SpaceCoord2(ctx, op.space);
            const f32 scale = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const u32 seed = static_cast<u32>(op.constant.y) + 1u;
            return glm::vec4(VoronoiF1(c * scale, seed));
        }
        case NodeType::Gradient: {
            const f32 t = has[0] ? in[0].x : SpaceCoord2(ctx, op.space).x;
            const f32 tc = std::clamp(t, 0.0f, 1.0f);
            return {tc, tc, tc, 1.0f};
        }
        case NodeType::ColorRamp: {
            const f32 t = has[0] ? in[0].x : 0.0f;
            return EvalRamp(op.ramp, t);
        }

        case NodeType::MaterialLayer: return op.constant; // external ref; neutral in value eval
        case NodeType::Output: return glm::vec4(0.0f);     // not a value node
        default: return glm::vec4(0.0f);
    }
    (void)SampleRamp;
}

} // namespace

// ---- Compile ----------------------------------------------------------------------------
CompiledGraph Compile(const Graph& gIn, const std::vector<ParamOverride>& overrides) {
    CompiledGraph out;
    // Apply instance overrides to a copy of the params (never mutate the source graph).
    out.resolvedParams = gIn.params;
    ApplyOverrides(out.resolvedParams, overrides);

    const Node* outputNode = gIn.OutputNode();
    if (!outputNode) {
        out.error = "graph has no (single) Output node";
        return out;
    }

    // Index nodes by id.
    std::unordered_map<u32, const Node*> byId;
    byId.reserve(gIn.nodes.size() * 2);
    for (const auto& n : gIn.nodes) byId[n.id] = &n;

    // Reachability: BFS backward from the Output node's connected inputs (drop dead nodes).
    std::unordered_map<u32, bool> reachable;
    std::vector<u32> stack;
    auto pushInputs = [&](const Node& n) {
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount; ++pin)
            if (const Link* l = gIn.LinkInto(n.id, pin))
                if (byId.count(l->fromNode) && !reachable.count(l->fromNode)) {
                    reachable[l->fromNode] = true;
                    stack.push_back(l->fromNode);
                }
    };
    pushInputs(*outputNode);
    while (!stack.empty()) {
        const u32 id = stack.back();
        stack.pop_back();
        pushInputs(*byId[id]);
    }

    // Stable topological order (Kahn) over the reachable set, iterating nodes in vector order so
    // the result is deterministic regardless of link insertion order.
    std::unordered_map<u32, int> indeg;
    for (const auto& n : gIn.nodes) {
        if (!reachable.count(n.id)) continue;
        int d = 0;
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount; ++pin)
            if (const Link* l = gIn.LinkInto(n.id, pin))
                if (reachable.count(l->fromNode)) ++d;
        indeg[n.id] = d;
    }
    std::vector<u32> order;
    order.reserve(reachable.size());
    bool progress = true;
    std::unordered_map<u32, bool> emitted;
    while (progress) {
        progress = false;
        for (const auto& n : gIn.nodes) { // vector order == deterministic tie-break
            if (!reachable.count(n.id) || emitted.count(n.id)) continue;
            if (indeg[n.id] != 0) continue;
            order.push_back(n.id);
            emitted[n.id] = true;
            progress = true;
            // Decrement consumers.
            for (const auto& m : gIn.nodes) {
                if (!reachable.count(m.id) || emitted.count(m.id)) continue;
                const u8 inCount = NodeInfoOf(m.type).inputCount;
                for (u8 pin = 0; pin < inCount; ++pin)
                    if (const Link* l = gIn.LinkInto(m.id, pin))
                        if (l->fromNode == n.id) indeg[m.id]--;
            }
        }
    }
    if (order.size() != reachable.size()) {
        out.error = "graph contains a cycle";
        return out;
    }

    // Emit ops in topo order; map nodeId -> register index.
    std::unordered_map<u32, u16> reg;
    reg.reserve(order.size() * 2);
    out.ops.reserve(order.size());
    for (const u32 id : order) {
        const Node& n = *byId[id];
        Op op;
        op.type = n.type;
        op.space = n.space;
        op.constant = n.constant;
        op.texture = n.texture;
        op.ramp = n.ramp;
        std::sort(op.ramp.begin(), op.ramp.end(),
                  [](const RampStop& a, const RampStop& b) { return a.pos < b.pos; });
        // If a Constant-family node is bound to an exposed param, bake the resolved value in.
        if ((n.type == NodeType::Constant || n.type == NodeType::Color || n.type == NodeType::Float ||
             n.type == NodeType::Vector) &&
            !n.paramName.empty()) {
            if (const Param* p = out.resolvedParams.Find(n.paramName)) op.constant = p->value;
        }
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount && pin < 3; ++pin) {
            if (const Link* l = gIn.LinkInto(id, pin)) {
                auto it = reg.find(l->fromNode);
                if (it != reg.end()) op.inputReg[pin] = it->second;
            }
        }
        reg[id] = static_cast<u16>(out.ops.size());
        out.ops.push_back(std::move(op));
    }

    // Constant-fold forward pass. An op folds when its node type is not context/texture dependent
    // and every connected input is itself folded. Unconnected inputs use neutral defaults (const).
    for (usize i = 0; i < out.ops.size(); ++i) {
        Op& op = out.ops[i];
        const bool ctxDep = NodeInfoOf(op.type).contextDependent;
        bool inputsConst = true;
        glm::vec4 in[3]{};
        bool has[3] = {false, false, false};
        for (int k = 0; k < 3; ++k) {
            if (op.inputReg[k] == Op::kNoReg) continue;
            has[k] = true;
            const Op& src = out.ops[op.inputReg[k]];
            if (!src.folded) {
                inputsConst = false;
            } else {
                in[k] = src.foldedValue;
            }
        }
        if (!ctxDep && inputsConst) {
            SampleContext neutral; // context-independent by construction here
            op.foldedValue = EvalOp(op, in, has, neutral, {});
            op.folded = true;
        }
    }

    // Bind Output channels to registers.
    bool allBoundConst = true;
    for (u8 c = 0; c < kChannelCount; ++c) {
        if (const Link* l = gIn.LinkInto(outputNode->id, c)) {
            auto it = reg.find(l->fromNode);
            if (it != reg.end()) {
                out.channelReg[c] = static_cast<int>(it->second);
                if (!out.ops[it->second].folded) allBoundConst = false;
            }
        }
    }
    out.fullyConstant = allBoundConst;
    out.ok = true;
    return out;
}

// ---- Eval -------------------------------------------------------------------------------
SurfaceSample CompiledGraph::Eval(const SampleContext& ctx, const TextureProvider& tex) const {
    SurfaceSample s;
    if (!ok) return s;
    std::vector<glm::vec4> regv(ops.size(), glm::vec4(0.0f));
    for (usize i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (op.folded) {
            regv[i] = op.foldedValue;
            continue;
        }
        glm::vec4 in[3]{};
        bool has[3] = {false, false, false};
        for (int k = 0; k < 3; ++k) {
            if (op.inputReg[k] == Op::kNoReg) continue;
            has[k] = true;
            in[k] = regv[op.inputReg[k]];
        }
        regv[i] = EvalOp(op, in, has, ctx, tex);
    }
    auto ch = [&](Channel c) -> glm::vec4 {
        const int r = channelReg[static_cast<u32>(c)];
        return r >= 0 ? regv[static_cast<usize>(r)] : glm::vec4(0.0f);
    };
    if (channelReg[static_cast<u32>(Channel::BaseColor)] >= 0) s.baseColor = glm::vec3(ch(Channel::BaseColor));
    if (channelReg[static_cast<u32>(Channel::Roughness)] >= 0) s.roughness = ch(Channel::Roughness).x;
    if (channelReg[static_cast<u32>(Channel::Metallic)] >= 0) s.metallic = ch(Channel::Metallic).x;
    if (channelReg[static_cast<u32>(Channel::Normal)] >= 0) {
        const glm::vec3 n = glm::vec3(ch(Channel::Normal));
        s.normalTS = glm::length(n) > 1e-5f ? glm::normalize(n) : glm::vec3(0, 0, 1);
    }
    if (channelReg[static_cast<u32>(Channel::Height)] >= 0) s.height = ch(Channel::Height).x;
    if (channelReg[static_cast<u32>(Channel::AO)] >= 0) s.ao = ch(Channel::AO).x;
    if (channelReg[static_cast<u32>(Channel::Emissive)] >= 0) s.emissive = glm::vec3(ch(Channel::Emissive));
    if (channelReg[static_cast<u32>(Channel::Opacity)] >= 0) s.opacity = ch(Channel::Opacity).x;
    return s;
}

SurfaceParams CompiledGraph::ToSurfaceParams() const {
    SurfaceParams p; // OpenPBR spec defaults
    const SurfaceSample s = Eval(SampleContext{}, {});
    p.base_color = glm::vec4(s.baseColor, s.opacity);
    p.specular_roughness = std::clamp(s.roughness, 0.0f, 1.0f);
    p.base_metalness = std::clamp(s.metallic, 0.0f, 1.0f);
    p.emission_color = s.emissive;
    return p;
}

u64 CompiledGraph::Hash() const {
    u64 h = 1469598103934665603ull;
    h = HashBytes(&fullyConstant, sizeof(fullyConstant), h);
    for (const Op& op : ops) {
        const u8 t = static_cast<u8>(op.type);
        const u8 sp = static_cast<u8>(op.space);
        h = HashBytes(&t, 1, h);
        h = HashBytes(&sp, 1, h);
        for (int k = 0; k < 4; ++k) h = HashF32(op.constant[k], h);
        h = HashBytes(op.inputReg, sizeof(op.inputReg), h);
        h = HashBytes(&op.folded, 1, h);
        for (int k = 0; k < 4; ++k) h = HashF32(op.foldedValue[k], h);
        h = HashStr(op.texture, h);
        for (const RampStop& r : op.ramp) {
            h = HashF32(r.pos, h);
            for (int k = 0; k < 4; ++k) h = HashF32(r.color[k], h);
        }
    }
    for (u32 c = 0; c < kChannelCount; ++c) h = HashBytes(&channelReg[c], sizeof(int), h);
    return h;
}

} // namespace hbe::mat

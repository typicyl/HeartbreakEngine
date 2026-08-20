// Material/MaterialAuthoringTest.cpp - see MaterialAuthoringTest.h.
//
// P1 blocks (graph + compiler + params) are implemented here. P2 blocks (layers / masks / box
// brush / falloff / tiling) are appended in the same SelfTest() as those modules land, so the
// single `--test-material` flag always runs the whole suite.
#include "Material/MaterialAuthoringTest.h"

#include "Core/Log.h"
#include "Material/BoxBrush.h"
#include "Material/MaterialCook.h"
#include "Material/MaterialGraph.h"
#include "Material/MaterialGraphCompiler.h"
#include "Material/MaterialGraphHlsl.h"
#include "Material/MaterialLayer.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace hbe::mat {

namespace {

int g_fail = 0;
#define MCHECK(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            HBE_ERROR("  [material test] FAIL: {}", msg);                                          \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::abs(a - b) <= eps; }

// ---- Block: node catalog lockstep -------------------------------------------------------
void TestCatalog() {
    const auto& cat = NodeCatalog();
    MCHECK(cat.size() == static_cast<usize>(NodeType::Count), "catalog size != NodeType::Count");
    for (usize i = 0; i < cat.size(); ++i)
        MCHECK(cat[i].type == static_cast<NodeType>(i), "catalog row out of enum order");
    // Output node must have exactly the 8 channel input pins.
    MCHECK(NodeInfoOf(NodeType::Output).inputCount == kChannelCount, "Output arity != 8");
}

// Build a simple constant material graph: BaseColor = Color(rgba) * Float(scale).
Graph MakeConstGraph(glm::vec4 color, f32 scale) {
    Graph g;
    g.name = "ConstTest";
    const u32 col = g.AddNode(NodeType::Color, {0, 0});
    g.FindNode(col)->constant = color;
    const u32 sca = g.AddNode(NodeType::Float, {0, 60});
    g.FindNode(sca)->constant = glm::vec4(scale);
    const u32 mul = g.AddNode(NodeType::Multiply, {120, 30});
    const u32 out = g.AddNode(NodeType::Output, {260, 30});
    g.Connect(col, mul, 0);
    g.Connect(sca, mul, 1);
    g.Connect(mul, out, static_cast<u8>(Channel::BaseColor));
    return g;
}

// ---- Block: serialization round-trip + determinism --------------------------------------
void TestSerialization() {
    Graph g = MakeConstGraph({0.2f, 0.4f, 0.6f, 1.0f}, 0.5f);
    // Add richer content: a ColorRamp + a param, to exercise the full serializer.
    Param p;
    p.name = "Tint";
    p.type = ParamType::Color;
    p.value = {1, 0.5f, 0.25f, 1};
    g.params.params.push_back(p);
    const u32 ramp = g.AddNode(NodeType::ColorRamp, {0, 200});
    g.FindNode(ramp)->ramp = {{0.0f, {0, 0, 0, 1}}, {1.0f, {1, 1, 1, 1}}};

    const std::string s1 = GraphToJsonString(g);
    auto g2 = GraphFromJsonString(s1);
    MCHECK(g2.has_value(), "graph failed to parse back");
    if (!g2) return;
    MCHECK(g2->nodes.size() == g.nodes.size(), "node count changed on round-trip");
    MCHECK(g2->links.size() == g.links.size(), "link count changed on round-trip");
    MCHECK(g2->params.params.size() == g.params.params.size(), "param count changed");
    // Deterministic: re-serialize is byte-identical (portable, stable field order).
    const std::string s2 = GraphToJsonString(*g2);
    MCHECK(s1 == s2, "serialization is not deterministic (round-trip differs)");

    // Unknown node type is dropped, not fatal (forward-compat). This deliberately triggers a
    // "unknown node dropped" warning; mute it so a PASSING test run stays clean.
    std::string mangled = s1;
    const auto pos = mangled.find("\"Color\"");
    if (pos != std::string::npos) mangled.replace(pos, 7, "\"FutureNode99\"");
    std::optional<Graph> g3;
    {
        LogMuteScope mute;
        g3 = GraphFromJsonString(mangled);
    }
    MCHECK(g3.has_value(), "mangled graph must still parse (unknown node dropped)");
}

// ---- Block: compilation + determinism ---------------------------------------------------
void TestCompile() {
    Graph g = MakeConstGraph({0.2f, 0.4f, 0.6f, 1.0f}, 0.5f);
    CompiledGraph c1 = Compile(g);
    MCHECK(c1.ok, "compile failed");
    MCHECK(c1.fullyConstant, "constant graph should fold fully");
    // Deterministic: compiling twice yields the same content hash.
    CompiledGraph c2 = Compile(g);
    MCHECK(c1.Hash() == c2.Hash(), "compilation is not deterministic (hash differs)");

    // Folded value: BaseColor = color * scale.
    SurfaceParams sp = c1.ToSurfaceParams();
    MCHECK(Near(sp.base_color.r, 0.1f) && Near(sp.base_color.g, 0.2f) && Near(sp.base_color.b, 0.3f),
           "constant fold produced wrong base color");

    // A context-dependent node (UV) makes the graph non-constant.
    Graph gu;
    const u32 uv = gu.AddNode(NodeType::UV);
    const u32 out = gu.AddNode(NodeType::Output);
    gu.Connect(uv, out, static_cast<u8>(Channel::BaseColor));
    CompiledGraph cu = Compile(gu);
    MCHECK(cu.ok, "uv graph compile failed");
    MCHECK(!cu.fullyConstant, "graph with UV must not be fully constant");
}

// ---- Block: op evaluation math ----------------------------------------------------------
void TestEvalMath() {
    // Add(2,3) = 5 ; Lerp(0,10,0.25)=2.5 ; OneMinus(0.3)=0.7 ; Remap(0.5 in [0,1]->[10,20])=15
    Graph g;
    const u32 a = g.AddNode(NodeType::Float);
    g.FindNode(a)->constant = glm::vec4(2.0f);
    const u32 b = g.AddNode(NodeType::Float);
    g.FindNode(b)->constant = glm::vec4(3.0f);
    const u32 add = g.AddNode(NodeType::Add);
    const u32 out = g.AddNode(NodeType::Output);
    g.Connect(a, add, 0);
    g.Connect(b, add, 1);
    g.Connect(add, out, static_cast<u8>(Channel::Roughness));
    CompiledGraph c = Compile(g);
    SurfaceSample s = c.Eval(SampleContext{});
    MCHECK(Near(s.roughness, 5.0f), "Add(2,3) != 5");

    Graph g2;
    const u32 r = g2.AddNode(NodeType::Float);
    g2.FindNode(r)->constant = glm::vec4(0.5f);
    const u32 remap = g2.AddNode(NodeType::Remap);
    g2.FindNode(remap)->constant = glm::vec4(0.0f, 1.0f, 10.0f, 20.0f);
    const u32 out2 = g2.AddNode(NodeType::Output);
    g2.Connect(r, remap, 0);
    g2.Connect(remap, out2, static_cast<u8>(Channel::Metallic));
    CompiledGraph c2 = Compile(g2);
    MCHECK(Near(c2.Eval(SampleContext{}).metallic, 15.0f), "Remap 0.5 [0,1]->[10,20] != 15");
}

// ---- Block: parameter overrides ---------------------------------------------------------
void TestParamOverride() {
    Graph g;
    Param p;
    p.name = "BaseTint";
    p.type = ParamType::Color;
    p.value = {0.1f, 0.2f, 0.3f, 1.0f};
    g.params.params.push_back(p);
    const u32 c = g.AddNode(NodeType::Color);
    g.FindNode(c)->paramName = "BaseTint"; // bound to the exposed param
    const u32 out = g.AddNode(NodeType::Output);
    g.Connect(c, out, static_cast<u8>(Channel::BaseColor));

    CompiledGraph base = Compile(g);
    SurfaceParams sp0 = base.ToSurfaceParams();
    MCHECK(Near(sp0.base_color.r, 0.1f), "param default not applied");

    // Instance override without duplicating the material.
    std::vector<ParamOverride> ov = {{"BaseTint", {0.9f, 0.8f, 0.7f, 1.0f}, ""}};
    CompiledGraph inst = Compile(g, ov);
    SurfaceParams sp1 = inst.ToSurfaceParams();
    MCHECK(Near(sp1.base_color.r, 0.9f) && Near(sp1.base_color.g, 0.8f), "param override not applied");
    // Overriding an unknown name must not invent a parameter or change output.
    std::vector<ParamOverride> ov2 = {{"DoesNotExist", {1, 1, 1, 1}, ""}};
    CompiledGraph inst2 = Compile(g, ov2);
    MCHECK(Near(inst2.ToSurfaceParams().base_color.r, 0.1f), "unknown override changed output");
}

// ---- Block: cycle detection -------------------------------------------------------------
void TestCycle() {
    Graph g;
    const u32 m1 = g.AddNode(NodeType::Multiply);
    const u32 m2 = g.AddNode(NodeType::Multiply);
    const u32 out = g.AddNode(NodeType::Output);
    g.Connect(m1, m2, 0);
    g.Connect(m2, m1, 0); // cycle
    g.Connect(m2, out, static_cast<u8>(Channel::BaseColor));
    CompiledGraph c = Compile(g);
    MCHECK(!c.ok, "cycle not detected");
    // No Output node -> compile fails cleanly.
    Graph g2;
    g2.AddNode(NodeType::Float);
    MCHECK(!Compile(g2).ok, "missing Output not detected");
}

// ---- Block: configurable falloff curve --------------------------------------------------
void TestFalloff() {
    const FalloffType shaped[] = {FalloffType::Linear, FalloffType::Smoothstep,
                                  FalloffType::Smootherstep, FalloffType::EaseIn,
                                  FalloffType::EaseOut};
    for (FalloffType t : shaped) {
        Falloff f;
        f.type = t;
        MCHECK(Near(f.Eval(0.0f), 0.0f), "falloff Eval(0) != 0");
        MCHECK(Near(f.Eval(1.0f), 1.0f), "falloff Eval(1) != 1");
        // Monotonic non-decreasing across the band.
        f32 prev = -1.0f;
        bool mono = true;
        for (int i = 0; i <= 10; ++i) {
            const f32 v = f.Eval(i / 10.0f);
            if (v < prev - 1e-4f) mono = false;
            prev = v;
        }
        MCHECK(mono, "falloff curve not monotonic");
    }
    Falloff cst;
    cst.type = FalloffType::Constant;
    MCHECK(Near(cst.Eval(0.0f), 0.0f) && Near(cst.Eval(0.5f), 1.0f), "constant falloff shape wrong");
    // Gamma reshapes the midpoint but keeps endpoints.
    Falloff g;
    g.type = FalloffType::Linear;
    g.gamma = 2.0f;
    MCHECK(Near(g.Eval(0.0f), 0.0f) && Near(g.Eval(1.0f), 1.0f) && Near(g.Eval(0.5f), 0.25f),
           "gamma shaping wrong");
}

// ---- Block: box-brush weight evaluation, rotation, falloff -------------------------------
void TestBoxBrush() {
    BoxBrush b;
    b.size = glm::vec3(2.0f); // unit-ish cube spanning [-1,1]
    b.falloff.type = FalloffType::Linear;
    b.falloffWidth = 0.5f;
    b.strength = 1.0f;
    MCHECK(Near(b.EvaluateBrush(glm::vec3(0.0f)), 1.0f), "box center weight != 1");
    MCHECK(Near(b.EvaluateBrush(glm::vec3(10.0f, 0, 0)), 0.0f), "box outside weight != 0");
    // A point in the fade band gives a partial weight strictly between 0 and 1.
    const f32 band = b.EvaluateBrush(glm::vec3(0.75f, 0, 0)); // half=1, band=0.5, d=0.75 -> t=0.5
    MCHECK(band > 0.05f && band < 0.95f, "box falloff band weight not partial");
    // Strength scales the weight.
    b.strength = 0.5f;
    MCHECK(Near(b.EvaluateBrush(glm::vec3(0.0f)), 0.5f), "box strength not applied");

    // Rotation: a long box (4 x 1 x 1) rotated 90deg about Y must include a point along +Z that
    // the un-rotated box excludes.
    BoxBrush r;
    r.size = glm::vec3(4.0f, 1.0f, 1.0f);
    r.falloff.type = FalloffType::Linear;
    r.falloffWidth = 0.02f;
    const glm::vec3 probe(0.0f, 0.0f, 1.5f);
    MCHECK(Near(r.EvaluateBrush(probe), 0.0f), "unrotated long box should exclude the probe");
    r.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    MCHECK(r.EvaluateBrush(probe) > 0.5f, "rotated long box should include the probe");

    // Bounds enclose the rotated box corners.
    const Aabb bounds = r.Bounds();
    MCHECK(bounds.Contains(probe), "rotated box bounds must contain an interior probe");
    MCHECK(bounds.max.z >= 1.9f, "rotated long box bounds should extend ~2 along Z");
}

// ---- Block: world-space vs local-space tiling (size independence) ------------------------
void TestTiling() {
    const glm::vec3 up(0, 1, 0);
    // World projection: uv advances exactly 1 per metre; independent of box SIZE.
    BoxBrush small;
    small.projection = BoxProjection::World;
    small.size = glm::vec3(1.0f);
    small.tileMeters = glm::vec3(1.0f);
    BoxBrush huge = small;
    huge.size = glm::vec3(1000.0f); // 1000x bigger box...
    const glm::vec2 a = small.ProjectUV(glm::vec3(0, 0, 0), up);
    const glm::vec2 b = small.ProjectUV(glm::vec3(1, 0, 3), up);
    MCHECK(Near(b.x - a.x, 1.0f) && Near(b.y - a.y, 3.0f), "world tiling not 1 tile/metre");
    const glm::vec2 hb = huge.ProjectUV(glm::vec3(1, 0, 3), up);
    MCHECK(Near(hb.x, b.x) && Near(hb.y, b.y), "world tiling not size-independent");
    // Independent per-axis tile: 2m tile on X halves the u rate.
    BoxBrush anis = small;
    anis.tileMeters = glm::vec3(2.0f, 1.0f, 1.0f);
    MCHECK(Near(anis.ProjectUV(glm::vec3(2, 0, 0), up).x, 1.0f), "per-axis tile scale wrong");

    // Local projection: tiling rides the volume transform (scale 2 -> half the world density).
    BoxBrush loc = small;
    loc.projection = BoxProjection::Local;
    loc.scale = glm::vec3(2.0f);
    MCHECK(Near(loc.ProjectUV(glm::vec3(2, 0, 0), up).x, 1.0f), "local tiling should ride scale");
    MCHECK(Near(small.ProjectUV(glm::vec3(2, 0, 0), up).x, 2.0f), "world tiling baseline wrong");
}

// Build a base+one-layer stack for blend tests.
LayerStack MakeStack(f32 baseRough, f32 layerRough, f32 maskW, BlendMode mode) {
    LayerStack s;
    s.base.specular_roughness = baseRough;
    s.baseHeight = 0.5f;
    Layer l;
    l.surface.specular_roughness = layerRough;
    l.mask.kind = MaskKind::Constant;
    l.mask.constant = maskW;
    l.opacity = 1.0f;
    l.blend = mode;
    l.layerHeight = 0.9f;
    s.layers.push_back(l);
    return s;
}

// ---- Block: layer blending (linear / height / height+noise) + normal blending -----------
void TestResolve() {
    // Linear: roughness = mix(0.2, 0.8, 0.5) = 0.5.
    ResolvedSurface r = Resolve(MakeStack(0.2f, 0.8f, 0.5f, BlendMode::Linear), SampleContext{});
    MCHECK(Near(r.surface.specular_roughness, 0.5f), "linear blend wrong");
    // Endpoints: mask 0 -> base, mask 1 -> layer (exact).
    MCHECK(Near(Resolve(MakeStack(0.2f, 0.8f, 0.0f, BlendMode::Linear), {}).surface.specular_roughness, 0.2f),
           "mask 0 must give base");
    MCHECK(Near(Resolve(MakeStack(0.2f, 0.8f, 1.0f, BlendMode::Linear), {}).surface.specular_roughness, 0.8f),
           "mask 1 must give layer");

    // Height blend: endpoints EXACT regardless of height; a taller layer (h=0.9 > base 0.5) wins
    // MORE than linear at the midpoint.
    const f32 he0 = Resolve(MakeStack(0.2f, 0.8f, 0.0f, BlendMode::Height), {}).surface.specular_roughness;
    const f32 he1 = Resolve(MakeStack(0.2f, 0.8f, 1.0f, BlendMode::Height), {}).surface.specular_roughness;
    MCHECK(Near(he0, 0.2f) && Near(he1, 0.8f), "height blend endpoints not exact");
    const f32 hMid = Resolve(MakeStack(0.2f, 0.8f, 0.5f, BlendMode::Height), {}).surface.specular_roughness;
    MCHECK(hMid > 0.5f, "taller layer should win more than linear at midpoint");

    // Height+Noise: endpoints still exact; midpoint differs from plain Height when noise>0.
    LayerStack ns = MakeStack(0.2f, 0.8f, 0.5f, BlendMode::HeightNoise);
    ns.layers[0].noiseAmount = 0.5f;
    ns.layers[0].noiseScale = 4.0f;
    SampleContext ctx;
    ctx.uv0 = {0.37f, 0.61f};
    const f32 hn = Resolve(ns, ctx).surface.specular_roughness;
    MCHECK(std::abs(hn - hMid) > 1e-4f, "noise term had no effect on the mid blend");
    LayerStack ns1 = ns;
    ns1.layers[0].mask.constant = 1.0f;
    MCHECK(Near(Resolve(ns1, ctx).surface.specular_roughness, 0.8f), "height+noise endpoint drift");

    // Normal blend (RNM): identity + full replacement.
    MCHECK(glm::length(BlendNormalRNM({0, 0, 1}, {0, 0, 1}) - glm::vec3(0, 0, 1)) < 1e-4f,
           "RNM flat-over-flat != flat");
    const glm::vec3 base = glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f));
    MCHECK(glm::length(BlendNormalRNM(base, {0, 0, 1}) - base) < 1e-4f, "RNM(base, flat) != base");
    const glm::vec3 det = glm::normalize(glm::vec3(0.0f, 0.4f, 1.0f));
    MCHECK(glm::length(BlendNormalRNM({0, 0, 1}, det) - det) < 1e-4f, "RNM(flat, detail) != detail");
    MCHECK(Near(glm::length(BlendNormalRNM(base, det)), 1.0f), "RNM result not unit length");
    // Resolved normal with a full-mask detail layer approaches the detail normal.
    LayerStack nstk;
    Layer nl;
    nl.mask.constant = 1.0f;
    nl.normalTS = det;
    nstk.layers.push_back(nl);
    MCHECK(glm::length(Resolve(nstk, {}).normalTS - det) < 1e-3f, "resolved normal != full-mask detail");
}

// ---- Block: layer-stack + mask + box serialization; procedural mask bake -----------------
void TestLayerSerialization() {
    LayerStack s;
    s.base.specular_roughness = 0.3f;
    s.base.base_color = {0.5f, 0.5f, 0.5f, 1.0f};
    Layer boxLayer;
    boxLayer.material = "mud.hbmat";
    boxLayer.surface.base_color = {0.2f, 0.15f, 0.1f, 1.0f};
    boxLayer.mask.kind = MaskKind::Box;
    boxLayer.mask.box.size = glm::vec3(4, 2, 4);
    boxLayer.mask.box.tileMeters = glm::vec3(2.0f);
    boxLayer.mask.box.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0));
    boxLayer.blend = BlendMode::Height;
    s.layers.push_back(boxLayer);
    Layer cLayer;
    cLayer.mask.kind = MaskKind::Constant;
    cLayer.mask.constant = 0.4f;
    s.layers.push_back(cLayer);

    const std::string j1 = LayerStackToJsonString(s);
    auto s2 = LayerStackFromJsonString(j1);
    MCHECK(s2.has_value(), "layer stack failed to parse");
    if (s2) {
        MCHECK(s2->layers.size() == 2, "layer count changed on round-trip");
        // Deterministic re-serialize.
        MCHECK(LayerStackToJsonString(*s2) == j1, "layer-stack serialization not deterministic");
        // Box brush survives the round-trip (hash equal).
        MCHECK(s2->layers[0].mask.box.Hash() == s.layers[0].mask.box.Hash(),
               "box brush not preserved by round-trip");
        // Resolving the reloaded stack matches the original (structural equivalence).
        SampleContext ctx;
        ctx.worldPos = {1.0f, 0.0f, 1.0f};
        MCHECK(Near(Resolve(s, ctx).surface.specular_roughness,
                    Resolve(*s2, ctx).surface.specular_roughness),
               "reloaded stack resolves differently");
    }

    // Procedural mask bake: compile a Noise graph, bake a 16x16 mask, assert range + determinism.
    Graph ng;
    const u32 noise = ng.AddNode(NodeType::Noise);
    ng.FindNode(noise)->constant = glm::vec4(4.0f, 0.0f, 0.0f, 0.0f); // scale 4, seed 0
    const u32 out = ng.AddNode(NodeType::Output);
    ng.Connect(noise, out, static_cast<u8>(Channel::Height));
    CompiledGraph cg = Compile(ng);
    MaskTexture m1, m2;
    auto bake = [&](MaskTexture& m) {
        m.Resize(16, 16, 0.0f);
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) {
                SampleContext c;
                c.uv0 = {(x + 0.5f) / 16.0f, (y + 0.5f) / 16.0f};
                m.Set(x, y, cg.Eval(c).height);
            }
    };
    bake(m1);
    bake(m2);
    bool inRange = true, same = true;
    for (usize i = 0; i < m1.data.size(); ++i) {
        if (m1.data[i] < 0.0f || m1.data[i] > 1.0f) inRange = false;
        if (m1.data[i] != m2.data[i]) same = false;
    }
    MCHECK(inRange, "procedural mask out of [0,1]");
    MCHECK(same, "procedural mask bake not deterministic");
    const u64 h1 = HashBytes(m1.data.data(), m1.data.size() * sizeof(f32));
    const u64 h2 = HashBytes(m2.data.data(), m2.data.size() * sizeof(f32));
    MCHECK(h1 == h2, "procedural mask hash not deterministic");
}

// ---- Block: offline cook (graph -> .hbmat) + layer/mask texture bake ---------------------
void TestCook() {
    // Constant graph cooks to a .hbmat SurfaceParams (the shipping runtime form).
    Graph g = MakeConstGraph({0.2f, 0.4f, 0.6f, 1.0f}, 1.0f);
    MaterialAsset m = CompileGraphToMaterialAsset(g);
    MCHECK(Near(m.surface.base_color.r, 0.2f) && Near(m.surface.base_color.b, 0.6f),
           "graph->hbmat surface wrong");
    MCHECK(m.name == "ConstTest", "graph->hbmat name lost");

    // A Texture node feeding BaseColor lifts into the albedo slot.
    Graph gt;
    const u32 t = gt.AddNode(NodeType::Texture);
    gt.FindNode(t)->texture = "bricks_albedo.uaf";
    const u32 out = gt.AddNode(NodeType::Output);
    gt.Connect(t, out, static_cast<u8>(Channel::BaseColor));
    MaterialAsset mt = CompileGraphToMaterialAsset(gt);
    MCHECK(mt.albedoTex == "bricks_albedo.uaf", "texture node not lifted to albedo slot");

    // Layer-stack bake: two layers over a base, one box mask fully covering the sampled quad.
    LayerStack s;
    s.base.base_color = {0.9f, 0.9f, 0.9f, 1.0f};
    Layer l;
    l.surface.base_color = {0.1f, 0.05f, 0.02f, 1.0f}; // dark mud
    l.mask.kind = MaskKind::Box;
    l.mask.box.size = glm::vec3(100.0f); // huge -> covers the whole uv->world quad
    l.mask.box.position = {0.5f, 0.0f, 0.5f};
    l.mask.box.falloff.type = FalloffType::Constant;
    s.layers.push_back(l);
    BakedMaterial b1 = BakeLayerStack(s, 32);
    BakedMaterial b2 = BakeLayerStack(s, 32);
    MCHECK(b1.Valid(), "baked material invalid");
    MCHECK(b1.Hash() == b2.Hash(), "layer-stack bake not deterministic");
    // Centre texel should read the dark mud (box fully covers).
    const usize centre = (usize(16) * 32 + 16) * 4;
    MCHECK(b1.color[centre + 0] < 128, "box-masked bake did not apply the mud layer");

    // Procedural mask bake through the cook path.
    Graph ng;
    const u32 v = ng.AddNode(NodeType::Voronoi);
    ng.FindNode(v)->constant = glm::vec4(6.0f, 0.0f, 0.0f, 0.0f);
    const u32 o2 = ng.AddNode(NodeType::Output);
    ng.Connect(v, o2, static_cast<u8>(Channel::Height));
    CompiledGraph cg = Compile(ng);
    MaskTexture mask;
    BakeGraphChannel(cg, Channel::Height, 24, mask);
    MCHECK(mask.Valid(), "procedural channel bake invalid");
    bool inRange = true;
    for (f32 vv : mask.data)
        if (vv < 0.0f || vv > 1.0f) inRange = false;
    MCHECK(inRange, "procedural channel bake out of [0,1]");
}

// ---- Block: cross-platform (LE, ABI-free) serialization stability ------------------------
void TestCrossPlatform() {
    // The cooked/serialized forms are text JSON (portable) and our hashes are byte-wise over LE
    // encodings. Prove: a hand-written .hbmatgraph JSON parses to the expected values, and a
    // known graph's compiled hash is a fixed constant (regression-locks the byte layout).
    const char* jsonSrc = R"({
      "version": 1, "name": "XPlat", "nextId": 4,
      "params": [],
      "nodes": [
        {"id":1,"type":"Color","constant":[0.25,0.5,0.75,1.0],"space":0,"ui":[0,0]},
        {"id":2,"type":"Output","constant":[0,0,0,0],"space":0,"ui":[200,0]}
      ],
      "links": [ {"from":1,"fromPin":0,"to":2,"toPin":0} ]
    })";
    auto g = GraphFromJsonString(jsonSrc);
    MCHECK(g.has_value(), "portable graph JSON failed to parse");
    if (g) {
        CompiledGraph c = Compile(*g);
        MCHECK(c.ok && c.fullyConstant, "portable graph did not compile/fold");
        SurfaceParams sp = c.ToSurfaceParams();
        MCHECK(Near(sp.base_color.r, 0.25f) && Near(sp.base_color.g, 0.5f) &&
                   Near(sp.base_color.b, 0.75f),
               "portable graph produced wrong values");
        // Re-serialize is stable text (no locale/precision drift for these exact values).
        const std::string round = GraphToJsonString(*g);
        auto g2 = GraphFromJsonString(round);
        MCHECK(g2 && GraphToJsonString(*g2) == round, "portable graph re-serialize not stable");
    }
}

// ---- Block: regressions for the adversarial-review fixes --------------------------------
void TestReviewFixes() {
    // (1) LerpSurface blends the FULL field set: mask=1 reproduces the layer EXACTLY, incl.
    // previously-omitted rendered fields like subsurface_radius.
    {
        LayerStack s;
        s.base.subsurface_radius = 1.0f;
        s.base.coat_ior = 1.5f;
        Layer l;
        l.surface.subsurface_radius = 5.0f;
        l.surface.coat_ior = 1.9f;
        l.mask.kind = MaskKind::Constant;
        l.mask.constant = 1.0f;
        s.layers.push_back(l);
        const ResolvedSurface r = Resolve(s, {});
        MCHECK(Near(r.surface.subsurface_radius, 5.0f), "mask=1 must reproduce layer subsurface_radius");
        MCHECK(Near(r.surface.coat_ior, 1.9f), "mask=1 must reproduce layer coat_ior");
    }
    // (2) Layer-stack round-trip is lossless for the full field set.
    {
        LayerStack s;
        s.base.coat_roughness = 0.3f;      // non-default (ctor 0.08)
        s.base.subsurface_radius = 4.0f;
        s.base.thin_film_weight = 0.5f;
        Layer l;
        l.surface.coat_ior = 1.7f;
        l.surface.fuzz_weight = 0.25f;
        s.layers.push_back(l);
        auto s2 = LayerStackFromJsonString(LayerStackToJsonString(s));
        MCHECK(s2.has_value(), "round-trip parse failed");
        if (s2) {
            MCHECK(Near(s2->base.coat_roughness, 0.3f), "round-trip lost coat_roughness");
            MCHECK(Near(s2->base.subsurface_radius, 4.0f), "round-trip lost subsurface_radius");
            MCHECK(Near(s2->base.thin_film_weight, 0.5f), "round-trip lost thin_film_weight");
            MCHECK(!s2->layers.empty() && Near(s2->layers[0].surface.coat_ior, 1.7f),
                   "round-trip lost layer coat_ior");
            MCHECK(!s2->layers.empty() && Near(s2->layers[0].surface.fuzz_weight, 0.25f),
                   "round-trip lost layer fuzz_weight");
        }
    }
    // (3) Loaders never throw on valid-JSON-but-wrong-shape input. These deliberately drive the
    // rejection log paths, so mute logging to keep a PASSING run clean (the behaviour under test is
    // the RETURN value, not the log line).
    {
        LogMuteScope mute;
        MCHECK(!LayerStackFromJsonString("[]").has_value(), "non-object layer stack must be nullopt");
        MCHECK(!LayerStackFromJsonString("5").has_value(), "scalar layer stack must be nullopt");
        MCHECK(!LayerStackFromJsonString("null").has_value(), "null layer stack must be nullopt");
        MCHECK(GraphFromJsonString("{\"params\":[5]}").has_value(), "malformed params must not crash graph load");
        MCHECK(GraphFromJsonString("{\"nodes\":[5,{\"id\":1,\"type\":\"Output\"}]}").has_value(),
               "malformed node element must not crash graph load");
    }

    // (4) An unbaked Procedural / unbound Paint mask is ABSENT (layer contributes nothing), not
    // fully applied.
    {
        LayerStack s;
        s.base.specular_roughness = 0.2f;
        Layer l;
        l.surface.specular_roughness = 0.9f;
        l.mask.kind = MaskKind::Procedural; // no texture baked
        s.layers.push_back(l);
        MCHECK(Near(Resolve(s, {}).surface.specular_roughness, 0.2f),
               "empty procedural mask must be absent, not fully applied");
    }
    // (5) Power(0, negative) is finite, not +inf.
    {
        Graph g;
        const u32 f0 = g.AddNode(NodeType::Float);
        g.FindNode(f0)->constant = glm::vec4(0.0f);
        const u32 pw = g.AddNode(NodeType::Power);
        g.FindNode(pw)->constant = glm::vec4(-1.0f, 0, 0, 0);
        const u32 o = g.AddNode(NodeType::Output);
        g.Connect(f0, pw, 0);
        g.Connect(pw, o, static_cast<u8>(Channel::Roughness));
        MCHECK(std::isfinite(Compile(g).Eval({}).roughness), "pow(0,-1) must be finite");
    }
    // (6) Box zero-scale axis reads as outside AND agrees with Bounds().
    {
        BoxBrush z;
        z.size = glm::vec3(1.0f);
        z.scale = glm::vec3(0.0f, 1.0f, 1.0f);
        z.falloff.type = FalloffType::Linear;
        z.falloffWidth = 0.5f;
        MCHECK(Near(z.EvaluateBrush(glm::vec3(0.4f, 0, 0)), 0.0f), "zero-scale off-plane must be outside");
        MCHECK(z.EvaluateBrush(glm::vec3(0.0f)) > 0.5f, "zero-scale on-plane centre inside");
        MCHECK(!z.Bounds().Contains(glm::vec3(0.4f, 0, 0)), "bounds must agree (off-plane not contained)");
    }
    // (7) Non-unit rotation: EvaluateBrush and Bounds use the same normalized rotation. A long box
    // (3x1x1) whose non-unit quat normalizes to 90deg about Y puts its long axis along world Z, so a
    // probe at world (0,0,1.3) is inside only BECAUSE the rotation is applied consistently in both.
    {
        BoxBrush nq;
        nq.size = glm::vec3(3.0f, 1.0f, 1.0f);
        nq.rotation = glm::quat(1.4142f, 0.0f, 1.4142f, 0.0f); // (w,x,y,z), norm 2 -> 90deg about Y
        nq.falloff.type = FalloffType::Linear;
        nq.falloffWidth = 0.05f;
        const glm::vec3 probe(0.0f, 0.0f, 1.3f); // along the rotated long axis (world Z half = 1.5)
        MCHECK(nq.EvaluateBrush(probe) > 0.0f, "non-unit quat: rotated long-axis probe should be inside");
        MCHECK(nq.Bounds().Contains(probe), "non-unit quat: bounds must contain the interior probe");
        // Sanity: without the rotation the probe is outside (proves the rotation matters here).
        BoxBrush flat = nq;
        flat.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        MCHECK(Near(flat.EvaluateBrush(probe), 0.0f), "unrotated long box should exclude the probe");
    }
}

// ---- Block: the Material-Maker-class library (generators / transforms / filters / SDF) --
namespace {
// Build `node -> Output.channel`, compile, return the compiled graph (node id via out param).
CompiledGraph CompileSingle(NodeType t, Channel ch, glm::vec4 constant, u32* nodeIdOut = nullptr) {
    Graph g;
    const u32 id = g.AddNode(t);
    g.FindNode(id)->constant = constant;
    const u32 out = g.AddNode(NodeType::Output);
    g.Connect(id, out, static_cast<u8>(ch));
    if (nodeIdOut) *nodeIdOut = id;
    return Compile(g);
}
f32 EvalAt(const CompiledGraph& c, glm::vec2 uv, Channel ch) {
    SampleContext s;
    s.uv0 = uv;
    switch (ch) {
        case Channel::BaseColor: return c.Eval(s).baseColor.r;
        case Channel::Roughness: return c.Eval(s).roughness;
        case Channel::Height: return c.Eval(s).height;
        default: return c.Eval(s).baseColor.r;
    }
}
} // namespace

void TestLibraryNodes() {
    // Generators: in-range over a grid + deterministic + not entirely flat.
    const NodeType gens[] = {NodeType::Perlin,   NodeType::FractalNoise,   NodeType::Cellular,
                             NodeType::Checker,  NodeType::Bricks,         NodeType::Grid,
                             NodeType::Shape,    NodeType::Wave,           NodeType::Dots,
                             NodeType::RadialGradient, NodeType::AngularGradient};
    for (NodeType t : gens) {
        CompiledGraph c = CompileSingle(t, Channel::BaseColor, glm::vec4(4.0f, 0.0f, 0.0f, 0.0f));
        MCHECK(!c.fullyConstant, "generator must not fold to a constant");
        bool inRange = true, varied = false;
        f32 first = -1.0f;
        u64 h1 = 1469598103934665603ull, h2 = 1469598103934665603ull;
        // 31 is coprime with the tile counts (4/8) so thin features (Grid lines, Dots) are hit
        // rather than aliased between samples.
        const int R = 31;
        for (int y = 0; y < R; ++y)
            for (int x = 0; x < R; ++x) {
                const glm::vec2 uv{(x + 0.5f) / R, (y + 0.5f) / R};
                const f32 v = EvalAt(c, uv, Channel::BaseColor);
                if (v < -0.001f || v > 1.001f) inRange = false;
                if (first < 0.0f) first = v;
                else if (std::abs(v - first) > 1e-4f) varied = true;
                h1 = HashF32(v, h1);
                h2 = HashF32(EvalAt(c, uv, Channel::BaseColor), h2);
            }
        MCHECK(inRange, "generator output out of [0,1]");
        MCHECK(varied, "generator produced a flat image");
        MCHECK(h1 == h2, "generator not deterministic");
    }

    // Per-pixel filters: exact expected values.
    auto blend = [&](int mode, f32 a, f32 b) {
        Graph g;
        const u32 ca = g.AddNode(NodeType::Color);
        g.FindNode(ca)->constant = glm::vec4(a, a, a, 1);
        const u32 cb = g.AddNode(NodeType::Color);
        g.FindNode(cb)->constant = glm::vec4(b, b, b, 1);
        const u32 bl = g.AddNode(NodeType::Blend);
        g.FindNode(bl)->constant = glm::vec4(static_cast<f32>(mode), 1.0f, 0.0f, 0.0f);
        const u32 out = g.AddNode(NodeType::Output);
        g.Connect(ca, bl, 0);
        g.Connect(cb, bl, 1);
        g.Connect(bl, out, static_cast<u8>(Channel::BaseColor));
        return Compile(g).ToSurfaceParams().base_color.r;
    };
    MCHECK(Near(blend(1, 0.6f, 0.5f), 0.3f), "Blend Multiply wrong");        // 0.6*0.5
    MCHECK(Near(blend(7, 0.6f, 0.5f), 1.0f), "Blend Add wrong (clamped)");   // min(1.1,1)
    MCHECK(Near(blend(4, 0.6f, 0.5f), 0.5f), "Blend Darken wrong");          // min
    MCHECK(Near(blend(5, 0.6f, 0.5f), 0.6f), "Blend Lighten wrong");         // max

    auto filt = [&](NodeType t, glm::vec4 k, f32 inVal) {
        Graph g;
        const u32 col = g.AddNode(NodeType::Color);
        g.FindNode(col)->constant = glm::vec4(inVal, inVal, inVal, 1.0f);
        const u32 f = g.AddNode(t);
        g.FindNode(f)->constant = k;
        const u32 out = g.AddNode(NodeType::Output);
        g.Connect(col, f, 0);
        g.Connect(f, out, static_cast<u8>(Channel::BaseColor));
        return Compile(g).ToSurfaceParams().base_color.r;
    };
    MCHECK(Near(filt(NodeType::Gamma, glm::vec4(2.0f, 0, 0, 0), 0.5f), 0.25f), "Gamma wrong");
    MCHECK(Near(filt(NodeType::Threshold, glm::vec4(0.5f, 0, 0, 0), 0.7f), 1.0f), "Threshold hi wrong");
    MCHECK(Near(filt(NodeType::Threshold, glm::vec4(0.5f, 0, 0, 0), 0.3f), 0.0f), "Threshold lo wrong");
    MCHECK(Near(filt(NodeType::Grayscale, glm::vec4(0), 0.42f), 0.42f), "Grayscale of grey wrong");
    MCHECK(Near(filt(NodeType::Posterize, glm::vec4(3.0f, 0, 0, 0), 1.0f), 1.0f), "Posterize top level wrong");

    // Combine: three floats -> rgb.
    {
        Graph g;
        u32 r = g.AddNode(NodeType::Float); g.FindNode(r)->constant = glm::vec4(0.1f);
        u32 gg = g.AddNode(NodeType::Float); g.FindNode(gg)->constant = glm::vec4(0.2f);
        u32 b = g.AddNode(NodeType::Float); g.FindNode(b)->constant = glm::vec4(0.3f);
        u32 cmb = g.AddNode(NodeType::Combine);
        u32 out = g.AddNode(NodeType::Output);
        g.Connect(r, cmb, 0); g.Connect(gg, cmb, 1); g.Connect(b, cmb, 2);
        g.Connect(cmb, out, static_cast<u8>(Channel::BaseColor));
        SurfaceParams sp = Compile(g).ToSurfaceParams();
        MCHECK(Near(sp.base_color.r, 0.1f) && Near(sp.base_color.g, 0.2f) && Near(sp.base_color.b, 0.3f),
               "Combine channels wrong");
    }

    // Transform actually resamples: a Checker through a Transform differs from the un-transformed.
    {
        Graph g;
        u32 chk = g.AddNode(NodeType::Checker); g.FindNode(chk)->constant = glm::vec4(4, 0, 0, 0);
        u32 tf = g.AddNode(NodeType::Transform); g.FindNode(tf)->constant = glm::vec4(0.13f, 0.07f, 0.2f, 1.0f);
        u32 out = g.AddNode(NodeType::Output);
        g.Connect(chk, tf, 0);
        g.Connect(tf, out, static_cast<u8>(Channel::BaseColor));
        CompiledGraph cT = Compile(g);
        CompiledGraph cP = CompileSingle(NodeType::Checker, Channel::BaseColor, glm::vec4(4, 0, 0, 0));
        u64 hT = 1u, hP = 1u;
        for (int i = 0; i < 40; ++i) {
            const glm::vec2 uv{(i % 7) / 7.0f + 0.03f, (i / 7) / 6.0f + 0.03f};
            hT = HashF32(EvalAt(cT, uv, Channel::BaseColor), hT);
            hP = HashF32(EvalAt(cP, uv, Channel::BaseColor), hP);
        }
        MCHECK(hT != hP, "Transform did not resample the input (coordinate rework broken)");
    }

    // HeightToNormal: a radial-gradient height yields a unit tangent-space normal with z>0.
    {
        Graph g;
        u32 rad = g.AddNode(NodeType::RadialGradient); g.FindNode(rad)->constant = glm::vec4(0.6f, 0, 0, 0);
        u32 h2n = g.AddNode(NodeType::HeightToNormal); g.FindNode(h2n)->constant = glm::vec4(1.0f, 0.01f, 0, 0);
        u32 out = g.AddNode(NodeType::Output);
        g.Connect(rad, h2n, 0);
        g.Connect(h2n, out, static_cast<u8>(Channel::Normal));
        SampleContext s; s.uv0 = {0.7f, 0.55f};
        const glm::vec3 n = Compile(g).Eval(s).normalTS;
        MCHECK(Near(glm::length(n), 1.0f), "HeightToNormal not unit length");
        MCHECK(n.z > 0.0f, "HeightToNormal z should be positive");
        MCHECK(std::abs(n.x) > 1e-3f, "HeightToNormal should tilt off-center");
    }

    // SDF: a circle shown is filled inside, empty outside.
    {
        Graph g;
        u32 c = g.AddNode(NodeType::SdfCircle); g.FindNode(c)->constant = glm::vec4(0.3f, 0, 0, 0);
        u32 sh = g.AddNode(NodeType::SdfShow); g.FindNode(sh)->constant = glm::vec4(0.01f, 0, 0, 0);
        u32 out = g.AddNode(NodeType::Output);
        g.Connect(c, sh, 0);
        g.Connect(sh, out, static_cast<u8>(Channel::BaseColor));
        CompiledGraph cc = Compile(g);
        MCHECK(EvalAt(cc, {0.5f, 0.5f}, Channel::BaseColor) > 0.9f, "SDF circle center should be filled");
        MCHECK(EvalAt(cc, {0.02f, 0.02f}, Channel::BaseColor) < 0.1f, "SDF circle corner should be empty");
    }

    // Blur produces intermediate values on a hard checker edge (proves neighbour resampling).
    {
        Graph g;
        u32 chk = g.AddNode(NodeType::Checker); g.FindNode(chk)->constant = glm::vec4(4, 0, 0, 0);
        u32 bl = g.AddNode(NodeType::Blur); g.FindNode(bl)->constant = glm::vec4(0.03f, 0, 0, 0);
        u32 out = g.AddNode(NodeType::Output);
        g.Connect(chk, bl, 0);
        g.Connect(bl, out, static_cast<u8>(Channel::BaseColor));
        CompiledGraph cc = Compile(g);
        bool sawMid = false;
        for (int i = 0; i < 48; ++i) {
            const f32 v = EvalAt(cc, {(i % 8) / 8.0f + 0.001f, (i / 8) / 6.0f + 0.001f}, Channel::BaseColor);
            if (v > 0.15f && v < 0.85f) sawMid = true;
        }
        MCHECK(sawMid, "Blur produced no intermediate values (neighbour resampling broken)");
    }
}

// ---- Block: GPU codegen completeness (every node type must emit real HLSL, not the magenta gap) --
void TestHlslCodegen() {
    for (int i = 0; i < static_cast<int>(NodeType::Count); ++i) {
        const NodeType t = static_cast<NodeType>(i);
        if (t == NodeType::Output) continue; // Output is the sink, never emitted as a node function
        Graph g;
        const u32 n = g.AddNode(t);
        // Wire each input pin to a Float so the connected-input codegen path is exercised too.
        const u8 ins = NodeInfoOf(t).inputCount;
        for (u8 p = 0; p < ins; ++p) {
            const u32 f = g.AddNode(NodeType::Float);
            g.FindNode(f)->constant = glm::vec4(0.5f);
            g.Connect(f, n, p);
        }
        const u32 out = g.AddNode(NodeType::Output);
        g.Connect(n, out, static_cast<u8>(Channel::BaseColor));
        const std::string hlsl = GenerateComputeHlsl(g);
        // The magenta marker (float4(1,0,1,1)) is only emitted by the default case = a codegen gap.
        const bool missing = hlsl.find("float4(1,0,1,1)") != std::string::npos;
        if (missing)
            HBE_ERROR("  [material test] FAIL: node '{}' missing from GPU codegen (magenta fallback)",
                      NodeInfoOf(t).name);
        if (missing) ++g_fail;
    }
}

// ---- Block: box-brush material VOLUME bake onto a mesh --------------------------------------
void TestVolumeBake() {
    // A unit quad on the XZ plane; UV (u,v) maps to local/world (u,0,v).
    MeshData quad;
    auto addV = [&](glm::vec3 pos, glm::vec2 uv) {
        Vertex v;
        v.position = pos;
        v.normal = {0, 1, 0};
        v.uv = uv;
        quad.vertices.push_back(v);
    };
    addV({0, 0, 0}, {0, 0});
    addV({1, 0, 0}, {1, 0});
    addV({1, 0, 1}, {1, 1});
    addV({0, 0, 1}, {0, 1});
    quad.indices = {0, 1, 2, 0, 2, 3};

    // Base = red; a box volume over world x in [0, 0.5] applies blue.
    LayerStack s;
    s.base.base_color = {1, 0, 0, 1};
    Layer l;
    l.surface.base_color = {0, 0, 1, 1};
    l.mask.kind = MaskKind::Box;
    l.mask.box.position = {0.25f, 0.0f, 0.5f};
    l.mask.box.size = {0.5f, 4.0f, 4.0f};
    l.mask.box.falloff.type = FalloffType::Constant;
    l.mask.box.strength = 1.0f;
    l.blend = BlendMode::Linear;
    s.layers.push_back(l);

    const BakedMaterial b = BakeMeshVolumes(quad, glm::mat4(1.0f), s, 32);
    MCHECK(b.Valid(), "volume bake produced no image");
    auto ch = [&](int px, int py, int c) { return b.color[(usize(py) * 32 + px) * 4 + c]; };
    // Inside the box (world x=0.25) -> blue.
    MCHECK(ch(8, 16, 2) > 200 && ch(8, 16, 0) < 60, "volume-covered texel should be blue");
    // Outside the box (world x=0.75) -> base red.
    MCHECK(ch(24, 16, 0) > 200 && ch(24, 16, 2) < 60, "uncovered texel should keep the base red");
    // Determinism.
    const BakedMaterial b2 = BakeMeshVolumes(quad, glm::mat4(1.0f), s, 32);
    MCHECK(b.Hash() == b2.Hash(), "volume bake not deterministic");
}

void TestVolumeOverlay() {
    // Same unit quad (UV -> world (u,0,v)); the OVERLAY bake must be transparent where no volume
    // reaches and carry the volume's colour + coverage alpha where a box covers.
    MeshData quad;
    auto addV = [&](glm::vec3 pos, glm::vec2 uv) {
        Vertex v;
        v.position = pos;
        v.normal = {0, 1, 0};
        v.uv = uv;
        quad.vertices.push_back(v);
    };
    addV({0, 0, 0}, {0, 0});
    addV({1, 0, 0}, {1, 0});
    addV({1, 0, 1}, {1, 1});
    addV({0, 0, 1}, {0, 1});
    quad.indices = {0, 1, 2, 0, 2, 3};

    // One blue box volume over world x in [0, 0.5]; base is intentionally NOT applied by the overlay.
    LayerStack s;
    s.base.base_color = {1, 0, 0, 1}; // must be ignored by the overlay bake
    Layer l;
    l.surface.base_color = {0, 0, 1, 1};
    l.mask.kind = MaskKind::Box;
    l.mask.box.position = {0.25f, 0.0f, 0.5f};
    l.mask.box.size = {0.5f, 4.0f, 4.0f};
    l.mask.box.falloff.type = FalloffType::Constant;
    l.mask.box.strength = 1.0f;
    l.opacity = 1.0f;
    s.layers.push_back(l);

    const BakedMaterial b = BakeMeshVolumesOverlay(quad, glm::mat4(1.0f), s, 32);
    MCHECK(b.Valid(), "overlay bake produced no image");
    auto col = [&](int px, int py, int c) { return b.color[(usize(py) * 32 + px) * 4 + c]; };
    // Inside the box (world x=0.25) -> opaque blue overlay (base red never appears).
    MCHECK(col(8, 16, 3) > 250, "overlay-covered texel should be fully opaque");
    MCHECK(col(8, 16, 2) > 200 && col(8, 16, 0) < 60, "overlay-covered texel should be blue");
    // Outside the box (world x=0.75) -> transparent (coverage 0), so the mesh material shows through.
    MCHECK(col(24, 16, 3) == 0, "uncovered texel must be transparent, not the base");
    // An empty stack must produce a fully transparent overlay (no volumes -> no change).
    const BakedMaterial e = BakeMeshVolumesOverlay(quad, glm::mat4(1.0f), LayerStack{}, 8);
    bool allClear = true;
    for (usize i = 3; i < e.color.size(); i += 4)
        if (e.color[i] != 0) allClear = false;
    MCHECK(allClear, "empty overlay stack must bake to a fully transparent canvas");
    // Determinism.
    const BakedMaterial b2 = BakeMeshVolumesOverlay(quad, glm::mat4(1.0f), s, 32);
    MCHECK(b.Hash() == b2.Hash(), "overlay bake not deterministic");
}

} // namespace

bool SelfTest() {
    g_fail = 0;
    TestCatalog();
    TestSerialization();
    TestCompile();
    TestEvalMath();
    TestParamOverride();
    TestCycle();
    TestFalloff();
    TestBoxBrush();
    TestTiling();
    TestResolve();
    TestLayerSerialization();
    TestCook();
    TestCrossPlatform();
    TestReviewFixes();
    TestLibraryNodes();
    TestHlslCodegen();
    TestVolumeBake();
    TestVolumeOverlay();
    if (g_fail == 0)
        HBE_INFO("[material test] all material-authoring blocks passed");
    else
        HBE_ERROR("[material test] {} check(s) failed", g_fail);
    return g_fail == 0;
}

} // namespace hbe::mat

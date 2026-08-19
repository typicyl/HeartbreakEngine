// Material/MaterialAuthoringTest.cpp - see MaterialAuthoringTest.h.
//
// P1 blocks (graph + compiler + params) are implemented here. P2 blocks (layers / masks / box
// brush / falloff / tiling) are appended in the same SelfTest() as those modules land, so the
// single `--test-material` flag always runs the whole suite.
#include "Material/MaterialAuthoringTest.h"

#include "Core/Log.h"
#include "Material/BoxBrush.h"
#include "Material/MaterialGraph.h"
#include "Material/MaterialGraphCompiler.h"
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

    // Unknown node type is dropped, not fatal (forward-compat).
    std::string mangled = s1;
    const auto pos = mangled.find("\"Color\"");
    if (pos != std::string::npos) mangled.replace(pos, 7, "\"FutureNode99\"");
    auto g3 = GraphFromJsonString(mangled);
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
    if (g_fail == 0)
        HBE_INFO("[material test] all material-authoring blocks passed");
    else
        HBE_ERROR("[material test] {} check(s) failed", g_fail);
    return g_fail == 0;
}

} // namespace hbe::mat

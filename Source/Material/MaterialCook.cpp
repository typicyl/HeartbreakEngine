// Material/MaterialCook.cpp - see MaterialCook.h.
#include "Material/MaterialCook.h"

#include <algorithm>
#include <cmath>

namespace hbe::mat {

namespace {
u8 ToU8(f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Linear -> sRGB (per-channel), matching the color canvas being an SRGB texture.
f32 LinearToSrgb(f32 c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
// Encode a tangent-space normal (z-up, [-1,1]) to RGB [0,1].
glm::vec3 EncodeNormal(const glm::vec3& n) {
    const glm::vec3 u = glm::normalize(glm::length(n) > 1e-6f ? n : glm::vec3(0, 0, 1));
    return u * 0.5f + 0.5f;
}

// Rasterize `mesh` into a `resolution`^2 UV image (its texture atlas). For every texel a triangle
// covers, invoke `sink(byteOffset, ctx)` with a SampleContext whose objectPos / worldPos / uv0 /
// normal are the barycentric-interpolated vertex attributes (worldPos = worldTransform * local,
// normal transformed by the upper 3x3). Shared by both mesh-volume bakes so the coverage rules
// (edge test, degenerate-UV skip, world mapping) stay identical between them.
template <class Sink>
void RasterizeMeshUV(const MeshData& mesh, const glm::mat4& worldTransform, u32 resolution,
                     Sink&& sink) {
    if (mesh.indices.size() < 3 || mesh.vertices.empty()) return;
    const f32 R = static_cast<f32>(resolution);
    const glm::mat3 nrmMat = glm::mat3(worldTransform);
    const std::vector<Vertex>& V = mesh.vertices;
    const std::vector<u32>& I = mesh.indices;
    for (usize t = 0; t + 2 < I.size(); t += 3) {
        if (I[t] >= V.size() || I[t + 1] >= V.size() || I[t + 2] >= V.size()) continue;
        const Vertex& v0 = V[I[t]];
        const Vertex& v1 = V[I[t + 1]];
        const Vertex& v2 = V[I[t + 2]];
        const glm::vec2 p0 = v0.uv * R, p1 = v1.uv * R, p2 = v2.uv * R;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
        const int x1 = std::min(static_cast<int>(resolution) - 1,
                                static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
        const int y1 = std::min(static_cast<int>(resolution) - 1,
                                static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
        const f32 den = (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
        if (std::abs(den) < 1e-9f) continue; // degenerate UV triangle
        const f32 invDen = 1.0f / den;
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px) {
                const glm::vec2 p(px + 0.5f, py + 0.5f);
                const f32 w0 = ((p1.y - p2.y) * (p.x - p2.x) + (p2.x - p1.x) * (p.y - p2.y)) * invDen;
                const f32 w1 = ((p2.y - p0.y) * (p.x - p2.x) + (p0.x - p2.x) * (p.y - p2.y)) * invDen;
                const f32 w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue; // outside the triangle
                const glm::vec3 localPos = v0.position * w0 + v1.position * w1 + v2.position * w2;
                SampleContext ctx;
                ctx.objectPos = localPos;
                ctx.worldPos = glm::vec3(worldTransform * glm::vec4(localPos, 1.0f));
                ctx.uv0 = {(px + 0.5f) / R, (py + 0.5f) / R};
                const glm::vec3 wn = nrmMat * (v0.normal * w0 + v1.normal * w1 + v2.normal * w2);
                if (glm::length(wn) > 1e-6f) ctx.normal = glm::normalize(wn);
                sink((usize(py) * resolution + px) * 4, ctx);
            }
    }
}
} // namespace

BakedMaterial BakeLayerStackWorld(const LayerStack& stack, u32 resolution, glm::vec2 worldMin,
                                  glm::vec2 worldMax, const TextureProvider& tex) {
    (void)tex; // layer surfaces are already resolved values; per-layer texture sampling is future
    BakedMaterial b;
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    b.width = b.height = resolution;
    const usize n = usize(resolution) * resolution * 4;
    b.color.resize(n);
    b.material.resize(n);
    b.normal.resize(n);
    for (u32 y = 0; y < resolution; ++y) {
        for (u32 x = 0; x < resolution; ++x) {
            SampleContext ctx;
            ctx.uv0 = {(x + 0.5f) / resolution, (y + 0.5f) / resolution};
            ctx.worldPos = {glm::mix(worldMin.x, worldMax.x, ctx.uv0.x), 0.0f,
                            glm::mix(worldMin.y, worldMax.y, ctx.uv0.y)};
            ctx.objectPos = ctx.worldPos;
            const ResolvedSurface r = Resolve(stack, ctx);
            const usize o = (usize(y) * resolution + x) * 4;
            const glm::vec3 c = glm::vec3(r.surface.base_color);
            b.color[o + 0] = ToU8(LinearToSrgb(c.r));
            b.color[o + 1] = ToU8(LinearToSrgb(c.g));
            b.color[o + 2] = ToU8(LinearToSrgb(c.b));
            b.color[o + 3] = ToU8(r.surface.base_color.a);
            b.material[o + 0] = ToU8(r.surface.base_metalness);
            b.material[o + 1] = ToU8(r.surface.specular_roughness);
            b.material[o + 2] = ToU8(r.height);
            b.material[o + 3] = 255; // ao (resolved AO channel is future; opaque for now)
            const glm::vec3 en = EncodeNormal(r.normalTS);
            b.normal[o + 0] = ToU8(en.r);
            b.normal[o + 1] = ToU8(en.g);
            b.normal[o + 2] = ToU8(en.b);
            b.normal[o + 3] = 255;
        }
    }
    return b;
}

BakedMaterial BakeLayerStack(const LayerStack& stack, u32 resolution, const TextureProvider& tex) {
    return BakeLayerStackWorld(stack, resolution, glm::vec2(0.0f), glm::vec2(1.0f), tex);
}

void BakeGraphColor(const CompiledGraph& compiled, u32 resolution, glm::vec2 worldMin,
                    glm::vec2 worldMax, std::vector<u8>& rgba, const TextureProvider& tex) {
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    rgba.assign(usize(resolution) * resolution * 4, 0);
    for (u32 y = 0; y < resolution; ++y)
        for (u32 x = 0; x < resolution; ++x) {
            SampleContext ctx;
            ctx.uv0 = {(x + 0.5f) / resolution, (y + 0.5f) / resolution};
            ctx.worldPos = {glm::mix(worldMin.x, worldMax.x, ctx.uv0.x), 0.0f,
                            glm::mix(worldMin.y, worldMax.y, ctx.uv0.y)};
            ctx.objectPos = ctx.worldPos;
            const SurfaceSample s = compiled.Eval(ctx, tex);
            const usize o = (usize(y) * resolution + x) * 4;
            rgba[o + 0] = ToU8(LinearToSrgb(s.baseColor.r));
            rgba[o + 1] = ToU8(LinearToSrgb(s.baseColor.g));
            rgba[o + 2] = ToU8(LinearToSrgb(s.baseColor.b));
            rgba[o + 3] = ToU8(s.opacity);
        }
}

void BakeGraphChannel(const CompiledGraph& compiled, Channel channel, u32 resolution,
                      MaskTexture& out, const TextureProvider& tex) {
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    out.Resize(resolution, resolution, 0.0f);
    for (u32 y = 0; y < resolution; ++y)
        for (u32 x = 0; x < resolution; ++x) {
            SampleContext ctx;
            ctx.uv0 = {(x + 0.5f) / resolution, (y + 0.5f) / resolution};
            ctx.worldPos = {ctx.uv0.x, 0.0f, ctx.uv0.y};
            const SurfaceSample s = compiled.Eval(ctx, tex);
            f32 v;
            switch (channel) {
                case Channel::Roughness: v = s.roughness; break;
                case Channel::Metallic: v = s.metallic; break;
                case Channel::Height: v = s.height; break;
                case Channel::AO: v = s.ao; break;
                case Channel::Opacity: v = s.opacity; break;
                default: v = s.baseColor.r; break; // BaseColor/Emissive -> luminance-ish (r)
            }
            out.Set(x, y, std::clamp(v, 0.0f, 1.0f));
        }
}

MaterialAsset CompileGraphToMaterialAsset(const Graph& g, const std::vector<ParamOverride>& overrides) {
    MaterialAsset m;
    m.name = g.name;
    const CompiledGraph c = Compile(g, overrides);
    if (!c.ok) return m; // returns a default material; caller checks c.ok separately if needed
    m.surface = c.ToSurfaceParams();
    m.flags = 0;

    // Lift a Texture/NormalMap node feeding a channel into the matching .hbmat slot. Operate on the
    // source Graph (node identity is lost in the flattened CompiledGraph).
    const Node* outNode = g.OutputNode();
    if (outNode) {
        auto texForChannel = [&](Channel ch) -> std::string {
            if (const Link* l = g.LinkInto(outNode->id, static_cast<u8>(ch)))
                if (const Node* src = g.FindNode(l->fromNode))
                    if (src->type == NodeType::Texture || src->type == NodeType::NormalMap ||
                        src->type == NodeType::Height)
                        return src->texture;
            return {};
        };
        m.albedoTex = texForChannel(Channel::BaseColor);
        m.normalTex = texForChannel(Channel::Normal);
        m.emissiveTex = texForChannel(Channel::Emissive);
        // Roughness/Metallic share the MR slot in the engine; take whichever is a texture.
        std::string mr = texForChannel(Channel::Roughness);
        if (mr.empty()) mr = texForChannel(Channel::Metallic);
        m.mrTex = mr;
        m.aoTex = texForChannel(Channel::AO);
    }
    return m;
}

// ---- Visual test scene ------------------------------------------------------------------
namespace {
NamedImage MaskToImage(const std::string& name, const MaskTexture& m) {
    NamedImage img;
    img.name = name;
    img.width = m.width;
    img.height = m.height;
    img.rgba.resize(usize(m.width) * m.height * 4);
    for (u32 i = 0; i < m.width * m.height; ++i) {
        const u8 v = ToU8(m.data[i]);
        img.rgba[i * 4 + 0] = v;
        img.rgba[i * 4 + 1] = v;
        img.rgba[i * 4 + 2] = v;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}
NamedImage ColorToImage(const std::string& name, u32 res, std::vector<u8> rgba) {
    NamedImage img;
    img.name = name;
    img.width = img.height = res;
    img.rgba = std::move(rgba);
    return img;
}
// Bake a box-brush weight field over a world quad into a mask (the box's spatial weight, visualised).
void BakeBoxMask(const BoxBrush& box, u32 res, glm::vec2 wmin, glm::vec2 wmax, MaskTexture& out,
                 bool takeMax) {
    if (!out.Valid()) out.Resize(res, res, 0.0f);
    for (u32 y = 0; y < res; ++y)
        for (u32 x = 0; x < res; ++x) {
            const glm::vec3 wp(glm::mix(wmin.x, wmax.x, (x + 0.5f) / res), 0.0f,
                               glm::mix(wmin.y, wmax.y, (y + 0.5f) / res));
            const f32 w = box.EvaluateBrush(wp);
            out.Set(x, y, takeMax ? std::max(out.At(x, y), w) : w);
        }
}
// Crisp world-space CHECKER over a quad: the clearest possible proof that a fixed tile size stays
// fixed regardless of surface size (a 1m checker shows 1 square on a 1m quad, 16 on a 4m quad).
void BakeWorldChecker(u32 res, glm::vec2 wmin, glm::vec2 wmax, f32 tile, std::vector<u8>& rgba) {
    rgba.assign(usize(res) * res * 4, 0);
    for (u32 y = 0; y < res; ++y)
        for (u32 x = 0; x < res; ++x) {
            const f32 wx = glm::mix(wmin.x, wmax.x, (x + 0.5f) / res);
            const f32 wz = glm::mix(wmin.y, wmax.y, (y + 0.5f) / res);
            const int cx = static_cast<int>(std::floor(wx / tile));
            const int cz = static_cast<int>(std::floor(wz / tile));
            const bool on = ((cx + cz) & 1) != 0;
            const u8 v = on ? 210 : 70;
            const usize o = (usize(y) * res + x) * 4;
            rgba[o + 0] = on ? v : 60;
            rgba[o + 1] = v;
            rgba[o + 2] = on ? 60 : v; // tinted so the two tiles read at a glance
            rgba[o + 3] = 255;
        }
}
// Soft additive paint dab into a UV-space mask (a stand-in for a real paint stroke).
void StampDab(MaskTexture& m, glm::vec2 uv, f32 radius, f32 strength) {
    for (u32 y = 0; y < m.height; ++y)
        for (u32 x = 0; x < m.width; ++x) {
            const glm::vec2 p((x + 0.5f) / m.width, (y + 0.5f) / m.height);
            const f32 d = glm::length(p - uv) / std::max(radius, 1e-4f);
            if (d < 1.0f) {
                const f32 w = strength * (1.0f - d * d);
                m.Set(x, y, std::min(1.0f, m.At(x, y) + w));
            }
        }
}
} // namespace

std::vector<NamedImage> BuildDemoScene(u32 res) {
    res = std::clamp<u32>(res, 16u, 2048u);
    std::vector<NamedImage> out;

    // (A) WORLD-SPACE TILING at two surface sizes, shown two ways.
    // (A1) A CRISP 1m checker baked over a 1m and a 4m quad: 1 square vs 16 squares of the SAME
    // physical size -> the tile stays 1m regardless of surface size (NO stretch). The clearest proof.
    {
        std::vector<u8> c1, c4;
        BakeWorldChecker(res, {0, 0}, {1, 1}, 0.5f, c1); // 0.5m checker -> 2x2 on a 1m quad
        BakeWorldChecker(res, {0, 0}, {4, 4}, 0.5f, c4); //             -> 8x8 on a 4m quad
        out.push_back(ColorToImage("tiling_checker_1m", res, std::move(c1)));
        out.push_back(ColorToImage("tiling_checker_4m", res, std::move(c4)));
    }
    // (A2) The same idea with a procedural Voronoi material (2 cells/m, world space).
    {
        Graph g;
        const u32 v = g.AddNode(NodeType::Voronoi);
        g.FindNode(v)->constant = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f); // 2 cells / metre
        g.FindNode(v)->space = Space::World;
        const u32 o = g.AddNode(NodeType::Output);
        g.Connect(v, o, static_cast<u8>(Channel::BaseColor));
        CompiledGraph cg = Compile(g);
        std::vector<u8> rgba1, rgba4;
        BakeGraphColor(cg, res, {0, 0}, {1, 1}, rgba1);
        BakeGraphColor(cg, res, {0, 0}, {4, 4}, rgba4);
        out.push_back(ColorToImage("tiling_voronoi_1m", res, std::move(rgba1)));
        out.push_back(ColorToImage("tiling_voronoi_4m", res, std::move(rgba4)));
    }

    // (B) BOX BRUSH weight field (rotated, soft falloff) over a 4m quad -> a soft rotated rectangle.
    {
        BoxBrush box;
        box.position = {2.0f, 0.0f, 2.0f};
        box.size = {2.4f, 1.0f, 1.4f};
        box.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0));
        box.falloff.type = FalloffType::Smoothstep;
        box.falloffWidth = 0.5f;
        MaskTexture mask;
        mask.Resize(res, res, 0.0f);
        BakeBoxMask(box, res, {0, 0}, {4, 4}, mask, false);
        out.push_back(MaskToImage("box_mask_rotated", mask));

        // (C) OVERLAPPING VOLUMES: a second box unioned in (max).
        BoxBrush box2 = box;
        box2.position = {2.8f, 0.0f, 2.6f};
        box2.rotation = glm::angleAxis(glm::radians(-20.0f), glm::vec3(0, 1, 0));
        BakeBoxMask(box2, res, {0, 0}, {4, 4}, mask, true);
        out.push_back(MaskToImage("box_overlap", mask));
    }

    // (D) PROCEDURAL MASKS: voronoi + value noise (both tiling in world space).
    {
        Graph gn;
        const u32 nz = gn.AddNode(NodeType::Noise);
        gn.FindNode(nz)->constant = glm::vec4(3.0f, 0.0f, 0.0f, 0.0f);
        gn.FindNode(nz)->space = Space::World;
        const u32 on = gn.AddNode(NodeType::Output);
        gn.Connect(nz, on, static_cast<u8>(Channel::Height));
        CompiledGraph cn = Compile(gn);
        MaskTexture nmask;
        BakeGraphChannel(cn, Channel::Height, res, nmask);
        out.push_back(MaskToImage("mask_noise", nmask));
    }

    // (E) PAINTED MASK: a few soft dabs (UV-space painting stand-in).
    MaskTexture paint;
    paint.Resize(res, res, 0.0f);
    StampDab(paint, {0.35f, 0.4f}, 0.18f, 0.9f);
    StampDab(paint, {0.5f, 0.55f}, 0.12f, 0.8f);
    StampDab(paint, {0.62f, 0.42f}, 0.09f, 1.0f);
    out.push_back(MaskToImage("paint_mask", paint));

    // (F) HEIGHT+NOISE vs LINEAR blend of two materials with the same gradient mask: shows the
    // height/noise transition breaking up vs the smooth linear one.
    {
        // Gradient mask: u from 0..1 across the quad.
        MaskTexture grad;
        grad.Resize(res, res, 0.0f);
        for (u32 y = 0; y < res; ++y)
            for (u32 x = 0; x < res; ++x) grad.Set(x, y, (x + 0.5f) / res);
        auto twoMat = [&](BlendMode mode, f32 noiseAmt) {
            LayerStack s;
            s.base.base_color = {0.72f, 0.70f, 0.66f, 1.0f}; // light concrete
            Layer l;
            l.surface.base_color = {0.16f, 0.11f, 0.06f, 1.0f}; // dark mud
            l.mask.kind = MaskKind::Procedural;
            l.mask.texture = grad;
            l.mask.textureSpace = Space::UV0;
            l.blend = mode;
            l.layerHeight = 0.85f;
            l.noiseAmount = noiseAmt;
            l.noiseScale = 64.0f; // finer break-up so the transition reads as texture, not blocks
            s.layers.push_back(l);
            return s;
        };
        BakedMaterial lin = BakeLayerStack(twoMat(BlendMode::Linear, 0.0f), res);
        BakedMaterial hn = BakeLayerStack(twoMat(BlendMode::HeightNoise, 0.6f), res);
        out.push_back(ColorToImage("blend_linear", res, std::move(lin.color)));
        out.push_back(ColorToImage("blend_height_noise", res, std::move(hn.color)));
    }

    // (G) COMPOSITE FLOOR (the hero): concrete base + mud(box mask) + moss(voronoi procedural,
    // height blend) + blood(paint mask), resolved to final color over a 4m quad. Uses all three
    // mask sources through the ONE resolver.
    {
        // Moss procedural mask (voronoi, world tiled).
        Graph gm;
        const u32 mv = gm.AddNode(NodeType::Voronoi);
        gm.FindNode(mv)->constant = glm::vec4(1.5f, 0.0f, 0.0f, 0.0f);
        gm.FindNode(mv)->space = Space::UV0;
        const u32 mo = gm.AddNode(NodeType::Output);
        gm.Connect(mv, mo, static_cast<u8>(Channel::Height));
        MaskTexture mossMask;
        BakeGraphChannel(Compile(gm), Channel::Height, res, mossMask);

        LayerStack s;
        s.base.base_color = {0.55f, 0.54f, 0.52f, 1.0f}; // concrete
        s.base.specular_roughness = 0.8f;

        Layer mud;
        mud.surface.base_color = {0.18f, 0.12f, 0.07f, 1.0f};
        mud.mask.kind = MaskKind::Box;
        mud.mask.box.position = {2.0f, 0.0f, 2.0f};
        mud.mask.box.size = {3.0f, 1.0f, 2.2f};
        mud.mask.box.rotation = glm::angleAxis(glm::radians(18.0f), glm::vec3(0, 1, 0));
        mud.mask.box.falloff.type = FalloffType::Smoothstep;
        mud.mask.box.falloffWidth = 0.6f;
        mud.blend = BlendMode::Height;
        mud.layerHeight = 0.4f;
        s.layers.push_back(mud);

        Layer moss;
        moss.surface.base_color = {0.10f, 0.28f, 0.09f, 1.0f};
        moss.mask.kind = MaskKind::Procedural;
        moss.mask.texture = mossMask;   // voronoi distance: moss settles in the crevices
        moss.blend = BlendMode::HeightNoise;
        moss.layerHeight = 0.7f;
        moss.opacity = 0.5f;            // partial coverage so concrete + mud still read
        moss.noiseAmount = 0.5f;
        moss.noiseScale = 60.0f;        // finer break-up (was blocky at 30)
        s.layers.push_back(moss);

        Layer blood;
        blood.surface.base_color = {0.32f, 0.02f, 0.02f, 1.0f};
        blood.surface.specular_roughness = 0.3f;
        MaskTexture bloodMask;
        bloodMask.Resize(res, res, 0.0f);
        StampDab(bloodMask, {0.6f, 0.35f}, 0.14f, 1.0f);
        StampDab(bloodMask, {0.68f, 0.45f}, 0.06f, 1.0f);
        blood.mask.kind = MaskKind::Paint;
        blood.mask.texture = bloodMask;
        blood.blend = BlendMode::Linear;
        s.layers.push_back(blood);

        BakedMaterial comp = BakeLayerStackWorld(s, res, {0, 0}, {4, 4});
        out.push_back(ColorToImage("composite_floor_color", res, std::move(comp.color)));
        BakedMaterial comp2 = BakeLayerStackWorld(s, res, {0, 0}, {4, 4});
        out.push_back(ColorToImage("composite_floor_material_MRH", res, std::move(comp2.material)));
    }

    return out;
}

std::vector<NamedImage> BakeGraphMaps(const CompiledGraph& compiled, u32 resolution,
                                      const TextureProvider& tex) {
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    const usize n = usize(resolution) * resolution * 4;
    NamedImage base, normal, rough, metal, height, ao, emis, opac;
    base.name = "basecolor"; normal.name = "normal"; rough.name = "roughness";
    metal.name = "metallic"; height.name = "height"; ao.name = "ao";
    emis.name = "emissive"; opac.name = "opacity";
    for (NamedImage* im : {&base, &normal, &rough, &metal, &height, &ao, &emis, &opac}) {
        im->width = im->height = resolution;
        im->rgba.assign(n, 255);
    }
    auto gray = [](NamedImage& im, usize o, f32 v) {
        const u8 b = ToU8(v);
        im.rgba[o + 0] = im.rgba[o + 1] = im.rgba[o + 2] = b;
        im.rgba[o + 3] = 255;
    };
    for (u32 y = 0; y < resolution; ++y)
        for (u32 x = 0; x < resolution; ++x) {
            SampleContext ctx;
            ctx.uv0 = {(x + 0.5f) / resolution, (y + 0.5f) / resolution};
            ctx.worldPos = {ctx.uv0.x, 0.0f, ctx.uv0.y};
            const SurfaceSample s = compiled.Eval(ctx, tex);
            const usize o = (usize(y) * resolution + x) * 4;
            base.rgba[o + 0] = ToU8(LinearToSrgb(s.baseColor.r));
            base.rgba[o + 1] = ToU8(LinearToSrgb(s.baseColor.g));
            base.rgba[o + 2] = ToU8(LinearToSrgb(s.baseColor.b));
            base.rgba[o + 3] = ToU8(s.opacity);
            const glm::vec3 en = EncodeNormal(s.normalTS);
            normal.rgba[o + 0] = ToU8(en.r);
            normal.rgba[o + 1] = ToU8(en.g);
            normal.rgba[o + 2] = ToU8(en.b);
            emis.rgba[o + 0] = ToU8(LinearToSrgb(s.emissive.r));
            emis.rgba[o + 1] = ToU8(LinearToSrgb(s.emissive.g));
            emis.rgba[o + 2] = ToU8(LinearToSrgb(s.emissive.b));
            gray(rough, o, s.roughness);
            gray(metal, o, s.metallic);
            gray(height, o, s.height);
            gray(ao, o, s.ao);
            gray(opac, o, s.opacity);
        }
    return {std::move(base),  std::move(normal), std::move(rough), std::move(metal),
            std::move(height), std::move(ao),     std::move(emis),  std::move(opac)};
}

BakedMaterial BakeMeshVolumes(const MeshData& mesh, const glm::mat4& worldTransform,
                              const LayerStack& stack, u32 resolution) {
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    BakedMaterial b;
    b.width = b.height = resolution;
    const usize n = usize(resolution) * resolution * 4;
    b.color.resize(n);
    b.material.resize(n);
    b.normal.resize(n);

    // Encode one resolved surface into the three buffers at byte offset o.
    auto writeTexel = [&](usize o, const ResolvedSurface& r) {
        const glm::vec3 c = glm::vec3(r.surface.base_color);
        b.color[o + 0] = ToU8(LinearToSrgb(c.r));
        b.color[o + 1] = ToU8(LinearToSrgb(c.g));
        b.color[o + 2] = ToU8(LinearToSrgb(c.b));
        b.color[o + 3] = ToU8(r.surface.base_color.a);
        b.material[o + 0] = ToU8(r.surface.base_metalness);
        b.material[o + 1] = ToU8(r.surface.specular_roughness);
        b.material[o + 2] = ToU8(r.height);
        b.material[o + 3] = 255;
        const glm::vec3 en = EncodeNormal(r.normalTS);
        b.normal[o + 0] = ToU8(en.r);
        b.normal[o + 1] = ToU8(en.g);
        b.normal[o + 2] = ToU8(en.b);
        b.normal[o + 3] = 255;
    };

    // Fill every texel with the BASE material first (texels no triangle covers keep this). Evaluating
    // the stack far from the origin makes every box volume weigh 0, so this is exactly the base.
    {
        SampleContext far;
        far.worldPos = glm::vec3(1e9f);
        const ResolvedSurface base = Resolve(stack, far);
        for (usize i = 0; i < n; i += 4) writeTexel(i, base);
    }
    RasterizeMeshUV(mesh, worldTransform, resolution,
                    [&](usize o, const SampleContext& ctx) { writeTexel(o, Resolve(stack, ctx)); });
    return b;
}

BakedMaterial BakeMeshVolumesOverlay(const MeshData& mesh, const glm::mat4& worldTransform,
                                     const LayerStack& stack, u32 resolution) {
    resolution = std::clamp<u32>(resolution, 1u, 8192u);
    BakedMaterial b;
    b.width = b.height = resolution;
    const usize n = usize(resolution) * resolution * 4;
    // Zero-initialised: every texel starts FULLY TRANSPARENT (color.a == material.a == 0). Texels no
    // triangle covers, and covered texels no volume reaches, stay transparent so the mesh's own
    // material shows through unchanged - this bakes a paint-canvas OVERLAY, not a replacement set.
    b.color.assign(n, 0);
    b.material.assign(n, 0);
    b.normal.assign(n, 0);
    if (stack.layers.empty()) return b;

    RasterizeMeshUV(mesh, worldTransform, resolution, [&](usize o, const SampleContext& ctx) {
        // Composite the volume layers (stack.base is intentionally ignored - the underlying mesh
        // material IS the base, supplied at render time by the paint blend). `cov` is the union
        // coverage under the standard `over` operator; the top volume wins the material by its
        // own weight, matching Resolve's per-layer LerpSurface so CPU/GPU/paint stay consistent.
        SurfaceParams surf{};
        glm::vec3 nrm(0.0f, 0.0f, 1.0f);
        f32 height = 0.5f;
        f32 cov = 0.0f;
        bool any = false;
        for (const Layer& L : stack.layers) {
            const f32 w = std::clamp(L.mask.Evaluate(ctx) * L.opacity, 0.0f, 1.0f);
            if (w <= 1e-4f) continue;
            if (!any) {
                surf = L.surface;
                nrm = L.normalTS;
                height = L.layerHeight;
                cov = w;
                any = true;
            } else {
                surf = LerpSurface(surf, L.surface, w);
                nrm = BlendNormalRNM(nrm, L.normalTS, w);
                height = glm::mix(height, L.layerHeight, w);
                cov = cov + w * (1.0f - cov);
            }
        }
        if (!any) return; // no volume reaches this texel -> leave it transparent
        const glm::vec3 c = glm::vec3(surf.base_color);
        b.color[o + 0] = ToU8(LinearToSrgb(c.r));
        b.color[o + 1] = ToU8(LinearToSrgb(c.g));
        b.color[o + 2] = ToU8(LinearToSrgb(c.b));
        b.color[o + 3] = ToU8(cov);
        b.material[o + 0] = ToU8(surf.base_metalness);
        b.material[o + 1] = ToU8(surf.specular_roughness);
        b.material[o + 2] = ToU8(height);
        b.material[o + 3] = ToU8(cov);
        const glm::vec3 en = EncodeNormal(nrm);
        b.normal[o + 0] = ToU8(en.r);
        b.normal[o + 1] = ToU8(en.g);
        b.normal[o + 2] = ToU8(en.b);
        b.normal[o + 3] = ToU8(cov);
    });
    return b;
}

u64 BakedMaterial::Hash() const {
    u64 h = 1469598103934665603ull;
    h = HashBytes(&width, sizeof(width), h);
    h = HashBytes(&height, sizeof(height), h);
    if (!color.empty()) h = HashBytes(color.data(), color.size(), h);
    if (!material.empty()) h = HashBytes(material.data(), material.size(), h);
    if (!normal.empty()) h = HashBytes(normal.data(), normal.size(), h);
    return h;
}

} // namespace hbe::mat

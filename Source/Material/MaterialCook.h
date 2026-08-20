// Material/MaterialCook.h - offline BAKE / cook of the authoring representation to runtime forms.
//
// This is the "Compiled/shipping representation" half of the request: the SOURCE (node graph +
// layer stack) is resolved OFFLINE into the engine's existing runtime forms so the runtime pays
// today's cost (docs/Design-MaterialAuthoring.md, Part B.2 "BAKED mode"):
//
//   * A constant-foldable graph          -> a plain hbe::MaterialAsset (.hbmat) + texture refs.
//   * A layer stack / procedural graph   -> baked channel textures (base color / MR / normal /
//                                            height) or a single-channel mask, produced by running
//                                            the CPU resolver over UV space. These feed the paint
//                                            canvas or become .uaf textures the normal path samples.
//
// Backend-agnostic and headless (no RHI / Renderer): produces in-memory pixel buffers. The editor
// integration writes them out as .uaf via the existing importer/BC path.
#pragma once

#include "Assets/MaterialAsset.h" // hbe::MaterialAsset (.hbmat runtime form)
#include "Assets/Mesh.h"          // hbe::MeshData (BakeMeshVolumes rasterizes a mesh into UV space)
#include "Core/Types.h"
#include "Material/MaterialCore.h"
#include "Material/MaterialGraph.h"
#include "Material/MaterialGraphCompiler.h"
#include "Material/MaterialLayer.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::mat {

// Baked channel textures for one surface (RGBA8, row-major). Channel packing mirrors the paint
// canvas so a bake can feed paint:: directly: `material` = (R metal, G rough, B height, A ao),
// `color` = (RGB base color sRGB, A opacity), `normal` = encoded tangent-space normal.
struct BakedMaterial {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> color;    // width*height*4
    std::vector<u8> material; // width*height*4
    std::vector<u8> normal;   // width*height*4
    bool Valid() const {
        const usize n = usize(width) * height * 4;
        return width > 0 && height > 0 && color.size() == n && material.size() == n && normal.size() == n;
    }
    u64 Hash() const;
};

// Resolve a layer stack over a WORLD-space quad into baked channel textures (BAKED mode). Each
// texel's uv0 is its normalized centre; worldPos is mapped from uv across [worldMin, worldMax] on
// the XZ plane (so world-space box masks + world tiling bake at their true physical size).
BakedMaterial BakeLayerStackWorld(const LayerStack& stack, u32 resolution, glm::vec2 worldMin,
                                  glm::vec2 worldMax, const TextureProvider& tex = {});
// Convenience: bake over the UV unit square (a 1x1 metre quad).
BakedMaterial BakeLayerStack(const LayerStack& stack, u32 resolution, const TextureProvider& tex = {});

// Bake a compiled graph's BaseColor+Opacity over a world quad into an RGBA8 (sRGB) image. Used by
// the visual test scene to show WORLD-space tiling at true physical size (no UV stretch).
void BakeGraphColor(const CompiledGraph& compiled, u32 resolution, glm::vec2 worldMin,
                    glm::vec2 worldMax, std::vector<u8>& rgba, const TextureProvider& tex = {});

// One named RGBA8 image produced by the visual test scene.
struct NamedImage {
    std::string name;
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; // width*height*4
};

// Build the VISUAL TEST SCENE as a set of baked images (the request's "large tiled floor / wall /
// box brush / overlapping volumes / painted masks / procedural masks / height blending / different
// tiling scales"), so stretching / blending / painting correctness is visible without a live GPU.
// The editor's --matscene writes these to PNGs via movie::WritePng.
std::vector<NamedImage> BuildDemoScene(u32 resolution = 256);

// Bake a compiled material graph to the full PBR texture SET (one Eval per texel, all 8 channels):
// base_color (sRGB, A=opacity), normal (encoded), roughness, metallic, height, ao, emissive (sRGB),
// opacity. This is the Material-Maker-style "export textures" output; the editor and --matexport
// write each NamedImage to a PNG. Deterministic. `tex` samples any Texture-node references.
std::vector<NamedImage> BakeGraphMaps(const CompiledGraph& compiled, u32 resolution,
                                      const TextureProvider& tex = {});

// Bake material VOLUMES onto a mesh (the box-brush world tool's BAKED application). Rasterizes the
// mesh into UV space; at each texel a triangle covers, evaluates the base material + the box-masked
// layers of `stack` at that texel's WORLD position (worldTransform * interpolated local position),
// writing the resolved base color / material (metal/rough/height) / normal. The box masks
// (MaskKind::Box) are evaluated per texel via their world-space weight field, so a box volume paints
// exactly the region it encloses, with its falloff. Texels no triangle covers keep the base material.
// Headless + deterministic; the result becomes the mesh's material texture set.
BakedMaterial BakeMeshVolumes(const MeshData& mesh, const glm::mat4& worldTransform,
                              const LayerStack& stack, u32 resolution);

// Bake material VOLUMES onto a mesh as a paint-canvas OVERLAY (the box-brush world tool's editor
// application). Same UV rasterization as BakeMeshVolumes, but the output is TRANSPARENT wherever no
// volume reaches: `stack.base` is ignored and each texel's alpha is the union coverage of the box
// layers (standard `over`), so the result drops straight into a PaintLayer (color = RGBA sRGB+cov,
// material = metal/rough/height + cov) and composites over the mesh's existing material at render
// time - non-destructive and removable. Reuses the tested Resolve machinery (LerpSurface / RNM).
BakedMaterial BakeMeshVolumesOverlay(const MeshData& mesh, const glm::mat4& worldTransform,
                                     const LayerStack& stack, u32 resolution);

// Bake one scalar Output channel of a compiled graph into a single-channel mask (procedural mask).
void BakeGraphChannel(const CompiledGraph& compiled, Channel channel, u32 resolution,
                      MaskTexture& out, const TextureProvider& tex = {});

// Cook a node graph to the runtime .hbmat form: compile, fold the constant value channels into
// SurfaceParams, and lift any Texture/NormalMap node feeding a channel into the matching .hbmat
// texture slot (base color -> albedoTex, Normal -> normalTex, ...). This is the constant-material
// cook path; procedural detail is handled by the BakeLayerStack/BakeGraphChannel texture bakes.
MaterialAsset CompileGraphToMaterialAsset(const Graph& g,
                                          const std::vector<ParamOverride>& overrides = {});

} // namespace hbe::mat

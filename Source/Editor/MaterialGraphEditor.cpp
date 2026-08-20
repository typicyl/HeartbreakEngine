// Editor/MaterialGraphEditor.cpp - the Material Maker: a node-graph editor for .hbmatgraph.
//
// Same Blueprints-style canvas as the Schematic / Dialogue editors, but authors a mat::Graph
// (Source/Material) which COMPILES to the engine's one runtime material (hbe::SurfaceParams ->
// OpenPBR). Unlike those exec-flow graphs, this is DATA flow: a node has N input pins on the LEFT
// and one output pin on the RIGHT; the single Output node exposes the 8 surface-channel input pins
// (Base Color / Roughness / Metallic / Normal / Height / AO / Emissive / Opacity). A live preview
// compiles the graph and bakes a swatch through the real resolver so the author sees the result.
//
// Kept in its own translation unit (like DialogueEditor.cpp) so Editor.cpp does not grow further.
#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Editor/MovieRender.h"            // movie::WritePng for the texture export
#include "Editor/RuntimeShaderCompiler.h"  // interactive GPU preview: compile the graph shader
#include "Engine/Engine.h"                 // engine.GetRenderer() for the preview upload
#include "Material/MaterialCook.h"
#include "Material/MaterialGraph.h"
#include "Material/MaterialGraphCompiler.h"
#include "Material/MaterialGraphHlsl.h"    // graph -> compute HLSL for the GPU preview
#include "Project/Project.h"
#include "RHI/RHI.h"
#include "Renderer/Renderer.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hbe {

namespace {
using mat::Channel;
using mat::NodeType;

// Title-bar colour per node CATEGORY (from the catalog), so the graph reads at a glance.
ImU32 MgNodeColor(NodeType t) {
    const char* cat = mat::NodeInfoOf(t).category;
    if (std::strcmp(cat, "Input") == 0) return IM_COL32(58, 70, 104, 255);
    if (std::strcmp(cat, "Math") == 0) return IM_COL32(60, 100, 66, 255);
    if (std::strcmp(cat, "Procedural") == 0) return IM_COL32(104, 78, 52, 255);
    if (std::strcmp(cat, "Coordinate") == 0) return IM_COL32(52, 92, 90, 255);
    if (std::strcmp(cat, "Layer") == 0) return IM_COL32(92, 62, 112, 255);
    if (std::strcmp(cat, "Output") == 0) return IM_COL32(120, 72, 58, 255);
    return IM_COL32(72, 72, 84, 255);
}

const char* MgChannelName(Channel c) {
    switch (c) {
        case Channel::BaseColor: return "Base Color";
        case Channel::Roughness: return "Roughness";
        case Channel::Metallic:  return "Metallic";
        case Channel::Normal:    return "Normal";
        case Channel::Height:    return "Height";
        case Channel::AO:        return "AO";
        case Channel::Emissive:  return "Emissive";
        case Channel::Opacity:   return "Opacity";
        default:                 return "?";
    }
}

// Label for input pin `pin` of a node (Output uses channel names; math uses A/B/T).
std::string MgInputLabel(NodeType t, u8 pin) {
    if (t == NodeType::Output) return MgChannelName(static_cast<Channel>(pin));
    switch (t) {
        case NodeType::Lerp:     return pin == 0 ? "A" : (pin == 1 ? "B" : "T");
        case NodeType::Multiply:
        case NodeType::Add:
        case NodeType::Subtract:
        case NodeType::Divide:   return pin == 0 ? "A" : "B";
        default:                 return "In";
    }
}

u8 MgInputCount(NodeType t) { return mat::NodeInfoOf(t).inputCount; }
bool MgHasOutput(NodeType t) { return t != NodeType::Output; }

// A short body preview line so a node reads at a glance without opening the inspector.
std::string MgNodePreview(const mat::Node& n) {
    char buf[64];
    switch (n.type) {
        case NodeType::Float:
            std::snprintf(buf, sizeof(buf), "%.3g", n.constant.x);
            return buf;
        case NodeType::Color:
        case NodeType::Vector:
        case NodeType::Constant:
            std::snprintf(buf, sizeof(buf), "%.2g,%.2g,%.2g", n.constant.x, n.constant.y, n.constant.z);
            return buf;
        case NodeType::Texture:
        case NodeType::NormalMap:
        case NodeType::Height:
            return n.texture.empty() ? std::string("(no texture)")
                                     : std::filesystem::path(n.texture).filename().string();
        case NodeType::Noise:
        case NodeType::Voronoi:
            std::snprintf(buf, sizeof(buf), "scale %.2g", n.constant.x);
            return buf;
        case NodeType::Power:
            std::snprintf(buf, sizeof(buf), "^ %.2g", n.constant.x);
            return buf;
        default:
            return {};
    }
}

// ---- Interactive GPU preview (compiles the graph to a shader at runtime + runs it) -------
// One editor instance, so file-local state is fine (mirrors the asset-viewer's static preview id).
struct MgGpuState {
    bool enabled = true;
    bool availChecked = false, avail = false;
    u64 key = 0, pendingKey = 0;
    int pendingFrames = 0, frame = 0, dispatchFrame = -1;
    rhi::ComputePipelineHandle pipe;
    rhi::GpuBufferHandle buf;   // res*res*8 float4 output (8 channels per pixel)
    rhi::TextureHandle tex;     // res*res RGBA8 display texture
    u64 texUiId = 0;
    bool haveResult = false, compileFailed = false;
    std::string error;
    u32 res = 256;
};
MgGpuState g_gpu;

u8 SrgbU8(f32 c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(s * 255.0f + 0.5f);
}

// Debounced GPU preview of `pg` (whose Output.BaseColor is shown) at content key `key`. Returns the
// display texture's ImGui id, or 0 if no GPU result yet (caller falls back to the CPU bake). Never
// throws; any failure sets compileFailed/error and returns 0. See --test-runtimegpu for the same
// compile->pipeline->dispatch->readback path verified on hardware.
u64 RunGpuPreview(Renderer& r, const mat::Graph& pg, u64 key) {
    MgGpuState& g = g_gpu;
    ++g.frame;
    const rhi::GraphicsAPI api = r.API();
    if (!g.availChecked) {
        g.avail = r.SupportsGpuCompute() && editor::RuntimeShaderCompiler::Available(api);
        g.availChecked = true;
    }
    if (!g.enabled || !g.avail) return 0;

    constexpr int kDebounce = 12; // recompile only after the graph has been stable ~12 frames
    if (key != g.key) {
        if (key != g.pendingKey) { g.pendingKey = key; g.pendingFrames = 0; }
        else if (++g.pendingFrames >= kDebounce) {
            const std::string hlsl = mat::GenerateComputeHlsl(pg);
            const auto cr = editor::RuntimeShaderCompiler::Compile(api, hlsl, "CSMain", "cs", "MatGraphPreview");
            g.key = key;
            g.pendingKey = 0;
            g.pendingFrames = 0;
            if (!cr.ok) {
                g.compileFailed = true;
                g.error = cr.log;
                return g.haveResult ? g.texUiId : 0;
            }
            g.compileFailed = false;
            if (g.pipe.IsValid()) r.DestroyComputePipeline(g.pipe); // free the previous kernel; no leak
            rhi::ComputePipelineDesc pd{};
            pd.shaderName = "MatGraphPreview";
            pd.entryPoint = "CSMain";
            pd.constantBytes = 16;
            pd.uavCount = 1;
            g.pipe = r.CreateComputePipeline(pd);
            if (!g.buf.IsValid()) {
                rhi::GpuBufferDesc bd{};
                bd.elementCount = g.res * g.res * 8;
                bd.elementStride = sizeof(glm::vec4);
                bd.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::ShaderRead;
                bd.debugName = "MatGraphPreviewBuf";
                g.buf = r.CreateGpuBuffer(bd);
            }
            if (g.pipe.IsValid() && g.buf.IsValid()) {
                struct CB { u32 res, p0, p1, p2; } cb{g.res, 0, 0, 0};
                rhi::ComputeDispatch d{};
                d.pipeline = g.pipe;
                d.constants = &cb;
                d.constantBytes = sizeof(cb);
                d.uavs[0] = g.buf;
                d.uavCount = 1;
                d.groupsX = (g.res + 7) / 8;
                d.groupsY = (g.res + 7) / 8;
                r.QueueCompute(d); // drains at the next BeginFrame (before RenderScene)
                g.dispatchFrame = g.frame;
            }
        }
    }

    // A couple frames after dispatch, read back and upload the base-colour channel for display.
    if (g.dispatchFrame >= 0 && g.frame - g.dispatchFrame >= 2 && g.buf.IsValid()) {
        std::vector<glm::vec4> data(static_cast<usize>(g.res) * g.res * 8);
        if (r.ReadGpuBuffer(g.buf, data.data(), static_cast<u32>(data.size() * sizeof(glm::vec4)))) {
            std::vector<u32> px(static_cast<usize>(g.res) * g.res, 0u);
            for (usize p = 0; p < px.size(); ++p) {
                const glm::vec4 base = data[p * 8 + 0];
                px[p] = SrgbU8(base.r) | (static_cast<u32>(SrgbU8(base.g)) << 8) |
                        (static_cast<u32>(SrgbU8(base.b)) << 16) | (255u << 24);
            }
            rhi::TextureDesc td{};
            td.width = g.res;
            td.height = g.res;
            td.format = rhi::Format::R8G8B8A8_UNORM;
            td.pixels = px.data();
            td.debugName = "MatGraphGpuPreview";
            if (!g.tex.IsValid()) {
                g.tex = r.UploadTexture(td);
                g.texUiId = r.TextureUIId(g.tex);
            } else {
                r.UpdateTexture(g.tex, td);
            }
            g.haveResult = true;
        }
        g.dispatchFrame = -1;
    }
    return g.haveResult ? g.texUiId : 0;
}
} // namespace

std::filesystem::path Editor::CreateMaterialGraphAsset(const std::filesystem::path& dirIn,
                                                       const std::string& name) {
    if (dirIn.empty() && !Project::HasActive()) return {};
    const std::filesystem::path dir =
        dirIn.empty() ? Project::Active().AssetsDir() / "Materials" : dirIn;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string base = name.empty() ? "NewMaterial" : name;
    std::string stem = base;
    for (int i = 1; std::filesystem::exists(dir / (stem + mat::kMaterialGraphExtension), ec); ++i)
        stem = base + std::to_string(i);
    const std::filesystem::path p = dir / (stem + mat::kMaterialGraphExtension);
    // A fresh material: a neutral grey base colour + 0.5 roughness wired to the Output.
    mat::Graph g;
    g.name = stem;
    const u32 col = g.AddNode(NodeType::Color, {60.0f, 60.0f});
    g.FindNode(col)->constant = {0.8f, 0.8f, 0.8f, 1.0f};
    const u32 rough = g.AddNode(NodeType::Float, {60.0f, 200.0f});
    g.FindNode(rough)->constant = {0.5f, 0.0f, 0.0f, 0.0f};
    const u32 out = g.AddNode(NodeType::Output, {360.0f, 90.0f});
    g.Connect(col, out, static_cast<u8>(Channel::BaseColor));
    g.Connect(rough, out, static_cast<u8>(Channel::Roughness));
    return mat::SaveGraph(p, g) ? p : std::filesystem::path{};
}

void Editor::OpenMaterialGraph(const std::filesystem::path& path) {
    auto loaded = mat::LoadGraph(path);
    mat::Graph g;
    if (loaded && !loaded->nodes.empty()) {
        g = std::move(*loaded);
    } else {
        // Fresh / unreadable: seed with a Color -> Output.BaseColor so there's something to wire.
        g = mat::Graph{};
        const u32 col = g.AddNode(NodeType::Color, {60.0f, 80.0f});
        g.FindNode(col)->constant = {0.8f, 0.8f, 0.8f, 1.0f};
        const u32 out = g.AddNode(NodeType::Output, {360.0f, 80.0f});
        g.Connect(col, out, static_cast<u8>(Channel::BaseColor));
    }
    mgGraph_ = std::move(g);
    mgHistory_.Clear(); // a different document must not let Ctrl+Z paste the previous file's graph
    mgPath_ = path;
    mgDirty_ = false;
    mgFocus_ = true;
    mgPan_ = glm::vec2(0.0f);
    mgSelected_ = 0;
    mgDragging_ = false;
    mgPreviewHash_ = 0; // force a preview rebake
    panelOpen_[Panel_MaterialGraph] = true;
}

bool Editor::SaveMaterialGraph() {
    if (mgPath_.empty()) return false;
    if (!mat::SaveGraph(mgPath_, mgGraph_)) {
        HBE_ERROR("Failed to write material graph '{}'.", mgPath_.string());
        return false;
    }
    StampNewAsset(mgPath_); // the save rebuilt the JSON; restore its pack slot
    mgDirty_ = false;
    assetsDirty_ = true;
    return true;
}

void Editor::DrawMaterialGraph(Engine& engine) {
    if (!panelOpen_[Panel_MaterialGraph]) return;
    if (mgFocus_) {
        ImGui::SetNextWindowFocus();
        mgFocus_ = false;
    }
    const bool visible = ImGui::Begin("Material Graph", &panelOpen_[Panel_MaterialGraph]);
    // Claim Ctrl+S for THIS panel unconditionally, above the early returns - an empty Material
    // Graph must say "nothing open to save" instead of quietly writing the LEVEL (see the long
    // note in DialogueEditor.cpp for why this is above both early returns).
    ClaimFocus(editor::SaveSurface::MaterialGraph);
    if (!visible) {
        ImGui::End();
        return;
    }

    const auto mgSnap = [&] { mgHistory_.Push(mgGraph_); };
    const auto mgSnapOnActivate = [&] {
        if (ImGui::IsItemActivated()) mgSnap();
    };

    // Toolbar: New / Open / Save + open file name.
    if (ImGui::Button("New")) {
        const std::filesystem::path p = CreateMaterialGraphAsset();
        if (!p.empty()) OpenMaterialGraph(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) ImGui::OpenPopup("##mgopen");
    ImGui::SameLine();
    ImGui::BeginDisabled(mgPath_.empty());
    if (ImGui::Button(mgDirty_ ? "Save*" : "Save")) {
        if (SaveMaterialGraph())
            SetSaveStatus("Saved material graph '" + mgPath_.filename().string() + "'.", false);
        else
            SetSaveStatus("MATERIAL GRAPH SAVE FAILED - '" + mgPath_.filename().string() +
                              "' was NOT written.",
                          true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (mgPath_.empty()) {
        ImGui::TextDisabled("(no material open)");
    } else {
        ImGui::Text("%s", mgPath_.filename().string().c_str());
        if (mgDirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved - Ctrl+S)");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(mgPath_.empty());
    if (ImGui::Button("Export Textures")) ImGui::OpenPopup("##mgexport");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##mgexport")) {
        static int resIdx = 1;
        const int resVals[] = {256, 512, 1024, 2048};
        ImGui::TextUnformatted("Bake the full PBR texture set (basecolor/normal/roughness/");
        ImGui::TextUnformatted("metallic/height/ao/emissive/opacity):");
        ImGui::Combo("Resolution", &resIdx, "256\0" "512\0" "1024\0" "2048\0");
        if (ImGui::Button("Export")) {
            const mat::CompiledGraph c = mat::Compile(mgGraph_);
            if (c.ok) {
                const std::filesystem::path dir =
                    mgPath_.parent_path() / (mgPath_.stem().string() + "_textures");
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const auto maps = mat::BakeGraphMaps(c, static_cast<u32>(resVals[resIdx]));
                int ok = 0;
                for (const auto& m : maps)
                    if (movie::WritePng(dir / (m.name + ".png"), m.width, m.height, m.rgba)) ++ok;
                SetSaveStatus("Exported " + std::to_string(ok) + " maps (" +
                                  std::to_string(resVals[resIdx]) + "px) to " +
                                  dir.filename().string() + "/",
                              ok != static_cast<int>(maps.size()));
                assetsDirty_ = true;
            } else {
                SetSaveStatus("Export failed: " + c.error, true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##mgopen")) {
        bool any = false;
        for (const std::string& rel : ListAssetsByExt(mat::kMaterialGraphExtension)) {
            any = true;
            if (ImGui::Selectable(rel.c_str())) {
                OpenMaterialGraph(Project::Active().AssetsDir() / rel);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!any) ImGui::TextDisabled("No .hbmatgraph assets yet - use New.");
        ImGui::EndPopup();
    }

    if (mgPath_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Create or open a material, then build a node graph: pull constants / textures / noise "
            "into Math nodes and wire the result into the Output node's channels (Base Color, "
            "Roughness, Metallic, Normal, Height, AO, Emissive, Opacity). It compiles to the "
            "engine's OpenPBR material; a constant graph folds to plain values, a procedural one "
            "bakes to textures at cook time.");
        ImGui::TextDisabled("Right-click the canvas to add a node. Drag from a pin to wire.");
        ImGui::End();
        return;
    }

    ImGui::Separator();
    const float kInspW = 340.0f;
    float canvasW = ImGui::GetContentRegionAvail().x - kInspW - 8.0f;
    if (canvasW < 140.0f) canvasW = ImGui::GetContentRegionAvail().x;
    DrawMaterialGraphCanvas(engine, canvasW);
    ImGui::SameLine();

    // --- Right column: live preview + node inspector + parameters --------------------------
    ImGui::BeginChild("##mginsp", ImVec2(0, 0), ImGuiChildFlags_Borders);

    // Live preview (Material-Maker style): shows the SELECTED node's output, or the final material.
    // When the editor runtime shader compiler + GPU compute are available, the graph is compiled to
    // a compute shader and evaluated ON THE GPU (interactive, 256px); otherwise / until the first GPU
    // result lands it is CPU-baked (112px). Toggle with the "GPU" checkbox.
    {
        const mat::Node* selNode = mgSelected_ ? mgGraph_.FindNode(mgSelected_) : nullptr;
        const bool nodePreview = selNode && selNode->type != NodeType::Output;
        // The graph whose Output.BaseColor is previewed: the whole graph, or a copy with a fresh
        // Output wired to the selected node (so ANY node can be previewed).
        mat::Graph tmp;
        const mat::Graph* pg = &mgGraph_;
        if (nodePreview) {
            tmp = mgGraph_;
            tmp.nodes.erase(std::remove_if(tmp.nodes.begin(), tmp.nodes.end(),
                                           [](const mat::Node& nn) { return nn.type == NodeType::Output; }),
                            tmp.nodes.end());
            tmp.links.erase(std::remove_if(tmp.links.begin(), tmp.links.end(),
                                           [&](const mat::Link& l) {
                                               return !tmp.FindNode(l.fromNode) || !tmp.FindNode(l.toNode);
                                           }),
                            tmp.links.end());
            const u32 out = tmp.AddNode(NodeType::Output);
            tmp.Connect(mgSelected_, out, static_cast<u8>(Channel::BaseColor));
            pg = &tmp;
        }
        const mat::CompiledGraph c = mat::Compile(*pg);
        if (!c.ok) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Compile error: %s", c.error.c_str());
        } else {
            const u64 key = c.Hash() ^ (static_cast<u64>(mgSelected_) * 0x9E3779B97F4A7C15ull) ^
                            (nodePreview ? 0x1ull : 0x2ull);
            const u64 gpuId = RunGpuPreview(engine.GetRenderer(), *pg, key);
            if (gpuId != 0) {
                ImGui::Image(static_cast<ImTextureID>(gpuId), ImVec2(180, 180)); // GPU-evaluated
            } else {
                if (key != mgPreviewHash_) { // CPU fallback bake, gated by content hash
                    std::vector<u8> rgba;
                    mat::BakeGraphColor(c, 112, {0.0f, 0.0f}, {1.0f, 1.0f}, rgba);
                    std::vector<u32> px(112u * 112u, 0u);
                    for (usize i = 0; i < px.size(); ++i)
                        px[i] = static_cast<u32>(rgba[i * 4 + 0]) | (static_cast<u32>(rgba[i * 4 + 1]) << 8) |
                                (static_cast<u32>(rgba[i * 4 + 2]) << 16) | (static_cast<u32>(rgba[i * 4 + 3]) << 24);
                    Renderer& rr = engine.GetRenderer();
                    rhi::TextureDesc d;
                    d.width = 112;
                    d.height = 112;
                    d.format = rhi::Format::R8G8B8A8_UNORM;
                    d.pixels = px.data();
                    d.debugName = "matgraph_preview_cpu";
                    mgPreviewId_ = rr.TextureUIId(rr.UploadTexture(d));
                    mgPreviewHash_ = key;
                }
                if (mgPreviewId_ != 0) ImGui::Image(static_cast<ImTextureID>(mgPreviewId_), ImVec2(112, 112));
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(nodePreview ? mat::NodeInfoOf(selNode->type).name : "Final material");
            if (!nodePreview) {
                const SurfaceParams sp = c.ToSurfaceParams();
                ImGui::ColorButton("##mgbase",
                                   ImVec4(sp.base_color.r, sp.base_color.g, sp.base_color.b, 1.0f),
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
                ImGui::SameLine();
                ImGui::Text("Rough %.2f Metal %.2f", sp.specular_roughness, sp.base_metalness);
            }
            if (g_gpu.availChecked && g_gpu.avail) {
                ImGui::Checkbox("GPU", &g_gpu.enabled);
                ImGui::SameLine();
                ImGui::TextDisabled(gpuId ? "GPU-evaluated" : (g_gpu.enabled ? "compiling shader..." : "CPU"));
            } else {
                ImGui::TextDisabled(c.fullyConstant ? "constant (folds to .hbmat)"
                                                    : "procedural (bakes to textures)");
            }
            if (g_gpu.compileFailed)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "GPU shader compile error (see log)");
            ImGui::EndGroup();
        }
    }
    ImGui::Separator();

    // Node inspector for the selected node.
    mat::Node* n = mgSelected_ ? mgGraph_.FindNode(mgSelected_) : nullptr;
    if (!n) {
        ImGui::TextDisabled("Select a node to edit it.");
    } else {
        ImGui::TextUnformatted(mat::NodeInfoOf(n->type).name);
        ImGui::Separator();
        // Compact param helpers (DragFloat + dirty + undo-on-grab; combo maps an int onto a float).
        auto pf = [&](const char* label, f32* v, f32 speed, f32 mn, f32 mx) {
            const bool changed = (mn < mx) ? ImGui::DragFloat(label, v, speed, mn, mx)
                                           : ImGui::DragFloat(label, v, speed);
            if (changed) mgDirty_ = true;
            mgSnapOnActivate();
        };
        auto pcombo = [&](const char* label, f32* v, const char* items) {
            int iv = static_cast<int>(*v);
            if (ImGui::Combo(label, &iv, items)) {
                mgSnap();
                *v = static_cast<f32>(iv);
                mgDirty_ = true;
            }
        };
        switch (n->type) {
            case NodeType::Float:
                if (ImGui::DragFloat("Value", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Color:
                if (ImGui::ColorEdit4("Color", &n->constant.x)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Vector:
            case NodeType::Constant:
                if (ImGui::DragFloat4("Value", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Texture:
            case NodeType::NormalMap:
            case NodeType::Height: {
                std::string picked;
                if (AssetPicker("Texture", n->texture, ".uaf", uaf::AssetType::Texture, picked)) {
                    mgSnap();
                    n->texture = picked;
                    mgDirty_ = true;
                }
                break;
            }
            case NodeType::Noise:
            case NodeType::Voronoi:
                if (ImGui::DragFloat("Scale", &n->constant.x, 0.05f, 0.01f, 256.0f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("Seed", &n->constant.y, 1.0f, 0.0f, 9999.0f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Clamp:
                if (ImGui::DragFloat("Min", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("Max", &n->constant.y, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Remap:
                if (ImGui::DragFloat("In Min", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("In Max", &n->constant.y, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("Out Min", &n->constant.z, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("Out Max", &n->constant.w, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Power:
                if (ImGui::DragFloat("Exponent", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Smoothstep:
                if (ImGui::DragFloat("Edge 0", &n->constant.x, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                if (ImGui::DragFloat("Edge 1", &n->constant.y, 0.01f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::Lerp:
                ImGui::TextDisabled("T pin unconnected uses this constant:");
                if (ImGui::SliderFloat("T", &n->constant.x, 0.0f, 1.0f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            case NodeType::ColorRamp: {
                ImGui::TextDisabled("Gradient stops (position -> colour):");
                int removeIdx = -1;
                for (usize i = 0; i < n->ramp.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::DragFloat("Pos", &n->ramp[i].pos, 0.01f, 0.0f, 1.0f)) mgDirty_ = true;
                    mgSnapOnActivate();
                    if (ImGui::ColorEdit4("##c", &n->ramp[i].color.x,
                                          ImGuiColorEditFlags_NoInputs)) mgDirty_ = true;
                    mgSnapOnActivate();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removeIdx = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (removeIdx >= 0) {
                    mgSnap();
                    n->ramp.erase(n->ramp.begin() + removeIdx);
                    mgDirty_ = true;
                }
                if (ImGui::SmallButton("+ Add stop")) {
                    mgSnap();
                    n->ramp.push_back({n->ramp.empty() ? 0.0f : 1.0f, glm::vec4(1.0f)});
                    mgDirty_ = true;
                }
                break;
            }
            case NodeType::Mask:
            case NodeType::MaterialLayer: {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", n->paramName.c_str());
                if (ImGui::InputText("Name", buf, sizeof(buf))) {
                    n->paramName = buf;
                    mgDirty_ = true;
                }
                mgSnapOnActivate();
                if (n->type == NodeType::Mask)
                    if (ImGui::DragFloat("Default", &n->constant.x, 0.01f, 0.0f, 1.0f)) mgDirty_ = true;
                mgSnapOnActivate();
                break;
            }
            // === Material-Maker library nodes ===
            case NodeType::Perlin:
                pf("Scale", &n->constant.x, 0.1f, 0.1f, 256.0f);
                pf("Seed", &n->constant.y, 1.0f, 0.0f, 9999.0f);
                break;
            case NodeType::FractalNoise:
                pf("Scale", &n->constant.x, 0.1f, 0.1f, 256.0f);
                pf("Octaves", &n->constant.y, 1.0f, 1.0f, 8.0f);
                pf("Persistence", &n->constant.z, 0.01f, 0.0f, 1.0f);
                pf("Seed", &n->constant.w, 1.0f, 0.0f, 9999.0f);
                break;
            case NodeType::Cellular:
                pf("Scale", &n->constant.x, 0.1f, 0.1f, 256.0f);
                pf("Seed", &n->constant.y, 1.0f, 0.0f, 9999.0f);
                pcombo("Mode", &n->constant.z, "F1\0F2\0F2-F1\0Cell\0");
                break;
            case NodeType::Checker:
                pf("Tiles", &n->constant.x, 0.1f, 1.0f, 64.0f);
                break;
            case NodeType::Bricks:
                pf("Columns", &n->constant.x, 0.1f, 1.0f, 32.0f);
                pf("Rows", &n->constant.y, 0.1f, 1.0f, 32.0f);
                pf("Mortar", &n->constant.z, 0.005f, 0.0f, 0.4f);
                pf("Row offset", &n->constant.w, 0.02f, 0.0f, 1.0f);
                break;
            case NodeType::Grid:
                pf("Tiles", &n->constant.x, 0.1f, 1.0f, 64.0f);
                pf("Line width", &n->constant.y, 0.005f, 0.0f, 0.5f);
                break;
            case NodeType::Shape:
                pf("Sides (0=circle)", &n->constant.x, 1.0f, 0.0f, 12.0f);
                pf("Radius", &n->constant.y, 0.01f, 0.0f, 0.6f);
                pf("AA", &n->constant.z, 0.002f, 0.0f, 0.2f);
                break;
            case NodeType::Wave:
                pf("Frequency", &n->constant.x, 0.1f, 0.0f, 64.0f);
                pcombo("Type", &n->constant.y, "Sine\0Triangle\0Saw\0Square\0");
                pf("Phase", &n->constant.z, 0.01f, 0.0f, 0.0f);
                break;
            case NodeType::Dots:
                pf("Tiles", &n->constant.x, 0.1f, 1.0f, 64.0f);
                pf("Radius", &n->constant.y, 0.01f, 0.0f, 0.5f);
                break;
            case NodeType::RadialGradient:
                pf("Radius", &n->constant.x, 0.01f, 0.01f, 2.0f);
                break;
            case NodeType::Transform:
                pf("Translate X", &n->constant.x, 0.01f, 0.0f, 0.0f);
                pf("Translate Y", &n->constant.y, 0.01f, 0.0f, 0.0f);
                pf("Rotation (turns)", &n->constant.z, 0.01f, 0.0f, 0.0f);
                pf("Scale", &n->constant.w, 0.01f, 0.01f, 16.0f);
                break;
            case NodeType::Tile:
                pf("Tiles X", &n->constant.x, 0.1f, 1.0f, 64.0f);
                pf("Tiles Y", &n->constant.y, 0.1f, 1.0f, 64.0f);
                break;
            case NodeType::Mirror:
                pcombo("Axis", &n->constant.x, "X\0Y\0Both\0");
                break;
            case NodeType::Warp:
                pf("Amount", &n->constant.x, 0.005f, 0.0f, 1.0f);
                break;
            case NodeType::Kaleidoscope:
                pf("Count", &n->constant.x, 1.0f, 1.0f, 32.0f);
                break;
            case NodeType::Blend:
                pcombo("Mode", &n->constant.x,
                       "Normal\0Multiply\0Screen\0Overlay\0Darken\0Lighten\0Difference\0Add\0"
                       "Subtract\0Dodge\0Burn\0Soft Light\0Hard Light\0");
                pf("Opacity", &n->constant.y, 0.01f, 0.0f, 1.0f);
                break;
            case NodeType::HSV:
                pf("Hue shift", &n->constant.x, 0.005f, 0.0f, 1.0f);
                pf("Saturation", &n->constant.y, 0.01f, 0.0f, 4.0f);
                pf("Value", &n->constant.z, 0.01f, 0.0f, 4.0f);
                break;
            case NodeType::BrightnessContrast:
                pf("Brightness", &n->constant.x, 0.01f, -1.0f, 1.0f);
                pf("Contrast", &n->constant.y, 0.01f, 0.0f, 4.0f);
                break;
            case NodeType::Levels:
                pf("In low", &n->constant.x, 0.01f, 0.0f, 1.0f);
                pf("In high", &n->constant.y, 0.01f, 0.0f, 1.0f);
                pf("Out low", &n->constant.z, 0.01f, 0.0f, 1.0f);
                pf("Out high", &n->constant.w, 0.01f, 0.0f, 1.0f);
                break;
            case NodeType::Gamma:
                pf("Gamma", &n->constant.x, 0.01f, 0.01f, 8.0f);
                break;
            case NodeType::Posterize:
                pf("Levels", &n->constant.x, 0.25f, 2.0f, 32.0f);
                break;
            case NodeType::Threshold:
                pf("Threshold", &n->constant.x, 0.01f, 0.0f, 1.0f);
                break;
            case NodeType::Swizzle:
                pcombo("Channel", &n->constant.x, "R\0G\0B\0A\0Grayscale\0");
                break;
            case NodeType::HeightToNormal:
                pf("Strength", &n->constant.x, 0.02f, 0.0f, 16.0f);
                pf("Epsilon", &n->constant.y, 0.001f, 0.0f, 0.1f);
                break;
            case NodeType::AmbientOcclusion:
                pf("Strength", &n->constant.x, 0.02f, 0.0f, 8.0f);
                pf("Radius", &n->constant.y, 0.002f, 0.0f, 0.2f);
                break;
            case NodeType::Blur:
                pf("Radius", &n->constant.x, 0.002f, 0.0f, 0.2f);
                break;
            case NodeType::Emboss:
                pf("Strength", &n->constant.x, 0.05f, 0.0f, 16.0f);
                break;
            case NodeType::SdfCircle:
                pf("Radius", &n->constant.x, 0.01f, 0.0f, 0.7f);
                break;
            case NodeType::SdfBox:
                pf("Half X", &n->constant.x, 0.01f, 0.0f, 0.7f);
                pf("Half Y", &n->constant.y, 0.01f, 0.0f, 0.7f);
                break;
            case NodeType::SdfOp:
                pcombo("Op", &n->constant.x, "Union\0Subtract\0Intersect\0Smooth union\0");
                pf("Smooth", &n->constant.y, 0.005f, 0.0f, 0.5f);
                break;
            case NodeType::SdfShow:
                pf("Width", &n->constant.x, 0.002f, 0.0f, 0.3f);
                break;

            default: {
                const char* cat = mat::NodeInfoOf(n->type).category;
                if (std::strcmp(cat, "Coordinate") == 0 || std::strcmp(cat, "Output") == 0 ||
                    n->type == NodeType::Grayscale || n->type == NodeType::Combine ||
                    n->type == NodeType::AngularGradient) {
                    ImGui::TextDisabled("%s has no editable fields.", mat::NodeInfoOf(n->type).name);
                } else {
                    ImGui::TextDisabled("Parameters:");
                    pf("X", &n->constant.x, 0.02f, 0.0f, 0.0f);
                    pf("Y", &n->constant.y, 0.02f, 0.0f, 0.0f);
                    pf("Z", &n->constant.z, 0.02f, 0.0f, 0.0f);
                    pf("W", &n->constant.w, 0.02f, 0.0f, 0.0f);
                }
                break;
            }
        }

        // Sampling space for the space-aware nodes.
        if (n->type == NodeType::Texture || n->type == NodeType::NormalMap ||
            n->type == NodeType::Noise || n->type == NodeType::Voronoi ||
            n->type == NodeType::Gradient) {
            int sp = static_cast<int>(n->space);
            if (ImGui::Combo("Space", &sp, "UV0\0UV1\0Object\0World\0Triplanar\0")) {
                mgSnap();
                n->space = static_cast<mat::Space>(sp);
                mgDirty_ = true;
            }
        }
        // Bind a constant-family node to an exposed parameter.
        if (n->type == NodeType::Constant || n->type == NodeType::Color ||
            n->type == NodeType::Float || n->type == NodeType::Vector) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", n->paramName.c_str());
            if (ImGui::InputText("Bind param", buf, sizeof(buf))) {
                n->paramName = buf;
                mgDirty_ = true;
            }
            mgSnapOnActivate();
        }
    }

    // Exposed parameters (instance overridable).
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Parameters")) {
        int removeIdx = -1;
        for (usize i = 0; i < mgGraph_.params.params.size(); ++i) {
            ImGui::PushID(1000 + static_cast<int>(i));
            mat::Param& p = mgGraph_.params.params[i];
            char nb[96];
            std::snprintf(nb, sizeof(nb), "%s", p.name.c_str());
            if (ImGui::InputText("Name", nb, sizeof(nb))) { p.name = nb; mgDirty_ = true; }
            mgSnapOnActivate();
            int ty = static_cast<int>(p.type);
            if (ImGui::Combo("Type", &ty, "Scalar\0Color\0Vector\0Texture\0Bool\0")) {
                mgSnap();
                p.type = static_cast<mat::ParamType>(ty);
                mgDirty_ = true;
            }
            if (p.type == mat::ParamType::Color) {
                if (ImGui::ColorEdit4("Value", &p.value.x)) mgDirty_ = true;
            } else if (p.type == mat::ParamType::Bool) {
                bool b = p.value.x != 0.0f;
                if (ImGui::Checkbox("Value", &b)) { p.value.x = b ? 1.0f : 0.0f; mgDirty_ = true; }
            } else if (p.type == mat::ParamType::Texture) {
                std::string picked;
                if (AssetPicker("Value", p.texture, ".uaf", uaf::AssetType::Texture, picked)) {
                    mgSnap();
                    p.texture = picked;
                    mgDirty_ = true;
                }
            } else {
                if (ImGui::DragFloat4("Value", &p.value.x, 0.01f)) mgDirty_ = true;
            }
            mgSnapOnActivate();
            if (ImGui::SmallButton("Remove")) removeIdx = static_cast<int>(i);
            ImGui::Separator();
            ImGui::PopID();
        }
        if (removeIdx >= 0) {
            mgSnap();
            mgGraph_.params.params.erase(mgGraph_.params.params.begin() + removeIdx);
            mgDirty_ = true;
        }
        if (ImGui::Button("+ Add parameter")) {
            mgSnap();
            mgGraph_.params.params.push_back(mat::Param{"NewParam", mat::ParamType::Scalar,
                                                        glm::vec4(0.0f), {}});
            mgDirty_ = true;
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

void Editor::DrawMaterialGraphCanvas(Engine& engine, float width) {
    (void)engine;
    const float NW = 190.0f, TITLEH = 24.0f, ROWH = 22.0f, PAD = 8.0f, PINR = 5.0f;
    const ImU32 kPin = IM_COL32(210, 214, 226, 255);
    // Undo snapshot (the panel's lambda is out of scope here). Push the graph BEFORE the edit.
    const auto mgSnap = [&] { mgHistory_.Push(mgGraph_); };

    if (width < 100.0f) width = 100.0f;
    ImGui::BeginChild("##mgcanvas", ImVec2(width, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cP0 = ImGui::GetCursorScreenPos();
    const ImVec2 cSz = ImGui::GetContentRegionAvail();
    const ImVec2 cP1(cP0.x + cSz.x, cP0.y + cSz.y);
    const ImVec2 mp = io.MousePos;
    const bool canvasHovered = ImGui::IsWindowHovered();

    dl->AddRectFilled(cP0, cP1, IM_COL32(26, 27, 32, 255));
    const float grid = 24.0f;
    const ImU32 gridCol = IM_COL32(40, 42, 48, 255);
    for (float x = std::fmod(mgPan_.x, grid); x < cSz.x; x += grid)
        dl->AddLine(ImVec2(cP0.x + x, cP0.y), ImVec2(cP0.x + x, cP1.y), gridCol);
    for (float y = std::fmod(mgPan_.y, grid); y < cSz.y; y += grid)
        dl->AddLine(ImVec2(cP0.x, cP0.y + y), ImVec2(cP1.x, cP0.y + y), gridCol);
    dl->AddRect(cP0, cP1, IM_COL32(12, 12, 16, 255));

    const ImVec2 origin(cP0.x + mgPan_.x, cP0.y + mgPan_.y);
    auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
    auto rowsOf = [&](const mat::Node& n) {
        return std::max(1, std::max<int>(MgInputCount(n.type), MgHasOutput(n.type) ? 1 : 0));
    };
    auto nodeH = [&](const mat::Node& n) { return TITLEH + ROWH * rowsOf(n) + PAD; };
    auto rowCenterY = [&](const mat::Node& n, int i) {
        return toScreen(n.uiPos).y + TITLEH + ROWH * i + ROWH * 0.5f;
    };
    auto inPinPos = [&](const mat::Node& n, int i) {
        return ImVec2(toScreen(n.uiPos).x, rowCenterY(n, i));
    };
    auto outPinPos = [&](const mat::Node& n) {
        return ImVec2(toScreen(n.uiPos).x + NW, toScreen(n.uiPos).y + nodeH(n) * 0.5f);
    };
    auto dist2 = [](ImVec2 a, ImVec2 b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    };

    // Hovered pin (nearest within a small radius).
    struct PinHover { u32 node = 0, pin = 0; bool isOutput = false; bool valid = false; };
    PinHover hov;
    {
        float best = (PINR + 4.0f) * (PINR + 4.0f);
        for (const mat::Node& n : mgGraph_.nodes) {
            const int ins = MgInputCount(n.type);
            for (int i = 0; i < ins; ++i) {
                const float dd = dist2(mp, inPinPos(n, i));
                if (dd < best) { best = dd; hov = {n.id, static_cast<u32>(i), false, true}; }
            }
            if (MgHasOutput(n.type)) {
                const float dd = dist2(mp, outPinPos(n));
                if (dd < best) { best = dd; hov = {n.id, 0, true, true}; }
            }
        }
    }

    // Links (bezier) + link hover.
    auto bez = [](ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t) {
        const float u = 1.0f - t;
        const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return ImVec2(w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
                      w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y);
    };
    int hoveredLink = -1;
    for (u32 li = 0; li < mgGraph_.links.size(); ++li) {
        const mat::Link& l = mgGraph_.links[li];
        const mat::Node* a = mgGraph_.FindNode(l.fromNode);
        const mat::Node* b = mgGraph_.FindNode(l.toNode);
        if (!a || !b) continue;
        if (static_cast<int>(l.toPin) >= MgInputCount(b->type) || !MgHasOutput(a->type)) continue;
        const ImVec2 p0 = outPinPos(*a);
        const ImVec2 p1 = inPinPos(*b, static_cast<int>(l.toPin));
        const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f);
        const ImVec2 c0(p0.x + dx, p0.y), c1(p1.x - dx, p1.y);
        float md = 1e9f;
        for (int s = 0; s <= 16; ++s) md = std::min(md, dist2(mp, bez(p0, c0, c1, p1, s / 16.0f)));
        const bool lh = canvasHovered && md < 36.0f && !hov.valid;
        if (lh) hoveredLink = static_cast<int>(li);
        dl->AddBezierCubic(p0, c0, c1, p1, lh ? IM_COL32(255, 200, 90, 255) : kPin, lh ? 3.5f : 2.2f);
    }

    // Nodes.
    for (mat::Node& n : mgGraph_.nodes) {
        const int ins = MgInputCount(n.type);
        const ImVec2 nMin = toScreen(n.uiPos);
        const ImVec2 nMax(nMin.x + NW, nMin.y + nodeH(n));
        const bool selected = (mgSelected_ == n.id);

        dl->AddRectFilled(nMin, nMax, IM_COL32(42, 44, 52, 240), 5.0f);
        dl->AddRectFilled(nMin, ImVec2(nMax.x, nMin.y + TITLEH), MgNodeColor(n.type), 5.0f,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(nMin, nMax, selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(18, 18, 22, 255),
                    5.0f, 0, selected ? 2.5f : 1.2f);
        dl->AddText(ImVec2(nMin.x + 9, nMin.y + 4), IM_COL32(240, 240, 245, 255),
                    mat::NodeInfoOf(n.type).name);

        const std::string preview = MgNodePreview(n);
        if (!preview.empty())
            dl->AddText(ImVec2(nMin.x + 10, nMin.y + TITLEH + 3), IM_COL32(185, 190, 205, 255),
                        preview.c_str());

        ImGui::PushID(static_cast<int>(n.id));
        ImGui::SetCursorScreenPos(nMin);
        ImGui::InvisibleButton("title", ImVec2(NW, TITLEH));
        if (ImGui::IsItemActivated()) mgSelected_ = n.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            n.uiPos.x += io.MouseDelta.x;
            n.uiPos.y += io.MouseDelta.y;
            mgDirty_ = true;
        }

        // Input pins (left) + labels.
        for (int i = 0; i < ins; ++i) {
            const ImVec2 pp = inPinPos(n, i);
            dl->AddTriangleFilled(ImVec2(pp.x - 4, pp.y - 5), ImVec2(pp.x - 4, pp.y + 5),
                                  ImVec2(pp.x + 5, pp.y), kPin);
            const std::string lbl = MgInputLabel(n.type, static_cast<u8>(i));
            if (!lbl.empty())
                dl->AddText(ImVec2(pp.x + 9, pp.y - 7), IM_COL32(200, 202, 210, 255), lbl.c_str());
        }
        // Output pin (right).
        if (MgHasOutput(n.type)) {
            const ImVec2 pp = outPinPos(n);
            dl->AddTriangleFilled(ImVec2(pp.x - 5, pp.y - 5), ImVec2(pp.x - 5, pp.y + 5),
                                  ImVec2(pp.x + 4, pp.y), kPin);
        }
        ImGui::PopID();
    }

    // In-progress wire.
    if (mgDragging_) {
        if (const mat::Node* s = mgGraph_.FindNode(mgDragNode_)) {
            const ImVec2 sp = mgDragFromOutput_ ? outPinPos(*s)
                                                : inPinPos(*s, static_cast<int>(mgDragPin_));
            const float dx = std::max(40.0f, std::fabs(mp.x - sp.x) * 0.5f);
            const float dir = mgDragFromOutput_ ? 1.0f : -1.0f;
            dl->AddBezierCubic(sp, ImVec2(sp.x + dx * dir, sp.y), ImVec2(mp.x - dx * dir, mp.y), mp,
                               IM_COL32(255, 235, 150, 255), 2.4f);
        } else {
            mgDragging_ = false;
        }
    }

    // Start / re-pick a wire.
    if (canvasHovered && hov.valid && !mgDragging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool started = false;
        if (!hov.isOutput) {
            // Picking up an input pin that already has a wire detaches it and re-drags the source.
            if (const mat::Link* l = mgGraph_.LinkInto(hov.node, static_cast<u8>(hov.pin))) {
                mgDragNode_ = l->fromNode;
                mgDragPin_ = 0;
                mgDragFromOutput_ = true;
                mgSnap();
                mgGraph_.Disconnect(hov.node, static_cast<u8>(hov.pin));
                mgDirty_ = true;
                started = true;
            }
        }
        if (!started) {
            mgDragNode_ = hov.node;
            mgDragPin_ = hov.pin;
            mgDragFromOutput_ = hov.isOutput;
        }
        mgDragging_ = true;
    }
    // Release: connect an output to an input (opposite orientation).
    if (mgDragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (hov.valid && hov.isOutput != mgDragFromOutput_) {
            u32 fromNode, toNode;
            u8 toPin;
            if (mgDragFromOutput_) { fromNode = mgDragNode_; toNode = hov.node; toPin = static_cast<u8>(hov.pin); }
            else { fromNode = hov.node; toNode = mgDragNode_; toPin = static_cast<u8>(mgDragPin_); }
            if (fromNode != toNode) {
                mgSnap(); // Connect replaces any existing wire on that input pin
                mgGraph_.Connect(fromNode, toNode, toPin);
                mgDirty_ = true;
            }
        }
        mgDragging_ = false;
    }

    // Empty-canvas click deselects.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hov.valid &&
        !ImGui::IsAnyItemHovered())
        mgSelected_ = 0;

    // Pan: middle-drag or left-drag empty space.
    if (canvasHovered && !mgDragging_ && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
            (!hov.valid && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))) {
            mgPan_.x += io.MouseDelta.x;
            mgPan_.y += io.MouseDelta.y;
        }
    }

    // Delete the selected node.
    if (mgSelected_ && ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        mgSnap();
        mgGraph_.RemoveNode(mgSelected_);
        mgSelected_ = 0;
        mgDirty_ = true;
    }

    // Right-click: delete a hovered link, else the add-node menu (grouped by catalog category).
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hoveredLink >= 0) {
            const mat::Link l = mgGraph_.links[static_cast<usize>(hoveredLink)];
            mgSnap();
            mgGraph_.Disconnect(l.toNode, l.toPin);
            mgDirty_ = true;
        } else if (!hov.valid) {
            mgAddPos_ = glm::vec2(mp.x - origin.x, mp.y - origin.y);
            ImGui::OpenPopup("##mgadd");
        }
    }
    if (ImGui::BeginPopup("##mgadd")) {
        const bool hasOutput = mgGraph_.OutputNode() != nullptr;
        auto addItem = [&](const mat::NodeInfo& info) {
            if (info.type == NodeType::Output && hasOutput) return; // one Output only
            if (ImGui::MenuItem(info.name)) {
                mgSnap();
                mgSelected_ = mgGraph_.AddNode(info.type, mgAddPos_);
                mgDirty_ = true;
            }
        };
        static char mgAddSearch[64] = "";
        if (ImGui::IsWindowAppearing()) {
            mgAddSearch[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(190.0f);
        ImGui::InputTextWithHint("##mgsearch", "search nodes...", mgAddSearch, sizeof(mgAddSearch));
        ImGui::Separator();
        if (mgAddSearch[0] != '\0') {
            std::string q = mgAddSearch;
            for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (const mat::NodeInfo& info : mat::NodeCatalog()) {
                std::string nm = info.name;
                for (char& ch : nm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (nm.find(q) != std::string::npos) addItem(info);
            }
        } else {
            const char* cats[] = {"Input",  "Generator", "Procedural", "Filter", "Transform",
                                  "SDF",    "Math",      "Coordinate", "Layer",  "Output"};
            for (const char* cat : cats) {
                if (!ImGui::BeginMenu(cat)) continue;
                for (const mat::NodeInfo& info : mat::NodeCatalog())
                    if (std::strcmp(info.category, cat) == 0) addItem(info);
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

} // namespace hbe

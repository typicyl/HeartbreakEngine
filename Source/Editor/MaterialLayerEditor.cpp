// Editor/MaterialLayerEditor.cpp - the Photoshop-style MATERIAL LAYER STACK editor (Window > Material
// Layers). A `.hbmatlayer` is `base material + a bottom-to-top list of layers`, each with a MASK
// (Constant / Box / Procedural / Paint), a blend mode, opacity, and normal/height contribution. This
// panel makes that stack visual and reorderable instead of hand-editing JSON: add / remove / reorder
// layers, pick each layer's material, choose its mask + blend, and watch a live preview baked through
// the SAME resolver the runtime uses (mat::Resolve via mat::BakeLayerStack). All backend pieces
// (LayerStack, MaskSource, Resolve, BakeLayerStack, SaveLayerStack/LoadLayerStack) are pre-existing
// and headless-tested; this file is purely the authoring surface on top of them.
#include "Editor/Editor.h"

#include "Assets/MaterialAsset.h" // assets::LoadMaterial (a .hbmat -> a layer's SurfaceParams)
#include "Assets/UAF.h" // uaf::AssetType (asset picker)
#include "Core/Log.h"
#include "Engine/Engine.h" // Engine::GetRenderer
#include "Material/MaterialCook.h" // mat::BakeLayerStack (live preview)
#include "Material/MaterialLayer.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"

#include <imgui.h>

#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {
namespace {

// The single in-editor document + its preview cache (one editor instance; mirrors the file-local
// g_gpu in MaterialGraphEditor). Not persisted; Save/Open move it to/from a `.hbmatlayer`.
struct LayerDoc {
    mat::LayerStack stack;
    std::string path; // `.hbmatlayer` relative to Assets/ ("" = unsaved)
    bool loaded = false;
    u64 previewHash = 0;
    u64 previewId = 0;
};
LayerDoc g_doc;

const char* kMaskNames[] = {"Constant", "Box", "Procedural", "Paint"};
const char* kBlendNames[] = {"Linear", "Height", "Height + Noise"};

SurfaceParams SolidSurface(glm::vec3 color, f32 rough, f32 metal) {
    SurfaceParams s;
    s.base_color = glm::vec4(color, 1.0f);
    s.specular_roughness = rough;
    s.base_metalness = metal;
    return s;
}

mat::Layer MakeLayer(const char* /*name*/, glm::vec3 color, f32 rough, f32 metal, f32 opacity,
                     mat::BlendMode blend) {
    mat::Layer l;
    l.surface = SolidSurface(color, rough, metal);
    l.opacity = opacity;
    l.blend = blend;
    l.mask.kind = mat::MaskKind::Constant;
    l.mask.constant = 1.0f;
    return l;
}

// Built-in starter stacks so an artist gets an instant, editable result (the brief's "presets").
mat::LayerStack PresetRockMoss() {
    mat::LayerStack s;
    s.base = SolidSurface({0.42f, 0.40f, 0.38f}, 0.85f, 0.0f); // grey rock
    s.layers.push_back(MakeLayer("Moss", {0.16f, 0.30f, 0.10f}, 0.9f, 0.0f, 0.6f,
                                 mat::BlendMode::Height)); // moss settles in the low spots
    return s;
}
mat::LayerStack PresetSnowRock() {
    mat::LayerStack s;
    s.base = SolidSurface({0.34f, 0.33f, 0.35f}, 0.8f, 0.0f); // dark rock
    s.layers.push_back(
        MakeLayer("Snow", {0.92f, 0.94f, 0.98f}, 0.6f, 0.0f, 0.75f, mat::BlendMode::Height));
    return s;
}
mat::LayerStack PresetSandPuddle() {
    mat::LayerStack s;
    s.base = SolidSurface({0.76f, 0.68f, 0.50f}, 0.9f, 0.0f); // sand
    s.layers.push_back(
        MakeLayer("Puddle", {0.05f, 0.05f, 0.06f}, 0.06f, 0.0f, 0.5f, mat::BlendMode::Height));
    return s;
}

// Bake the stack's resolved base colour to a swatch, gated on the stack hash so we upload once per
// edit (not per frame).
void DrawPreview(Editor&, Renderer& renderer, LayerDoc& doc) {
    const u64 h = doc.stack.Hash();
    if (h != doc.previewHash) {
        const mat::BakedMaterial b = mat::BakeLayerStack(doc.stack, 128);
        if (b.Valid()) {
            std::vector<u32> px(static_cast<usize>(b.width) * b.height, 0u);
            for (usize i = 0; i < px.size(); ++i)
                px[i] = static_cast<u32>(b.color[i * 4 + 0]) |
                        (static_cast<u32>(b.color[i * 4 + 1]) << 8) |
                        (static_cast<u32>(b.color[i * 4 + 2]) << 16) |
                        (static_cast<u32>(b.color[i * 4 + 3]) << 24);
            rhi::TextureDesc d;
            d.width = b.width;
            d.height = b.height;
            d.format = rhi::Format::R8G8B8A8_UNORM;
            d.pixels = px.data();
            d.debugName = "matlayer_preview";
            doc.previewId = renderer.TextureUIId(renderer.UploadTexture(d));
        }
        doc.previewHash = h;
    }
    if (doc.previewId != 0)
        ImGui::Image(static_cast<ImTextureID>(doc.previewId), ImVec2(128, 128));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Live preview");
    ImGui::TextDisabled("%zu layer(s)", doc.stack.layers.size());
    ImGui::TextDisabled("Box/Procedural/Paint masks");
    ImGui::TextDisabled("vary across a real surface;");
    ImGui::TextDisabled("the swatch bakes over a 1m quad.");
    ImGui::EndGroup();
}

// One SurfaceParams editor (inline colour / roughness / metallic + optional .hbmat pick).
void EditSurface(Editor& ed, const char* id, SurfaceParams& surf, std::string& matRef) {
    ImGui::PushID(id);
    ImGui::ColorEdit3("Color", glm::value_ptr(surf.base_color));
    ImGui::DragFloat("Roughness", &surf.specular_roughness, 0.005f, 0.0f, 1.0f);
    ImGui::DragFloat("Metallic", &surf.base_metalness, 0.005f, 0.0f, 1.0f);
    std::string pick;
    if (ed.AssetPickerPublic("From .hbmat", matRef, ".hbmat", pick) && !pick.empty() &&
        Project::HasActive()) {
        if (auto ma = assets::LoadMaterial(Project::Active().AssetsDir() / pick)) {
            surf = ma->surface;
            matRef = pick;
        }
    }
    ImGui::PopID();
}

} // namespace

bool Editor::AssetPickerPublic(const char* label, const std::string& current, const char* extension,
                               std::string& out) {
    return AssetPicker(label, current, extension, uaf::AssetType::Unknown, out);
}

void Editor::DrawMaterialLayers(Engine& engine) {
    if (!panelOpen_[Panel_MaterialLayers]) return;
    if (!ImGui::Begin("Material Layers", &panelOpen_[Panel_MaterialLayers])) {
        ImGui::End();
        return;
    }
    Renderer& renderer = engine.GetRenderer();
    LayerDoc& doc = g_doc;
    if (!doc.loaded) {
        doc.stack = PresetRockMoss();
        doc.loaded = true;
    }

    // --- Toolbar: New (presets) / Open / Save --------------------------------
    if (ImGui::Button("New")) {
        doc.stack = mat::LayerStack{};
        doc.path.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset")) ImGui::OpenPopup("##mlpreset");
    if (ImGui::BeginPopup("##mlpreset")) {
        if (ImGui::Selectable("Rock + Moss")) { doc.stack = PresetRockMoss(); doc.path.clear(); }
        if (ImGui::Selectable("Snow on Rock")) { doc.stack = PresetSnowRock(); doc.path.clear(); }
        if (ImGui::Selectable("Sand + Puddle")) { doc.stack = PresetSandPuddle(); doc.path.clear(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
        std::string pick;
        if (AssetPickerPublic("Open (.hbmatlayer)", doc.path, mat::kLayerStackExtension, pick) &&
            !pick.empty() && Project::HasActive()) {
            if (auto s = mat::LoadLayerStack(Project::Active().AssetsDir() / pick)) {
                doc.stack = *s;
                doc.path = pick;
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!Project::HasActive());
    if (ImGui::Button("Save")) {
        const std::filesystem::path dir = Project::Active().AssetsDir() / "materials";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string rel = doc.path;
        if (rel.empty()) rel = std::string("materials/layered") + mat::kLayerStackExtension;
        if (mat::SaveLayerStack(Project::Active().AssetsDir() / rel, doc.stack)) {
            doc.path = rel;
            HBE_INFO("Saved layer stack -> {}", rel);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", doc.path.empty() ? "(unsaved)" : doc.path.c_str());

    ImGui::Separator();
    DrawPreview(*this, renderer, doc);
    ImGui::Separator();

    // --- Base material -------------------------------------------------------
    if (ImGui::CollapsingHeader("Base Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        static std::string baseRef; // base has no persisted .hbmat ref in the stack; local convenience
        EditSurface(*this, "base", doc.stack.base, baseRef);
    }

    // --- Layers (displayed TOP-first, like a paint program) ------------------
    ImGui::SeparatorText("Layers (top = front)");
    int moveUp = -1, moveDown = -1, remove = -1; // deferred structural edits
    const int n = static_cast<int>(doc.stack.layers.size());
    for (int disp = 0; disp < n; ++disp) {
        const int k = n - 1 - disp; // real index (top of the list = last layer)
        mat::Layer& L = doc.stack.layers[k];
        ImGui::PushID(k);

        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "Layer %d  [%s / %s]", k, kMaskNames[static_cast<int>(L.mask.kind)],
                      kBlendNames[static_cast<int>(L.blend)]);
        const bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);

        // Row of controls: reorder / remove / opacity.
        if (ImGui::SmallButton("Up")) moveUp = k;
        ImGui::SameLine();
        if (ImGui::SmallButton("Dn")) moveDown = k;
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) remove = k;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Opacity", &L.opacity, 0.0f, 1.0f, "%.2f");

        if (open) {
            int mk = static_cast<int>(L.mask.kind);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Mask", &mk, kMaskNames, IM_ARRAYSIZE(kMaskNames)))
                L.mask.kind = static_cast<mat::MaskKind>(mk);
            if (L.mask.kind == mat::MaskKind::Constant) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("Weight", &L.mask.constant, 0.0f, 1.0f, "%.2f");
            } else {
                ImGui::SameLine();
                ImGui::Checkbox("Invert", &L.mask.invert);
                ImGui::TextDisabled(
                    L.mask.kind == mat::MaskKind::Box
                        ? "Box mask: a projection volume in the scene drives this weight."
                    : L.mask.kind == mat::MaskKind::Procedural
                        ? "Procedural mask: a baked graph/noise channel drives this weight."
                        : "Paint mask: the paint canvas drives this weight.");
            }
            int bl = static_cast<int>(L.blend);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Blend", &bl, kBlendNames, IM_ARRAYSIZE(kBlendNames)))
                L.blend = static_cast<mat::BlendMode>(bl);
            if (L.blend != mat::BlendMode::Linear) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("Height", &L.layerHeight, 0.0f, 1.0f, "%.2f");
            }
            ImGui::Checkbox("Affects Normal", &L.contributesNormal);
            ImGui::SameLine();
            ImGui::Checkbox("Affects Height", &L.contributesHeight);
            EditSurface(*this, "surf", L.surface, L.material);
        }
        ImGui::PopID();
    }

    // Apply deferred structural edits (after the loop so indices stay valid).
    auto& layers = doc.stack.layers;
    if (remove >= 0 && remove < static_cast<int>(layers.size()))
        layers.erase(layers.begin() + remove);
    if (moveUp >= 0 && moveUp + 1 < static_cast<int>(layers.size()))
        std::swap(layers[moveUp], layers[moveUp + 1]); // "up" in display = toward the front (higher index)
    if (moveDown > 0 && moveDown < static_cast<int>(layers.size()))
        std::swap(layers[moveDown], layers[moveDown - 1]);

    if (ImGui::Button("+ Add Layer"))
        layers.push_back(MakeLayer("Layer", {0.7f, 0.7f, 0.7f}, 0.6f, 0.0f, 1.0f,
                                   mat::BlendMode::Linear));

    ImGui::End();
}

} // namespace hbe

// UI/WidgetVTableTest.cpp - the P9.2 frozen-vertex parity gate (--test-uivtable).
//
// Proves the WidgetVTable emit path (Source/UI/UISystem.cpp) is BYTE-IDENTICAL to the
// legacy per-type switch it replaces. It builds an in-code corpus of widgets, emits it
// once with the legacy switch and once through the vtable, and memcmp's the two
// rhi::UIVertex streams. Behaviour-preserving-by-construction (the arms move verbatim)
// is only a claim until this passes; this is the proof, and the permanent guard against
// a future edit to any emit arm silently changing a byte.
//
// Runs in a real GPU session (so fonts bake and the emit path is the shipped one) but
// needs NO project - the corpus is built here, not loaded. See main_editor.cpp for the
// engine-loop harness and docs/Design-UIWidgetRegistry.md for the plan.
#include "UI/UISystem.h"

#include "Core/Log.h"
#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cstring>
#include <vector>

namespace hbe::ui {

namespace {
// Add one UIElement under `parent` at `offset`, size `size`; returns the entity so
// callers can nest children (e.g. a ScrollView's overflowing content).
entt::entity Add(Scene& scene, entt::entity parent, UIElement el, glm::vec2 offset,
                 glm::vec2 size) {
    auto& reg = scene.Registry();
    const entt::entity e = scene.CreateEntity("VTableCorpus");
    el.offset = offset;
    el.size = size;
    reg.emplace<UIElement>(e, std::move(el));
    reg.emplace<Parent>(e, Parent{parent});
    return e;
}
} // namespace

bool WidgetVTableSelfTest(Scene& scene, Renderer& renderer) {
    using T = UIElement::Type;
    auto& reg = scene.Registry();
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("uivtable: FAILED - {}", what);
        }
    };

    // --- Corpus canvas (screen space; no project needed) ---
    const entt::entity canvasE = scene.CreateEntity("VTableCanvas");
    UICanvas canvas;
    canvas.refWidth = 1920.0f;
    canvas.refHeight = 1080.0f;
    reg.emplace<UICanvas>(canvasE, canvas);

    // --- Corpus: every Type present, with state variants on the EXTRACTED ones ---
    // (Label/Image/Panel are the ones through the vtable today; the other seven fall
    // through to the legacy switch on BOTH builds, so they are trivially identical now
    // and become meaningful coverage as each is extracted.)
    f32 y = 0.0f;
    const auto row = [&]() {
        const f32 v = y;
        y += 56.0f;
        return glm::vec2(20.0f, v);
    };

    // Labels: alignment + colour-alpha variants (exercises Emitter::Text + colour).
    for (int a = 0; a < 3; ++a) {
        UIElement el;
        el.type = T::Label;
        el.text = "Widget vtable parity check";
        el.textSize = 22.0f;
        el.hAlign = a == 0   ? UIElement::HAlign::Left
                    : a == 1 ? UIElement::HAlign::Center
                             : UIElement::HAlign::Right;
        el.color = glm::vec4(0.9f, 0.82f, 0.7f, a == 2 ? 0.5f : 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(420.0f, 48.0f));
    }
    // Panels: plain colour rect, colour+text, and a 9-slice (sliced quad path).
    {
        UIElement el;
        el.type = T::Panel;
        el.color = glm::vec4(0.2f, 0.3f, 0.4f, 1.0f);
        el.text = "Panel with text";
        el.textSize = 20.0f;
        Add(scene, canvasE, el, row(), glm::vec2(320.0f, 48.0f));
    }
    {
        UIElement el;
        el.type = T::Panel;
        el.color = glm::vec4(0.5f, 0.2f, 0.2f, 0.8f);
        el.slice = glm::vec4(4.0f, 4.0f, 4.0f, 4.0f); // sliced, but no texture -> plain quad
        Add(scene, canvasE, el, row(), glm::vec2(320.0f, 48.0f));
    }
    // Images: intentional colour rect (empty path) and a failed-load path (tex==0 -> skip).
    {
        UIElement el;
        el.type = T::Image;
        el.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(72.0f, 72.0f));
    }
    {
        UIElement el;
        el.type = T::Image;
        el.texture = "__vtable_no_such_texture.png"; // resolves to 0 on both builds
        Add(scene, canvasE, el, row(), glm::vec2(72.0f, 72.0f));
    }
    // Button: hovered state (exercises StateFill's hover branch + auto-contrast text).
    {
        UIElement el;
        el.type = T::Button;
        el.text = "Play";
        el.textSize = 20.0f;
        el.color = glm::vec4(0.85f, 0.85f, 0.2f, 1.0f); // bright -> dark auto text
        el.hoverColor = glm::vec4(0.95f, 0.95f, 0.3f, 1.0f);
        el.hovered = true;
        Add(scene, canvasE, el, row(), glm::vec2(200.0f, 44.0f));
    }
    // ProgressBar: linear + radial (the Wheel path).
    {
        UIElement el;
        el.type = T::ProgressBar;
        el.fill = 0.37f;
        el.color = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
        el.fillColor = glm::vec4(0.3f, 0.8f, 0.4f, 1.0f);
        el.text = "37%";
        Add(scene, canvasE, el, row(), glm::vec2(240.0f, 28.0f));
    }
    {
        UIElement el;
        el.type = T::ProgressBar;
        el.radial = true;
        el.fill = 0.66f;
        el.color = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
        el.fillColor = glm::vec4(0.9f, 0.5f, 0.2f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(64.0f, 64.0f));
    }
    // Slider: a mid value (track + fill + handle geometry).
    {
        UIElement el;
        el.type = T::Slider;
        el.value = 0.42f;
        el.color = glm::vec4(0.3f, 0.3f, 0.35f, 1.0f);
        el.fillColor = glm::vec4(0.4f, 0.6f, 0.9f, 1.0f);
        el.handleSize = 18.0f;
        Add(scene, canvasE, el, row(), glm::vec2(240.0f, 32.0f));
    }
    // Toggle: off and on (the "On"/"Off" text branch, untextured).
    for (bool on : {false, true}) {
        UIElement el;
        el.type = T::Toggle;
        el.toggled = on;
        el.textSize = 18.0f;
        el.color = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);
        el.fillColor = glm::vec4(0.3f, 0.7f, 0.3f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(80.0f, 32.0f));
    }
    // Selector: >=3 options + a selection - exercises the per-option Text loop, which
    // is the load-bearing textSlot-ordering case (hazard 5.1).
    {
        UIElement el;
        el.type = T::Selector;
        el.options = {"Low", "Medium", "High"};
        el.selected = 1;
        el.textSize = 18.0f;
        el.color = glm::vec4(0.25f, 0.25f, 0.3f, 1.0f);
        el.fillColor = glm::vec4(0.4f, 0.4f, 0.6f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(300.0f, 36.0f));
    }
    // TextInput: filled + empty-with-placeholder (both non-editing; the caret/editing
    // path needs a UIContext and is covered by the interact-parity follow-up).
    {
        UIElement el;
        el.type = T::TextInput;
        el.text = "hello world";
        el.textSize = 18.0f;
        el.color = glm::vec4(0.15f, 0.15f, 0.18f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(260.0f, 34.0f));
    }
    {
        UIElement el;
        el.type = T::TextInput;
        el.placeholder = "type here...";
        el.textSize = 18.0f;
        el.color = glm::vec4(0.15f, 0.15f, 0.18f, 1.0f);
        Add(scene, canvasE, el, row(), glm::vec2(260.0f, 34.0f));
    }
    // ScrollView: overflowing content (two tall children) so the scrollbar thumb path
    // runs; scrollPos partway down.
    {
        UIElement sv;
        sv.type = T::ScrollView;
        sv.color = glm::vec4(0.1f, 0.1f, 0.12f, 1.0f);
        sv.fillColor = glm::vec4(0.5f, 0.5f, 0.55f, 1.0f);
        sv.scrollVertical = true;
        sv.scrollPos = glm::vec2(0.0f, 300.0f);
        const entt::entity svE =
            Add(scene, canvasE, sv, glm::vec2(400.0f, 0.0f), glm::vec2(200.0f, 160.0f));
        for (int i = 0; i < 4; ++i) {
            UIElement child;
            child.type = T::Label;
            child.text = "content row";
            child.textSize = 18.0f;
            child.color = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
            Add(scene, svE, child, glm::vec2(8.0f, 8.0f + i * 120.0f),
                glm::vec2(180.0f, 100.0f));
        }
    }

    // --- Converge, THEN capture one build per path from the SAME state ---
    // An auto-sized ScrollView measures contentExtent from the previous layout and that
    // value PERSISTS in the component; it is a fixed point after the first build. Emit
    // never writes it (layout does, identically for both paths), so once converged the
    // two paths read the same value. Warm up first, then capture golden (legacy) and
    // candidate (vtable) each once - if we instead built twice-per-path sequentially,
    // the vtable capture would have iterated more times and the thumb geometry would
    // drift, a harness artifact rather than an emit difference.
    const CanvasConfig cfg;
    const glm::vec2 target(cfg.refWidth, cfg.refHeight);
    const std::filesystem::path assetsDir; // no project: textures fail identically both ways

    const bool prev = GetUIUseVTable();
    std::vector<rhi::UIVertex> golden, candidate, prevBuild;
    std::vector<WorldUIBatch> gb, cb;

    // Loop LEGACY builds until two in a row are byte-identical: the ScrollView's
    // contentExtent is auto-measured, persists in the component, and settles over
    // successive builds rather than in one pass. Once the stream reaches a fixed point,
    // `golden` holds the converged legacy build and the single vtable capture below
    // reads that same converged state (emit never writes contentExtent - layout does,
    // identically for both paths).
    SetUIUseVTable(false);
    bool converged = false;
    for (int i = 0; i < 64 && !converged; ++i) {
        std::swap(golden, prevBuild);
        golden.clear();
        gb.clear();
        BuildVertices(scene, renderer, assetsDir, target, cfg, golden, &gb);
        converged = i > 0 && golden.size() == prevBuild.size() &&
                    (golden.empty() ||
                     std::memcmp(golden.data(), prevBuild.data(),
                                 golden.size() * sizeof(rhi::UIVertex)) == 0);
    }
    expect(converged, "the corpus layout converges to a fixed point");

    SetUIUseVTable(true);
    BuildVertices(scene, renderer, assetsDir, target, cfg, candidate, &cb);

    SetUIUseVTable(prev);

    expect(!golden.empty(), "the corpus emits geometry");
    expect(golden.size() == candidate.size(),
           "vertex COUNT parity (legacy switch vs WidgetVTable)");
    if (!golden.empty() && golden.size() == candidate.size()) {
        const bool same = std::memcmp(golden.data(), candidate.data(),
                                      golden.size() * sizeof(rhi::UIVertex)) == 0;
        expect(same, "WidgetVTable emit is BYTE-IDENTICAL to the legacy switch");
        if (!same) {
            for (usize i = 0; i < golden.size(); ++i) {
                const rhi::UIVertex& g = golden[i];
                const rhi::UIVertex& d = candidate[i];
                if (std::memcmp(&g, &d, sizeof(rhi::UIVertex)) != 0) {
                    HBE_ERROR("uivtable: first diff at vertex {} of {}", (u32)i,
                              (u32)golden.size());
                    HBE_ERROR("  legacy : xy({},{}) uv({},{}) rgba({},{},{},{}) tex{} fx{} "
                              "clip({},{},{},{})",
                              g.x, g.y, g.u, g.v, g.r, g.g, g.b, g.a, g.texIndex, g.fx,
                              g.clipX0, g.clipY0, g.clipX1, g.clipY1);
                    HBE_ERROR("  vtable : xy({},{}) uv({},{}) rgba({},{},{},{}) tex{} fx{} "
                              "clip({},{},{},{})",
                              d.x, d.y, d.u, d.v, d.r, d.g, d.b, d.a, d.texIndex, d.fx,
                              d.clipX0, d.clipY0, d.clipX1, d.clipY1);
                    break;
                }
            }
        }
    }
    HBE_INFO("uivtable: {} verts over the corpus (legacy {} / vtable {})",
             static_cast<u32>(golden.size()), static_cast<u32>(golden.size()),
             static_cast<u32>(candidate.size()));

    // Interact parity: the vertex compare above can't see input handling, so drive the
    // pointer over every interactive widget both ways and compare the flag mutations.
    if (!WidgetPointerParitySelfTest(scene, target, cfg)) ok = false;
    return ok;
}

} // namespace hbe::ui

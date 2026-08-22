// Editor/ParticleEditor.cpp - the dedicated PARTICLE EDITOR (Window > Particle Editor).
//
// A spacious, sectioned authoring surface for a native particle effect (`.hbvfx`), built as the
// simpler in-editor alternative to the external Effekseer editor. It authors the SAME rich
// ParticleEmitter the runtime simulates (module-stack sim, 8 presets, both CPU + GPU paths), and
// previews it LIVE: the effect runs as a real emitter in the scene, ticked every frame by
// particle::Update in edit mode, so every slider drag updates the running simulation instantly.
//
// PREVIEW LIFECYCLE. The preview is a real scene entity carrying the authored ParticleEmitter plus
// a ParticlePreviewTag, which the scene serializer treats as a write-exclusion - so the preview is
// simulated and drawn but never saved into the user's scene. It exists only while the panel is open
// AND the editor is in edit mode; whenever it goes away (panel closed, Play entered, scene reloaded)
// the authored fields are stashed so nothing is lost, and it is rebuilt from that stash on return.
//
// All the heavy lifting - the sim, the templates, the `.hbvfx` (de)serialization - is pre-existing
// (Scene/ParticleSystem, Scene/EffectAsset, Vfx/*). This file is purely the authoring UI on top.
#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Engine/Engine.h"       // Engine::GetScene
#include "Project/Project.h"
#include "Renderer/Renderer.h"   // Renderer::FocusOn (frame the preview)
#include "Scene/Components.h"     // ParticleEmitter, Transform, ParticlePreviewTag
#include "Scene/EffectAsset.h"    // particle::Save/LoadEffect, Effect<->String
#include "Scene/ParticleSystem.h" // particle::MakeTemplate / TemplateName / Template
#include "Scene/Scene.h"

#include <imgui.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

namespace hbe {
namespace {

// The single in-editor document. `preview` is the live scene entity; `authored` stashes the authored
// fields whenever no preview entity exists so a close / Play / scene-reload never loses the effect.
struct ParticleDoc {
    entt::entity preview = entt::null;
    std::string path;         // `.hbvfx` relative to Assets/ ("" = unsaved)
    ParticleEmitter authored; // authored fields only (runtime state left default)
    bool haveAuthored = false;
    glm::vec3 previewPos{0.0f, 0.5f, 0.0f}; // where the preview emitter sits in the world
};
ParticleDoc g_pdoc;

// Authored-only copy: round-trip through the `.hbvfx` serializer - the single source of truth for
// which fields are authored - so the live pool/stack/RNG state is dropped rather than deep-copied
// (copying the SoA pool would be both wrong here and expensive).
ParticleEmitter AuthoredCopy(const ParticleEmitter& src) {
    if (auto e = particle::EffectFromString(particle::EffectToString(src))) return *e;
    return src; // the round-trip should never fail; degrade to a shallow copy if it somehow does
}

// Replace an emitter's AUTHORED fields in place while carrying its live pool/stack/RNG across and
// re-arming the emission window - the exact pattern the inline inspector uses for presets, so a
// one-shot preset (Explosion/Sparks) actually re-fires instead of landing on a spent window.
void ApplyAuthoredInPlace(ParticleEmitter& pe, ParticleEmitter next) {
    next.pool = std::move(pe.pool);
    next.stack = std::move(pe.stack);
    next.state = pe.state;
    next.state.emitterAge = 0.0f;
    next.state.burstFired = false;
    next.state.wasEmitting = false;
    next.stackSignature = pe.stackSignature;
    pe = std::move(next);
    pe.textureResolved = false;
}

// A fresh, gentle default effect for "New".
ParticleEmitter DefaultEffect() { return particle::MakeTemplate(particle::Template::Fire); }

// ---------------------------------------------------------------------------
// Value-over-life editor widgets (gradient bar + 2D curve graph)
// ---------------------------------------------------------------------------
// Selection/grab state. Only one gradient and one curve editor is ever interacted with at a time
// (the Color and Size sections), so a single set of statics keyed by the owning object is enough.
int g_gradSel = -1, g_gradGrab = -1;
const void* g_gradOwner = nullptr;
int g_curveSel = -1, g_curveGrab = -1;
const void* g_curveOwner = nullptr;

ImU32 Col(const glm::vec4& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
}

// A horizontal gradient bar with draggable colour stops (double-click to add, Remove to delete).
// Stops keep their order: dragging a stop is clamped between its neighbours. Returns true on edit.
bool GradientEditor(const char* id, VfxGradient& g) {
    bool changed = false;
    ImGui::PushID(id);
    if (g_gradOwner != &g) { g_gradSel = g_gradGrab = -1; g_gradOwner = &g; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::CalcItemWidth();
    const float barH = 22.0f, handleH = 9.0f;
    ImGui::InvisibleButton("##bar", ImVec2(W, barH + handleH));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const auto xOf = [&](f32 t) { return p0.x + t * W; };
    const auto tOf = [&](f32 x) { return glm::clamp((x - p0.x) / glm::max(W, 1.0f), 0.0f, 1.0f); };
    const int n = static_cast<int>(g.stops.size());

    // Bar fill.
    if (n == 0) {
        dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + barH), IM_COL32(50, 50, 50, 255));
    } else {
        dl->AddRectFilled(p0, ImVec2(xOf(g.stops.front().t), p0.y + barH), Col(g.stops.front().color));
        for (int i = 1; i < n; ++i)
            dl->AddRectFilledMultiColor(ImVec2(xOf(g.stops[i - 1].t), p0.y),
                                        ImVec2(xOf(g.stops[i].t), p0.y + barH),
                                        Col(g.stops[i - 1].color), Col(g.stops[i].color),
                                        Col(g.stops[i].color), Col(g.stops[i - 1].color));
        dl->AddRectFilled(ImVec2(xOf(g.stops.back().t), p0.y), ImVec2(p0.x + W, p0.y + barH),
                          Col(g.stops.back().color));
    }
    dl->AddRect(p0, ImVec2(p0.x + W, p0.y + barH), IM_COL32(0, 0, 0, 200));
    // Stop handles.
    for (int i = 0; i < n; ++i) {
        const float x = xOf(g.stops[i].t);
        const ImU32 hc = (i == g_gradSel) ? IM_COL32(255, 230, 0, 255) : IM_COL32(230, 230, 230, 255);
        dl->AddTriangleFilled(ImVec2(x, p0.y + barH + 1), ImVec2(x - 4, p0.y + barH + handleH),
                              ImVec2(x + 4, p0.y + barH + handleH), hc);
        dl->AddTriangle(ImVec2(x, p0.y + barH + 1), ImVec2(x - 4, p0.y + barH + handleH),
                        ImVec2(x + 4, p0.y + barH + handleH), IM_COL32(0, 0, 0, 200));
    }
    // Grab nearest handle on press.
    if (ImGui::IsItemActivated()) {
        g_gradGrab = -1;
        float best = 7.0f;
        for (int i = 0; i < n; ++i) {
            const float d = std::fabs(xOf(g.stops[i].t) - mp.x);
            if (d < best) { best = d; g_gradGrab = i; g_gradSel = i; }
        }
    }
    // Drag the grabbed stop, clamped between neighbours so the order never changes.
    if (ImGui::IsItemActive() && g_gradGrab >= 0 && g_gradGrab < n) {
        const float lo = g_gradGrab > 0 ? g.stops[g_gradGrab - 1].t + 1e-3f : 0.0f;
        const float hi = g_gradGrab < n - 1 ? g.stops[g_gradGrab + 1].t - 1e-3f : 1.0f;
        const float nt = glm::clamp(tOf(mp.x), lo, hi);
        if (nt != g.stops[g_gradGrab].t) { g.stops[g_gradGrab].t = nt; changed = true; }
    }
    // Double-click empty bar to add a stop with the sampled colour.
    if (hov && ImGui::IsMouseDoubleClicked(0) && g_gradGrab < 0) {
        const float t = tOf(mp.x);
        VfxGradient::Stop s{t, g.stops.empty() ? glm::vec4(1.0f) : g.Eval(t)};
        auto it = std::lower_bound(g.stops.begin(), g.stops.end(), t,
                                   [](const VfxGradient::Stop& a, f32 tt) { return a.t < tt; });
        g_gradSel = static_cast<int>(it - g.stops.begin());
        g.stops.insert(it, s);
        changed = true;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Double-click the bar to add a stop.");

    // Selected-stop editor.
    if (g_gradSel >= 0 && g_gradSel < static_cast<int>(g.stops.size())) {
        VfxGradient::Stop& s = g.stops[g_gradSel];
        if (ImGui::ColorEdit4("Stop color", glm::value_ptr(s.color),
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_HDR))
            changed = true;
        if (ImGui::SliderFloat("Stop pos", &s.t, 0.0f, 1.0f, "%.3f")) {
            const int i = g_gradSel, m = static_cast<int>(g.stops.size());
            const float lo = i > 0 ? g.stops[i - 1].t + 1e-3f : 0.0f;
            const float hi = i < m - 1 ? g.stops[i + 1].t - 1e-3f : 1.0f;
            s.t = glm::clamp(s.t, lo, hi);
            changed = true;
        }
        ImGui::BeginDisabled(g.stops.size() <= 1);
        if (ImGui::SmallButton("Remove stop")) {
            g.stops.erase(g.stops.begin() + g_gradSel);
            g_gradSel = g_gradGrab = -1;
            changed = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return changed;
}

// A 2D curve graph over t in [0,1], value in [vmin,vmax]. Draggable keys; double-click to add, right-
// click a key to remove. Keys keep their order (t clamped between neighbours). Returns true on edit.
bool CurveEditor(const char* id, VfxCurve& c, f32 vmin, f32 vmax) {
    bool changed = false;
    ImGui::PushID(id);
    if (g_curveOwner != &c) { g_curveSel = g_curveGrab = -1; g_curveOwner = &c; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float W = ImGui::CalcItemWidth(), H = 110.0f;
    ImGui::InvisibleButton("##curve", ImVec2(W, H));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const f32 span = glm::max(vmax - vmin, 1e-4f);
    const auto sx = [&](f32 t) { return p0.x + glm::clamp(t, 0.0f, 1.0f) * W; };
    const auto sy = [&](f32 v) { return p0.y + H - glm::clamp((v - vmin) / span, 0.0f, 1.0f) * H; };
    const auto tOf = [&](f32 x) { return glm::clamp((x - p0.x) / glm::max(W, 1.0f), 0.0f, 1.0f); };
    const auto vOf = [&](f32 y) { return glm::clamp(vmin + (1.0f - (y - p0.y) / H) * span, vmin, vmax); };

    // Background + grid.
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(28, 28, 32, 255));
    for (int i = 1; i < 4; ++i) {
        const float gx = p0.x + W * (i / 4.0f), gy = p0.y + H * (i / 4.0f);
        dl->AddLine(ImVec2(gx, p0.y), ImVec2(gx, p0.y + H), IM_COL32(60, 60, 66, 255));
        dl->AddLine(ImVec2(p0.x, gy), ImVec2(p0.x + W, gy), IM_COL32(60, 60, 66, 255));
    }
    dl->AddRect(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(0, 0, 0, 200));
    // Curve polyline (sampled through Eval so it reflects the real interpolation).
    if (!c.keys.empty()) {
        const int kS = 64;
        ImVec2 prev(sx(0.0f), sy(c.Eval(0.0f)));
        for (int i = 1; i <= kS; ++i) {
            const f32 t = i / static_cast<f32>(kS);
            const ImVec2 cur(sx(t), sy(c.Eval(t)));
            dl->AddLine(prev, cur, IM_COL32(120, 200, 255, 255), 2.0f);
            prev = cur;
        }
    }
    // Key handles.
    const int n = static_cast<int>(c.keys.size());
    for (int i = 0; i < n; ++i) {
        const ImVec2 kp(sx(c.keys[i].t), sy(c.keys[i].v));
        dl->AddCircleFilled(kp, 4.0f,
                            i == g_curveSel ? IM_COL32(255, 230, 0, 255) : IM_COL32(240, 240, 240, 255));
        dl->AddCircle(kp, 4.0f, IM_COL32(0, 0, 0, 200));
    }
    // Grab nearest key on press.
    if (ImGui::IsItemActivated()) {
        g_curveGrab = -1;
        float best = 10.0f;
        for (int i = 0; i < n; ++i) {
            const float dx = sx(c.keys[i].t) - mp.x, dy = sy(c.keys[i].v) - mp.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < best) { best = d; g_curveGrab = i; g_curveSel = i; }
        }
    }
    // Drag the grabbed key (t clamped between neighbours, v free within range).
    if (ImGui::IsItemActive() && g_curveGrab >= 0 && g_curveGrab < n) {
        const float lo = g_curveGrab > 0 ? c.keys[g_curveGrab - 1].t + 1e-3f : 0.0f;
        const float hi = g_curveGrab < n - 1 ? c.keys[g_curveGrab + 1].t - 1e-3f : 1.0f;
        c.keys[g_curveGrab].t = glm::clamp(tOf(mp.x), lo, hi);
        c.keys[g_curveGrab].v = vOf(mp.y);
        changed = true;
    }
    // Double-click to add a key on the curve; right-click a key to remove.
    if (hov && ImGui::IsMouseDoubleClicked(0) && g_curveGrab < 0) {
        const f32 t = tOf(mp.x);
        auto it = std::lower_bound(c.keys.begin(), c.keys.end(), t,
                                   [](const VfxCurve::Key& a, f32 tt) { return a.t < tt; });
        g_curveSel = static_cast<int>(it - c.keys.begin());
        c.keys.insert(it, {t, vOf(mp.y)});
        changed = true;
    }
    if (hov && ImGui::IsMouseClicked(1)) {
        for (int i = 0; i < n; ++i) {
            const float dx = sx(c.keys[i].t) - mp.x, dy = sy(c.keys[i].v) - mp.y;
            if (std::sqrt(dx * dx + dy * dy) < 8.0f && c.keys.size() > 1) {
                c.keys.erase(c.keys.begin() + i);
                g_curveSel = g_curveGrab = -1;
                changed = true;
                break;
            }
        }
    }
    ImGui::TextDisabled("Double-click to add a key, right-click a key to remove. Range %.2f..%.2f",
                        vmin, vmax);
    if (g_curveSel >= 0 && g_curveSel < static_cast<int>(c.keys.size())) {
        VfxCurve::Key& k = c.keys[g_curveSel];
        if (ImGui::DragFloat("Key value", &k.v, 0.01f, vmin, vmax)) changed = true;
    }
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------
// Panel styling: a designed DCC-tool look rather than default ImGui
// ---------------------------------------------------------------------------

// Category accent per section, so the panel is scannable at a glance.
const ImVec4 kEmit(0.36f, 0.55f, 0.95f, 1.0f);   // blue
const ImVec4 kShape(0.35f, 0.78f, 0.52f, 1.0f);  // green
const ImVec4 kLife(0.30f, 0.72f, 0.72f, 1.0f);   // teal
const ImVec4 kMotion(0.95f, 0.62f, 0.30f, 1.0f); // orange
const ImVec4 kColorC(0.92f, 0.42f, 0.68f, 1.0f); // pink
const ImVec4 kSize(0.40f, 0.72f, 0.90f, 1.0f);   // cyan
const ImVec4 kRot(0.66f, 0.50f, 0.92f, 1.0f);    // violet
const ImVec4 kRender(0.90f, 0.78f, 0.35f, 1.0f); // amber
const ImVec4 kSub(0.90f, 0.42f, 0.42f, 1.0f);    // red
const ImVec4 kMuted(0.55f, 0.55f, 0.62f, 1.0f);  // gray (perf / effekseer)
const ImVec4 kAccent(0.55f, 0.48f, 1.0f, 1.0f);  // panel accent (violet)

// Scoped panel theme (rounded frames, coherent spacing, violet accent). PUSH after Begin(), POP
// before End() - NOT via RAII: an RAII destructor runs after ImGui::End(), which leaves the style
// stack unbalanced at End() ("Missing PopStyleVar/Color") and leaks the pops onto the next window.
struct ThemeCounts { int colors = 0, vars = 0; };
ThemeCounts PushParticleTheme() {
    ThemeCounts t;
    const auto col = [&](ImGuiCol c, ImVec4 v) { ImGui::PushStyleColor(c, v); ++t.colors; };
    const auto var1 = [&](ImGuiStyleVar v, float x) { ImGui::PushStyleVar(v, x); ++t.vars; };
    const auto var2 = [&](ImGuiStyleVar v, ImVec2 x) { ImGui::PushStyleVar(v, x); ++t.vars; };
    col(ImGuiCol_FrameBg, ImVec4(0.11f, 0.11f, 0.14f, 1.0f));
    col(ImGuiCol_FrameBgHovered, ImVec4(0.17f, 0.17f, 0.22f, 1.0f));
    col(ImGuiCol_FrameBgActive, ImVec4(0.21f, 0.21f, 0.28f, 1.0f));
    col(ImGuiCol_Button, ImVec4(0.19f, 0.19f, 0.25f, 1.0f));
    col(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.27f, 0.55f, 1.0f));
    col(ImGuiCol_ButtonActive, kAccent);
    col(ImGuiCol_SliderGrab, kAccent);
    col(ImGuiCol_SliderGrabActive, ImVec4(0.68f, 0.62f, 1.0f, 1.0f));
    col(ImGuiCol_CheckMark, kAccent);
    col(ImGuiCol_Separator, ImVec4(0.26f, 0.26f, 0.32f, 1.0f));
    col(ImGuiCol_Header, ImVec4(0.22f, 0.22f, 0.30f, 1.0f));
    col(ImGuiCol_HeaderHovered, ImVec4(0.28f, 0.28f, 0.38f, 1.0f));
    col(ImGuiCol_HeaderActive, ImVec4(0.32f, 0.32f, 0.44f, 1.0f));
    var1(ImGuiStyleVar_FrameRounding, 4.0f);
    var1(ImGuiStyleVar_GrabRounding, 3.0f);
    var1(ImGuiStyleVar_ChildRounding, 5.0f);
    var1(ImGuiStyleVar_TabRounding, 4.0f);
    var2(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    var2(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
    var2(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f, 4.0f));
    var2(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 3.0f));
    return t;
}
void PopParticleTheme(const ThemeCounts& t) {
    ImGui::PopStyleVar(t.vars);
    ImGui::PopStyleColor(t.colors);
}

// A title/stat header: accent stripe, effect name, and a live-particle-count chip.
void HeaderBar(const char* name, const ParticleEmitter& pe) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 42.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(28, 28, 38, 255), 6.0f);
    dl->AddRectFilled(p, ImVec2(p.x + 5.0f, p.y + h), IM_COL32(140, 122, 255, 255), 6.0f);
    dl->AddText(ImVec2(p.x + 16.0f, p.y + 8.0f), IM_COL32(238, 238, 248, 255), name);
    dl->AddText(ImVec2(p.x + 16.0f, p.y + 24.0f), IM_COL32(140, 140, 158, 255), "Particle Effect");
    char chip[48];
    std::snprintf(chip, sizeof(chip), "%u live", pe.pool.count);
    const ImVec2 ts = ImGui::CalcTextSize(chip);
    const ImVec2 c0(p.x + w - ts.x - 26.0f, p.y + (h - ts.y - 8.0f) * 0.5f);
    dl->AddRectFilled(c0, ImVec2(c0.x + ts.x + 16.0f, c0.y + ts.y + 8.0f), IM_COL32(46, 46, 60, 255),
                      10.0f);
    dl->AddText(ImVec2(c0.x + 8.0f, c0.y + 4.0f), IM_COL32(150, 132, 255, 255), chip);
    ImGui::Dummy(ImVec2(w, h + 2.0f));
}

// Colour-accented collapsing section header.
bool SectionC(const char* label, const ImVec4& c) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(c.x, c.y, c.z, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x, c.y, c.z, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(c.x, c.y, c.z, 0.86f));
    const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(3);
    return open;
}

// Two-column "label | control" rows - the aligned layout that reads as a real tool. BeginParams()
// opens the table; each Row* draws one row (left label, full-width control); EndParams() closes it.
bool BeginParams(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_PadOuterX)) return false;
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, 132.0f);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void EndParams() { ImGui::EndTable(); }
void RowLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}
bool RowDrag(const char* l, f32* v, f32 sp, f32 mn, f32 mx, const char* fmt = "%.3f") {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::DragFloat("##v", v, sp, mn, mx, fmt);
    ImGui::PopID();
    return c;
}
bool RowDrag3(const char* l, f32* v, f32 sp, f32 mn = 0.0f, f32 mx = 0.0f) {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::DragFloat3("##v", v, sp, mn, mx);
    ImGui::PopID();
    return c;
}
bool RowSlider(const char* l, f32* v, f32 mn, f32 mx, const char* fmt = "%.2f") {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::SliderFloat("##v", v, mn, mx, fmt);
    ImGui::PopID();
    return c;
}
bool RowDragInt(const char* l, int* v, f32 sp, int mn, int mx) {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::DragInt("##v", v, sp, mn, mx);
    ImGui::PopID();
    return c;
}
bool RowCombo(const char* l, int* v, const char* items) {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::Combo("##v", v, items);
    ImGui::PopID();
    return c;
}
bool RowColor(const char* l, f32* v) {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::ColorEdit4("##v", v,
                                     ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_HDR |
                                         ImGuiColorEditFlags_NoInputs);
    ImGui::PopID();
    return c;
}
bool RowCheck(const char* l, bool* v) {
    RowLabel(l);
    ImGui::PushID(l);
    const bool c = ImGui::Checkbox("##v", v);
    ImGui::PopID();
    return c;
}

} // namespace

void Editor::DrawParticleEditor(Engine& engine) {
    Scene& scene = engine.GetScene();
    entt::registry& reg = scene.Registry();

    // --- Preview lifecycle ---------------------------------------------------
    const bool want = panelOpen_[Panel_ParticleEditor] && !playMode_;
    const bool alive = reg.valid(g_pdoc.preview) && reg.all_of<ParticleEmitter>(g_pdoc.preview);
    if (!want) {
        // Tear the preview down, stashing the authored fields first.
        if (alive) {
            g_pdoc.authored = AuthoredCopy(reg.get<ParticleEmitter>(g_pdoc.preview));
            g_pdoc.haveAuthored = true;
        }
        if (reg.valid(g_pdoc.preview)) reg.destroy(g_pdoc.preview);
        g_pdoc.preview = entt::null;
        return;
    }
    if (!alive) {
        // (Re)create the preview emitter from the stash (or a default effect on first open).
        if (reg.valid(g_pdoc.preview)) reg.destroy(g_pdoc.preview);
        g_pdoc.preview = scene.CreateEntity("Particle Preview");
        Transform tf;
        tf.position = g_pdoc.previewPos;
        reg.emplace<Transform>(g_pdoc.preview, tf);
        reg.emplace<ParticleEmitter>(g_pdoc.preview,
                                     g_pdoc.haveAuthored ? g_pdoc.authored : DefaultEffect());
        reg.emplace<ParticlePreviewTag>(g_pdoc.preview);
        // Auto-frame the viewport camera on the effect the first time the panel is opened, so the
        // user immediately sees it instead of hunting for the preview origin.
        static bool s_framedOnce = false;
        if (!s_framedOnce) {
            engine.GetRenderer().FocusOn(g_pdoc.previewPos, 3.0f);
            s_framedOnce = true;
        }
    }

    if (!ImGui::Begin("Particle Editor", &panelOpen_[Panel_ParticleEditor])) {
        ImGui::End();
        return;
    }
    ParticleEmitter& pe = reg.get<ParticleEmitter>(g_pdoc.preview);
    const ThemeCounts theme = PushParticleTheme();

    // Header: effect name + live-particle chip.
    const std::string title = g_pdoc.path.empty()
                                  ? std::string("Untitled Effect")
                                  : std::filesystem::path(g_pdoc.path).stem().string();
    HeaderBar(title.c_str(), pe);
    ImGui::Spacing();

    // --- Toolbar -------------------------------------------------------------
    if (ImGui::Button("New")) {
        ApplyAuthoredInPlace(pe, DefaultEffect());
        g_pdoc.path.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset")) ImGui::OpenPopup("##pfxpreset");
    if (ImGui::BeginPopup("##pfxpreset")) {
        for (u32 i = 0; i < static_cast<u32>(particle::Template::Count); ++i)
            if (ImGui::Selectable(particle::TemplateName(static_cast<particle::Template>(i)))) {
                ApplyAuthoredInPlace(pe, particle::MakeTemplate(static_cast<particle::Template>(i)));
                g_pdoc.path.clear();
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
        std::string pick;
        if (AssetPickerPublic("Open", g_pdoc.path, particle::kEffectExtension, pick) &&
            !pick.empty() && Project::HasActive()) {
            if (auto loaded = particle::LoadEffect(Project::Active().AssetsDir() / pick)) {
                ApplyAuthoredInPlace(pe, std::move(*loaded));
                g_pdoc.path = pick;
            } else {
                HBE_WARN("Particle Editor: failed to load '{}'.", pick);
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!Project::HasActive());
    if (ImGui::Button("Save")) {
        const std::filesystem::path dir = Project::Active().AssetsDir() / "effects";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string rel = g_pdoc.path;
        if (rel.empty()) rel = std::string("effects/effect") + particle::kEffectExtension;
        if (particle::SaveEffect(Project::Active().AssetsDir() / rel, pe)) {
            g_pdoc.path = rel;
            HBE_INFO("Saved effect -> {}", rel);
        }
    }
    ImGui::EndDisabled();

    // Transport: a coloured Emit toggle (green = live) + Restart.
    ImGui::PushStyleColor(ImGuiCol_Button, pe.emitting ? ImVec4(0.18f, 0.52f, 0.30f, 1.0f)
                                                       : ImVec4(0.42f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pe.emitting ? ImVec4(0.24f, 0.62f, 0.38f, 1.0f)
                                                             : ImVec4(0.52f, 0.28f, 0.28f, 1.0f));
    if (ImGui::Button(pe.emitting ? "  Emitting  " : "  Paused  ")) pe.emitting = !pe.emitting;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        pe.state.emitterAge = 0.0f;
        pe.state.burstFired = false;
        pe.state.wasEmitting = false;
        pe.pool.Clear();
        pe.emitting = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.58f, 0.58f, 0.66f, 1.0f), "%s",
                       g_pdoc.path.empty() ? "unsaved" : g_pdoc.path.c_str());

    ImGui::Spacing();
    if (BeginParams("##pfx_prev")) {
        if (RowDrag3("Preview pos", glm::value_ptr(g_pdoc.previewPos), 0.05f))
            if (Transform* tf = reg.try_get<Transform>(g_pdoc.preview)) tf->position = g_pdoc.previewPos;
        EndParams();
    }
    if (ImGui::Button("Frame preview")) engine.GetRenderer().FocusOn(g_pdoc.previewPos, 3.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("Snap the viewport camera to the effect.");
    ImGui::Spacing();

    // --- Emission ------------------------------------------------------------
    if (SectionC("Emission", kEmit)) {
        if (BeginParams("##emit")) {
            RowDrag("Rate  (/s)", &pe.rate, 0.5f, 0.0f, 100000.0f, "%.1f");
            int mp = static_cast<int>(pe.maxParticles);
            if (RowDragInt("Max particles", &mp, 4.0f, 1, 200000))
                pe.maxParticles = static_cast<u32>(mp < 1 ? 1 : mp);
            int burst = static_cast<int>(pe.burst);
            if (RowDragInt("Burst", &burst, 1.0f, 0, 100000))
                pe.burst = static_cast<u32>(burst < 0 ? 0 : burst);
            RowCheck("Loop", &pe.loop);
            if (!pe.loop) RowDrag("Duration (s)", &pe.duration, 0.05f, 0.0f, 600.0f, "%.2f");
            EndParams();
        }
    }

    // --- Shape ---------------------------------------------------------------
    if (SectionC("Emission Shape", kShape)) {
        if (BeginParams("##shape")) {
            int shape = static_cast<int>(pe.shape);
            if (RowCombo("Shape", &shape, "Point\0Sphere\0Hemisphere\0Box\0Disc\0Cone\0"))
                pe.shape = static_cast<ParticleEmitter::Shape>(shape);
            switch (pe.shape) {
                case ParticleEmitter::Shape::Sphere:
                case ParticleEmitter::Shape::Hemisphere:
                case ParticleEmitter::Shape::Disc:
                    RowDrag("Radius", &pe.emitRadius, 0.02f, 0.0f, 1000.0f, "%.2f");
                    break;
                case ParticleEmitter::Shape::Box:
                    RowDrag3("Box half-extents", glm::value_ptr(pe.boxHalfExtents), 0.02f, 0.0f, 1000.0f);
                    break;
                case ParticleEmitter::Shape::Cone:
                    RowDrag("Cone angle (deg)", &pe.coneAngle, 0.5f, 0.0f, 180.0f, "%.1f");
                    RowDrag("Radius", &pe.emitRadius, 0.02f, 0.0f, 1000.0f, "%.2f");
                    break;
                case ParticleEmitter::Shape::Point:
                    break;
            }
            RowDrag3("Direction", glm::value_ptr(pe.direction), 0.01f, -1.0f, 1.0f);
            RowSlider("Spread", &pe.spread, 0.0f, 1.0f);
            RowDrag("Start speed", &pe.startSpeed, 0.02f, 0.0f, 1000.0f, "%.2f");
            RowSlider("Speed variance", &pe.speedVariance, 0.0f, 1.0f);
            EndParams();
        }
    }

    // --- Lifetime ------------------------------------------------------------
    if (SectionC("Lifetime", kLife)) {
        if (BeginParams("##life")) {
            RowDrag("Lifetime (s)", &pe.lifetime, 0.02f, 0.01f, 600.0f, "%.2f");
            RowSlider("Variance", &pe.lifetimeVariance, 0.0f, 1.0f);
            EndParams();
        }
    }

    // --- Motion / Forces -----------------------------------------------------
    if (SectionC("Motion & Forces", kMotion)) {
        if (BeginParams("##motion")) {
            RowDrag3("Gravity", glm::value_ptr(pe.gravity), 0.02f);
            RowDrag("Drag", &pe.drag, 0.01f, 0.0f, 100.0f, "%.2f");
            RowCheck("Exact (exp) drag", &pe.expDrag);
            RowDrag("Buoyancy", &pe.buoyancy, 0.02f, 0.0f, 100.0f, "%.2f");
            RowDrag("Vortex", &pe.vortex, 0.02f, 0.0f, 100.0f, "%.2f");
            RowDrag("Turbulence", &pe.turbulence, 0.02f, 0.0f, 100.0f, "%.2f");
            if (pe.turbulence > 0.0f)
                RowDrag("Turbulence scale", &pe.turbulenceScale, 0.02f, 0.01f, 100.0f, "%.2f");
            RowCheck("Curl noise", &pe.useCurlNoise);
            if (pe.useCurlNoise) {
                RowDrag("Curl strength", &pe.curlStrength, 0.02f, 0.0f, 100.0f, "%.2f");
                RowDrag("Curl frequency", &pe.curlFrequency, 0.02f, 0.01f, 100.0f, "%.2f");
            }
            EndParams();
        }
    }

    // --- Color ---------------------------------------------------------------
    if (SectionC("Color", kColorC)) {
        bool seed = false;
        if (BeginParams("##coltop")) {
            // Toggling the gradient on seeds it from the current start/end so nothing jumps.
            if (RowCheck("Gradient over life", &pe.useColorCurve) && pe.useColorCurve &&
                pe.colorCurve.stops.empty())
                seed = true;
            EndParams();
        }
        if (seed) {
            pe.colorCurve.stops.push_back({0.0f, pe.startColor});
            pe.colorCurve.stops.push_back({1.0f, pe.endColor});
        }
        if (pe.useColorCurve) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            GradientEditor("colorCurve", pe.colorCurve);
        } else if (BeginParams("##colse")) {
            RowColor("Start color", glm::value_ptr(pe.startColor));
            RowColor("End color", glm::value_ptr(pe.endColor));
            EndParams();
        }
        if (BeginParams("##colb")) {
            RowCheck("Additive (fire/sparks)", &pe.additive);
            RowCheck("Per-particle variance", &pe.simulateColor);
            if (pe.simulateColor) RowSlider("Color variance", &pe.colorVariance, 0.0f, 1.0f);
            EndParams();
        }
        if (pe.simulateColor && pe.useColorCurve)
            ImGui::TextDisabled("Per-particle variance overrides the gradient at runtime.");
    }

    // --- Size ----------------------------------------------------------------
    if (SectionC("Size", kSize)) {
        bool seed = false;
        if (BeginParams("##sztop")) {
            if (RowCheck("Curve over life", &pe.useSizeCurve) && pe.useSizeCurve &&
                pe.sizeCurve.keys.empty())
                seed = true;
            EndParams();
        }
        if (seed) {
            pe.sizeCurve.keys.push_back({0.0f, pe.startSize});
            pe.sizeCurve.keys.push_back({1.0f, pe.endSize});
        }
        if (pe.useSizeCurve) {
            static f32 sizeCurveMax = 2.0f;
            for (const VfxCurve::Key& k : pe.sizeCurve.keys)
                sizeCurveMax = glm::max(sizeCurveMax, k.v * 1.1f);
            if (BeginParams("##szmax")) {
                RowDrag("Graph max", &sizeCurveMax, 0.05f, 0.1f, 100.0f, "%.2f");
                EndParams();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            CurveEditor("sizeCurve", pe.sizeCurve, 0.0f, sizeCurveMax);
        } else if (BeginParams("##szse")) {
            RowDrag("Start size", &pe.startSize, 0.005f, 0.0f, 100.0f, "%.3f");
            RowDrag("End size", &pe.endSize, 0.005f, 0.0f, 100.0f, "%.3f");
            EndParams();
        }
        if (BeginParams("##szb")) {
            RowCheck("Per-particle variance", &pe.simulateSize);
            if (pe.simulateSize) RowSlider("Size variance", &pe.sizeVariance, 0.0f, 1.0f);
            EndParams();
        }
    }

    // --- Rotation ------------------------------------------------------------
    if (SectionC("Rotation", kRot)) {
        if (BeginParams("##rot")) {
            RowDrag("Spin (rad/s)", &pe.spin, 0.02f, -100.0f, 100.0f, "%.2f");
            EndParams();
        }
    }

    // --- Rendering -----------------------------------------------------------
    if (SectionC("Rendering", kRender)) {
        if (BeginParams("##rend")) {
            int rm = static_cast<int>(pe.render);
            if (RowCombo("Mode", &rm, "Billboard\0Stretched\0Horizontal\0Ribbon\0Mesh\0"))
                pe.render = static_cast<ParticleEmitter::Render>(rm);
            if (pe.render == ParticleEmitter::Render::Stretched)
                RowDrag("Stretch", &pe.stretch, 0.02f, 1.0f, 50.0f, "%.2f");
            int cols = static_cast<int>(pe.subUVCols), rows = static_cast<int>(pe.subUVRows);
            if (RowDragInt("Sheet cols", &cols, 1.0f, 1, 32))
                pe.subUVCols = static_cast<u32>(cols < 1 ? 1 : cols);
            if (RowDragInt("Sheet rows", &rows, 1.0f, 1, 32))
                pe.subUVRows = static_cast<u32>(rows < 1 ? 1 : rows);
            if (pe.subUVCols > 1 || pe.subUVRows > 1)
                RowDrag("Sheet FPS", &pe.subUVFps, 0.5f, 0.0f, 240.0f, "%.1f");
            RowSlider("Fade in", &pe.fadeIn, 0.0f, 1.0f);
            RowSlider("Fade out", &pe.fadeOut, 0.0f, 1.0f);
            RowDrag("Soft fade (depth)", &pe.softFade, 0.02f, 0.0f, 10.0f, "%.2f");
            EndParams();
        }
        if (pe.render == ParticleEmitter::Render::Ribbon)
            ImGui::TextDisabled("Ribbon connects particles into one trail. Best with Point shape on a "
                                "moving emitter; size controls ribbon width.");
        if (pe.render == ParticleEmitter::Render::Mesh) {
            std::string mpick;
            if (AssetPickerPublic("Mesh (.uaf)", pe.particleMesh, ".uaf", mpick)) {
                pe.particleMesh = mpick;
                pe.meshResolved = false;
            }
            ImGui::TextDisabled("Draws this mesh once per particle (size = scale, colour tints it). "
                                "Uniform colour instances into one draw; opaque.");
        } else {
            std::string tpick;
            if (AssetPickerPublic("Sprite (.uaf)", pe.texture, ".uaf", tpick)) {
                pe.texture = tpick; // "" clears back to the procedural soft dot
                pe.textureResolved = false;
            }
            if (!pe.texture.empty() && ImGui::SmallButton("Clear sprite")) {
                pe.texture.clear();
                pe.textureResolved = false;
            }
        }
    }

    // --- Sub-emitter (spawn a child effect on particle death) ----------------
    if (SectionC("Sub-emitter (on death)", kSub)) {
        std::string child;
        if (AssetPickerPublic("On-death effect (.hbvfx)", pe.onDeathEffect, particle::kEffectExtension,
                              child))
            pe.onDeathEffect = child;
        if (!pe.onDeathEffect.empty()) {
            if (ImGui::SmallButton("Clear on-death effect")) pe.onDeathEffect.clear();
            if (BeginParams("##sub")) {
                RowSlider("Spawn chance", &pe.onDeathChance, 0.0f, 1.0f);
                EndParams();
            }
            ImGui::TextDisabled("Spawns this child where each particle dies. Fires in Play mode; "
                                "the parent previews live.");
        } else {
            ImGui::TextDisabled("Spawn a child effect at each particle's death position "
                                "(fireworks, layered explosions).");
        }
    }

    // --- Performance (opt-in GPU paths) --------------------------------------
    if (SectionC("Performance", kMuted)) {
        if (BeginParams("##perf")) {
            RowCheck("GPU expand", &pe.gpuExpand);
            RowCheck("GPU simulate", &pe.gpuSim);
            EndParams();
        }
        if (pe.gpuSim)
            ImGui::TextDisabled("GPU sim uses the v1 module stack: sphere spawn + curl noise; "
                                "buoyancy/vortex and the non-sphere shapes are dropped.");
    }

    // --- Effekseer escape hatch ----------------------------------------------
    if (SectionC("Effekseer effect (advanced)", kMuted)) {
        std::string efk;
        if (AssetPickerPublic("Effekseer (.efkefc)", pe.effekseerEffect, ".efkefc", efk))
            pe.effekseerEffect = efk;
        if (!pe.effekseerEffect.empty()) {
            if (ImGui::SmallButton("Clear Effekseer link")) pe.effekseerEffect.clear();
            ImGui::TextDisabled("When set, SpawnEffect plays this .efkefc through the Effekseer "
                                "runtime and the native fields above are ignored at runtime.");
        }
    }

    PopParticleTheme(theme); // balance the style stack BEFORE End()
    ImGui::End();
}

} // namespace hbe

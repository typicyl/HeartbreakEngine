// UI/UIData.cpp - P9.4 data-binding model + resolve step + headless self-test.
#include "UI/UIData.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/UISystem.h" // UIContext

#include <cstdio>

namespace hbe::ui {

void UIDataModel::Set(std::string_view key, Value v) {
    const std::string k(key);
    auto it = vals_.find(k);
    if (it != vals_.end() && it->second == v) return; // unchanged -> no rev bump
    vals_[k] = std::move(v);
    revs_[k] = ++globalRev_;
}

const Value* UIDataModel::Get(std::string_view key) const {
    const auto it = vals_.find(std::string(key));
    return it == vals_.end() ? nullptr : &it->second;
}

u64 UIDataModel::Rev(std::string_view key) const {
    const auto it = revs_.find(std::string(key));
    return it == revs_.end() ? 0u : it->second;
}

void UIDataModel::Provide(std::string key, Provider p) {
    providers_[std::move(key)] = std::move(p);
}

void UIDataModel::RefreshProviders() {
    for (const auto& [k, p] : providers_)
        if (p) Set(k, p());
}

void UIDataModel::Clear() {
    vals_.clear();
    revs_.clear();
    ++globalRev_; // force the next ResolveBindings to re-walk
}

u32 ResolveBindings(Scene& scene, const UIDataModel& model, UIContext& ctx) {
    if (ctx.lastBindGlobalRev == model.GlobalRev()) return 0; // nothing changed globally
    ctx.lastBindGlobalRev = model.GlobalRev();
    auto& reg = scene.Registry();
    u32 writes = 0;
    for (const entt::entity e : reg.view<UIElement>()) {
        UIElement& el = reg.get<UIElement>(e);
        if (!el.bindText.empty()) {
            if (const Value* v = model.Get(el.bindText)) {
                std::string s;
                if (v->t == Value::T::Number) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%g", v->num);
                    s = b;
                } else {
                    s = v->str;
                }
                if (el.runtimeText != s) {
                    el.runtimeText = std::move(s);
                    ++writes;
                }
            }
        }
        if (!el.bindValue.empty()) {
            if (const Value* v = model.Get(el.bindValue)) {
                const f32 n = static_cast<f32>(v->num);
                if (el.value != n) {
                    el.value = n;
                    ++writes;
                }
                if (el.fill != n) {
                    el.fill = n;
                    ++writes;
                }
            }
        }
        if (!el.bindVisible.empty()) {
            if (const Value* v = model.Get(el.bindVisible)) {
                const bool vis =
                    (v->t == Value::T::Text) ? !v->str.empty() : (v->num != 0.0);
                if (el.visible != vis) {
                    el.visible = vis;
                    ++writes;
                }
            }
        }
        if (!el.bindTexture.empty()) {
            if (const Value* v = model.Get(el.bindTexture)) {
                if (el.texture != v->str) {
                    el.texture = v->str;
                    el.textureResolved = false; // force ResolveTexture to re-resolve
                    ++writes;
                }
            }
        }
    }
    return writes;
}

bool DataBindSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool c, const char* what) {
        if (!c) {
            ok = false;
            HBE_ERROR("uibind: FAILED - {}", what);
        }
    };

    Scene scene;
    auto& reg = scene.Registry();
    const entt::entity e = scene.CreateEntity("bindtest");
    {
        UIElement el;
        el.bindText = "hp";
        el.bindValue = "frac";
        el.bindVisible = "shown";
        el.bindTexture = "icon";
        reg.emplace<UIElement>(e, el);
    }

    UIDataModel model;
    UIContext ctx;

    expect(ResolveBindings(scene, model, ctx) == 0, "no bound keys set -> zero writes");

    // Values chosen to differ from the UIElement defaults (value=0.5, fill=0.65,
    // visible=true, text="") so every bound field actually writes on the first resolve.
    model.Set("hp", Value::Str("100/100"));
    model.Set("frac", Value::Num(0.25));
    model.Set("shown", Value::Boolean(false));
    model.Set("icon", Value::TexPath("hp.png"));
    const u32 w1 = ResolveBindings(scene, model, ctx);
    {
        const UIElement& el = reg.get<UIElement>(e);
        expect(el.runtimeText == "100/100", "bindText -> runtimeText");
        expect(el.value == 0.25f && el.fill == 0.25f, "bindValue -> value + fill");
        expect(!el.visible, "bindVisible -> visible");
        expect(el.texture == "hp.png", "bindTexture -> texture");
        expect(w1 == 5, "first resolve wrote text + value + fill + visible + texture");
    }

    expect(ResolveBindings(scene, model, ctx) == 0,
           "unchanged model -> zero writes (globalRev fast-skip)");

    model.Set("frac", Value::Num(0.25)); // identical to the current model value
    expect(ResolveBindings(scene, model, ctx) == 0,
           "Set with identical value -> no rev bump -> zero writes");

    model.Set("frac", Value::Num(0.75));
    const u32 w4 = ResolveBindings(scene, model, ctx);
    expect(reg.get<UIElement>(e).value == 0.75f, "changed value applied");
    expect(w4 == 2, "only value + fill rewrote (one binding, two fields)");

    model.Set("hp", Value::Num(42));
    ResolveBindings(scene, model, ctx);
    expect(reg.get<UIElement>(e).runtimeText == "42", "number bound to text formats");

    return ok;
}

} // namespace hbe::ui

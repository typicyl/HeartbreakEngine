// UI/UIManager.cpp
#include "UI/UIManager.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace hbe::ui {
namespace {
// True when `root` is `e` or a (transitive) ancestor of `e`.
bool IsUnder(entt::registry& reg, entt::entity e, entt::entity root) {
    int depth = 0;
    for (entt::entity cur = e; cur != entt::null && depth < 64; ++depth) {
        if (cur == root) return true;
        const Parent* p = reg.try_get<Parent>(cur);
        cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
    }
    return false;
}

entt::entity FindPanel(entt::registry& reg, const std::string& name) {
    for (const entt::entity e : reg.view<UIPanel>())
        if (reg.get<UIPanel>(e).name == name) return e;
    return entt::null;
}
} // namespace

void UIManager::Activate(Scene& scene, const std::string& name, bool on) {
    auto& reg = scene.Registry();
    const entt::entity root = FindPanel(reg, name);
    if (root == entt::null) return;
    UIPanel& panel = reg.get<UIPanel>(root);
    const bool was = panel.active;
    panel.active = on;
    // Direct field write - no EnTT signal fires; the laid-out element set just
    // changed, so bump the UI structure version explicitly (cache invalidation).
    if (was != on) scene.BumpUIVersion();
    if (on && !was) {
        // Restart the panel subtree's OnShow animators to play the entrance transition.
        for (const entt::entity e : reg.view<UIAnimator>()) {
            UIAnimator& an = reg.get<UIAnimator>(e);
            if (an.trigger == UIAnimator::Trigger::OnShow && IsUnder(reg, e, root)) {
                an.time = 0.0f;
                an.playing = true;
                an.captured = false;
            }
        }
    }
}

void UIManager::Init(Scene& scene) {
    auto& reg = scene.Registry();
    stack_.clear();
    initial_.clear();
    for (const entt::entity e : reg.view<UIPanel>()) {
        UIPanel& p = reg.get<UIPanel>(e);
        p.active = false; // start hidden; the game flow shows the right panel
        if (p.startVisible && initial_.empty()) initial_ = p.name;
    }
}

void UIManager::ShowInitial(Scene& scene) {
    if (!initial_.empty()) Show(scene, initial_);
}

void UIManager::Show(Scene& scene, const std::string& name) {
    for (const std::string& s : stack_) Activate(scene, s, false);
    stack_.clear();
    Activate(scene, name, true);
    stack_.push_back(name);
}

void UIManager::Push(Scene& scene, const std::string& name) {
    if (!stack_.empty() && stack_.back() != name) Activate(scene, stack_.back(), false);
    Activate(scene, name, true);
    stack_.push_back(name);
}

void UIManager::Pop(Scene& scene) {
    if (stack_.empty()) return;
    Activate(scene, stack_.back(), false);
    stack_.pop_back();
    if (!stack_.empty()) Activate(scene, stack_.back(), true);
}

bool UIManager::Has(Scene& scene, const std::string& name) {
    return FindPanel(scene.Registry(), name) != entt::null;
}

} // namespace hbe::ui

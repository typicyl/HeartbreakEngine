// UI/UIManager.cpp
#include "UI/UIManager.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>

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

// A panel belongs to one of the bound screen documents. An EMPTY set means
// UNBOUND: fall back to the whole registry, which is what the editor and any
// pre-document caller see.
bool InDocs(entt::registry& reg, entt::entity e, const std::vector<u32>& docs) {
    if (docs.empty()) return true;
    const UIDocMember* m = reg.try_get<UIDocMember>(e);
    if (!m) return false;
    return std::find(docs.begin(), docs.end(), m->doc) != docs.end();
}

// Linear scan fallback. Walks the documents IN BIND ORDER rather than in EnTT
// pool order, so "first wins" on a duplicate panel name is the manifest's order
// and not an allocation accident.
entt::entity ScanPanel(entt::registry& reg, const std::string& name,
                       const std::vector<u32>& docs) {
    if (docs.empty()) {
        for (const entt::entity e : reg.view<UIPanel>())
            if (reg.get<UIPanel>(e).name == name) return e;
        return entt::null;
    }
    for (const u32 doc : docs) {
        for (const entt::entity e : reg.view<UIPanel>()) {
            const UIDocMember* m = reg.try_get<UIDocMember>(e);
            if (m && m->doc == doc && reg.get<UIPanel>(e).name == name) return e;
        }
    }
    return entt::null;
}
} // namespace

void UIManager::Bind(const std::vector<u32>& docs) {
    docs_ = docs;
    // Drop 0 (== "no document"): it would otherwise be a member of the set that
    // no UIDocMember can ever match, which reads as "bound to nothing" instead of
    // the UNBOUND-means-empty rule the lookups rely on.
    docs_.erase(std::remove(docs_.begin(), docs_.end(), 0u), docs_.end());
    panels_.clear();
}

void UIManager::Bind(u32 doc) {
    docs_.clear();
    if (doc != 0) docs_.push_back(doc);
    panels_.clear();
}

entt::entity UIManager::Find(Scene& scene, const std::string& name) {
    auto& reg = scene.Registry();
    if (const auto it = panels_.find(name); it != panels_.end()) {
        const entt::entity e = it->second;
        if (reg.valid(e) && reg.all_of<UIPanel>(e) && reg.get<UIPanel>(e).name == name &&
            InDocs(reg, e, docs_))
            return e;
        panels_.erase(name); // stale (destroyed / renamed / re-created) - re-scan
    }
    const entt::entity e = ScanPanel(reg, name, docs_);
    if (e != entt::null) panels_[name] = e;
    return e;
}

entt::entity UIManager::PanelEntity(Scene& scene, const std::string& name) {
    return Find(scene, name);
}

void UIManager::Activate(Scene& scene, const std::string& name, bool on) {
    auto& reg = scene.Registry();
    const entt::entity root = Find(scene, name);
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
    firstPanel_.clear();
    panels_.clear();

    // Walk the set IN BIND ORDER (the .hbproj manifest order), not EnTT pool
    // order. That is what makes "the first startVisible panel is the initial
    // screen" and "first wins on a duplicate name" deterministic once the four
    // screens are four separate files opened in a defined sequence.
    const auto visit = [&](u32 doc) {
        for (const entt::entity e : reg.view<UIPanel>()) {
            if (doc != 0) {
                const UIDocMember* m = reg.try_get<UIDocMember>(e);
                if (!m || m->doc != doc) continue;
            }
            UIPanel& p = reg.get<UIPanel>(e);
            p.active = false; // start hidden; the game flow shows the right panel
            if (p.name.empty()) continue;
            const auto [it, fresh] = panels_.emplace(p.name, e);
            if (!fresh) {
                // Not survivable silently: Show("Settings") would toggle one of
                // them and leave the other permanently authoritative-looking.
                HBE_ERROR("UI: duplicate panel name '{}' across resident screen "
                          "documents; the first one (in project order) wins.",
                          p.name);
                continue;
            }
            if (firstPanel_.empty()) firstPanel_ = p.name;
            if (p.startVisible && initial_.empty()) initial_ = p.name;
        }
    };
    if (docs_.empty())
        visit(0); // UNBOUND: whole registry, as before
    else
        for (const u32 doc : docs_) visit(doc);
}

void UIManager::ShowInitial(Scene& scene) {
    if (!initial_.empty()) Show(scene, initial_);
}

// A NAME THAT NAMES NOTHING NEVER ENTERS THE STACK. It used to: Show/Push pushed
// unconditionally, so a missing screen document (a rename, a stale .hbproj slot)
// left `Top()` reporting a screen that does not exist - which is what
// InteractionsSuppressed, the free-cursor policy and the flow verbs all read. The
// stack must only ever describe screens that are actually on the display.
void UIManager::Show(Scene& scene, const std::string& name) {
    if (Find(scene, name) == entt::null) {
        HBE_ERROR("UI: Show('{}') - no such screen in the resident set; the stack is "
                  "left unchanged.",
                  name);
        return;
    }
    for (const std::string& s : stack_) Activate(scene, s, false);
    stack_.clear();
    Activate(scene, name, true);
    stack_.push_back(name);
}

void UIManager::Push(Scene& scene, const std::string& name) {
    if (Find(scene, name) == entt::null) {
        HBE_ERROR("UI: Push('{}') - no such screen in the resident set; the stack is "
                  "left unchanged.",
                  name);
        return;
    }
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
    return Find(scene, name) != entt::null;
}

} // namespace hbe::ui

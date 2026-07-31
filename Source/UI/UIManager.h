// UI/UIManager.h - shows/hides named UI panels across the resident SCREEN DOCUMENTS.
//
// ONE `.hbui` PER SCREEN. Each screen (MainMenu / Settings / Loading / HUD) is its
// own document; every one of them is opened at boot and stays RESIDENT for the
// process lifetime. The manager toggles which panel is active. It keeps a stack
// (Push/Pop) for menu navigation - only the top panel is active (visible +
// interactive), the rest hidden. Activating a panel restarts its OnShow animators
// (the entrance transition).
//
// RESIDENCY IS THE WHOLE DESIGN. Show("Settings") is a bool write plus a
// BumpUIVersion - no file I/O, no instantiate, no allocation - so there is no
// pop-in, no unstyled first frame and no "screen is loading" state to get wrong.
// The preload contract that makes that true (ui::PreloadUIAssets runs inside
// InstantiateDocument) lives at the OPEN site: every screen document must be
// opened with a real Renderer* and preload = true.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {
class Scene;
}

namespace hbe::ui {

class UIManager {
public:
    // Scope the manager to the open `.hbui` SCREEN DOCUMENTS (UI/UIDocument.h).
    // Panel lookup then ignores every UIPanel that is not a member of one of them.
    //
    // This is not a nicety. FindPanel used to return the first match in the WHOLE
    // registry, which silently cross-wires the moment a second document is open -
    // and it was already latent without documents: Engine::LoadGame's Replace
    // recreates a second copy of every panel from the .hbsave, and the manager
    // could hand back the duplicate. With one document per screen it is not
    // latent at all: the boot splash plus four screens is five resident
    // documents on the very first frame.
    //
    // Handles are bare u32 (== ui::DocHandle) so this header keeps its small
    // include footprint; an EMPTY set is UNBOUND, which restores the old
    // whole-registry behaviour and is what the editor (no document open) sees.
    void Bind(const std::vector<u32>& docs);
    void Bind(u32 doc); // single-document convenience (legacy .hbscene adopt, tests)
    const std::vector<u32>& BoundDocuments() const { return docs_; }
    u32 BoundDocument() const { return docs_.empty() ? 0u : docs_.front(); }

    // Prepare from the loaded screen documents: all panels start HIDDEN (the game
    // flow decides what to show, e.g. after the boot splash), and the first
    // startVisible panel IN BIND ORDER is recorded as the "initial" screen. Call
    // once after the screen documents open.
    //
    // Sweeping the whole SET is what closes the worst failure mode of a per-screen
    // split: a second document shipping `startVisible: true` would otherwise stay
    // permanently on screen because nothing ever deactivated it.
    //
    // Also builds the name -> panel-root table. A DUPLICATE panel name across
    // resident screens is a boot-time HBE_ERROR; first in bind order wins.
    void Init(Scene& scene);
    // The panel root entity for `name`, or entt::null. Scoped to the bound set, so
    // callers that used to scan the whole registry for a UIPanel by name (the
    // loading-bar driver) cannot pick a same-named panel out of another document.
    entt::entity PanelEntity(Scene& scene, const std::string& name);
    // Show the initial (first startVisible) panel - the main menu, typically.
    void ShowInitial(Scene& scene);
    const std::string& Initial() const { return initial_; }
    // The first named panel IN BIND ORDER, whether or not anything is startVisible.
    // The boot's last resort: with one screen per document, only ONE file carries
    // startVisible, so losing that file left `initial_` empty and ShowInitial a
    // silent no-op - i.e. a black screen with no way out. Empty only when the
    // resident set contains no named panel at all.
    const std::string& FirstPanelName() const { return firstPanel_; }
    // Overrides the initial screen (the boot's fallback above). Must name a panel
    // the manager can reach, or ShowInitial stays a no-op.
    void SetInitial(const std::string& name) { initial_ = name; }

    void Show(Scene& scene, const std::string& name); // clear the stack, activate only `name`
    void Push(Scene& scene, const std::string& name); // activate `name` over the current top
    void Pop(Scene& scene);                           // deactivate top, reactivate the previous
    bool Has(Scene& scene, const std::string& name);  // a panel with this name exists

    std::string Top() const { return stack_.empty() ? std::string() : stack_.back(); }
    bool Empty() const { return stack_.empty(); }
    void Clear() { stack_.clear(); }

private:
    void Activate(Scene& scene, const std::string& name, bool on);
    entt::entity Find(Scene& scene, const std::string& name);
    std::vector<std::string> stack_;
    std::string initial_;    // first startVisible panel name (the boot/main screen)
    std::string firstPanel_; // first NAMED panel in bind order (the boot fallback)
    std::vector<u32> docs_; // bound screen documents; EMPTY = whole registry
    // name -> panel root, built by Init. A cache, not a truth: every lookup
    // re-validates the entity (still alive, still a UIPanel, still named that,
    // still in the set) and falls back to a scan, so a Replace that somehow
    // recreated a panel cannot leave the manager pointing at a dead handle.
    std::unordered_map<std::string, entt::entity> panels_;
};

} // namespace hbe::ui

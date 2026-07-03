// UI/UIManager.h - shows/hides named UI panels within one persistent UI scene.
//
// Instead of loading a scene per screen, all screens live in one UI scene as UIPanel
// subtrees; the manager toggles which are active. It keeps a stack (Push/Pop) for menu
// navigation - only the top panel is active (visible + interactive), the rest hidden.
// Activating a panel restarts its OnShow animators (the entrance transition).
#pragma once

#include <string>
#include <vector>

namespace hbe {
class Scene;
}

namespace hbe::ui {

class UIManager {
public:
    // Prepare from the loaded UI scene: all panels start HIDDEN (the game flow decides
    // what to show, e.g. after the boot splash), and the first startVisible panel is
    // recorded as the "initial" screen. Call once after the UI scene loads.
    void Init(Scene& scene);
    // Show the initial (first startVisible) panel - the main menu, typically.
    void ShowInitial(Scene& scene);
    const std::string& Initial() const { return initial_; }

    void Show(Scene& scene, const std::string& name); // clear the stack, activate only `name`
    void Push(Scene& scene, const std::string& name); // activate `name` over the current top
    void Pop(Scene& scene);                           // deactivate top, reactivate the previous
    bool Has(Scene& scene, const std::string& name);  // a panel with this name exists

    std::string Top() const { return stack_.empty() ? std::string() : stack_.back(); }
    bool Empty() const { return stack_.empty(); }
    void Clear() { stack_.clear(); }

private:
    void Activate(Scene& scene, const std::string& name, bool on);
    std::vector<std::string> stack_;
    std::string initial_; // first startVisible panel name (the boot/main screen)
};

} // namespace hbe::ui

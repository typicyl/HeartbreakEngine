// UI/Style/Theme.h - reusable UI style/theme assets (`.hbtheme`).
//
// A `.hbtheme` is a set of named STYLES. A UIElement references one by
// `styleTheme` (the asset) + `styleName`; at emit time ui::style::ApplyStyle fills
// the element's UNSET skin fields from that style, so a whole family of widgets
// shares one editable look instead of duplicating colours/textures per element.
// The element's own set fields always win - a style provides DEFAULTS, not overrides.
//
// A Style mirrors the UIElement skin block and uses the SAME "unset" sentinels
// (alpha-0 colours, empty strings, all-zero slice), so a style only contributes the
// properties it actually declares. This header pulls in nlohmann/json nowhere (the
// json half lives in Theme.cpp), so main_editor.cpp can call the self-test freely.
#pragma once

#include "Core/Types.h"
#include "Scene/Components.h" // UIElement (ApplyStyle overlays onto it)

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace hbe::ui::style {

// The themeable subset of a UIElement's skin. Defaults are the unset sentinels.
struct Style {
    glm::vec4 hoverColor{0.0f};
    glm::vec4 pressedColor{0.0f};
    glm::vec4 focusedColor{0.0f};
    glm::vec4 disabledColor{0.0f};
    glm::vec4 selectedColor{0.0f};
    std::string font;
    std::string hoverSound, clickSound;
    std::string trackTexture, fillTexture, handleTexture, onTexture, offTexture,
        hoverTexture, pressedTexture, disabledTexture, cellTexture;
    glm::vec4 slice{0.0f};
};

struct Theme {
    std::unordered_map<std::string, Style> styles;
};

// Parses a `.hbtheme` JSON document (see docs / the self-test for the shape).
// Returns false on malformed input or a missing `styles` object.
bool ParseTheme(const std::string& jsonText, Theme& out);

// Looks up a named style in a `.hbtheme` (VFS/pack-aware, cached per path). Returns
// nullptr if the theme or the style is missing.
const Style* ResolveStyle(const std::filesystem::path& assetsDir,
                          const std::string& themeRel, const std::string& styleName);

// Overlays the named style onto `el`'s UNSET skin fields (element-set fields win).
// A missing theme/style is a no-op. PURELY VISUAL - call it on the element COPY used
// for emission, never on the authored component.
void ApplyStyle(UIElement& el, const std::filesystem::path& assetsDir,
                const std::string& themeRel, const std::string& styleName);

// Drops cached themes (project switch / re-import).
void ClearThemeCache();

// Headless self-test (--test-uitheme): parse, override precedence, malformed-reject.
bool ThemeSelfTest();

} // namespace hbe::ui::style

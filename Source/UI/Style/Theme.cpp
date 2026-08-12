// UI/Style/Theme.cpp
#include "UI/Style/Theme.h"

#include "Assets/VFS.h" // vfs::ReadFile (pack-aware)
#include "Core/Log.h"

#include <nlohmann/json.hpp>

namespace hbe::ui::style {

namespace {
using json = nlohmann::json;

glm::vec4 Vec4Of(const json& j, const glm::vec4& def) {
    // Per-element is_number guard: a colour array with a non-number element (e.g.
    // [1,0,0,"1"]) must fall back to the default, not throw nlohmann type_error.
    if (j.is_array() && j.size() == 4 && j[0].is_number() && j[1].is_number() &&
        j[2].is_number() && j[3].is_number()) {
        return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
    }
    return def;
}

// Non-throwing string field: nlohmann's value(key,default) throws type_error if the
// key is present but not a string ("font": 5). Return the default in that case too.
std::string StrOf(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

Style ParseStyle(const json& j) {
    Style s;
    if (!j.is_object()) return s;
    s.hoverColor = Vec4Of(j.value("hoverColor", json()), glm::vec4(0.0f));
    s.pressedColor = Vec4Of(j.value("pressedColor", json()), glm::vec4(0.0f));
    s.focusedColor = Vec4Of(j.value("focusedColor", json()), glm::vec4(0.0f));
    s.disabledColor = Vec4Of(j.value("disabledColor", json()), glm::vec4(0.0f));
    s.selectedColor = Vec4Of(j.value("selectedColor", json()), glm::vec4(0.0f));
    s.font = StrOf(j, "font");
    s.hoverSound = StrOf(j, "hoverSound");
    s.clickSound = StrOf(j, "clickSound");
    s.trackTexture = StrOf(j, "trackTexture");
    s.fillTexture = StrOf(j, "fillTexture");
    s.handleTexture = StrOf(j, "handleTexture");
    s.onTexture = StrOf(j, "onTexture");
    s.offTexture = StrOf(j, "offTexture");
    s.hoverTexture = StrOf(j, "hoverTexture");
    s.pressedTexture = StrOf(j, "pressedTexture");
    s.disabledTexture = StrOf(j, "disabledTexture");
    s.cellTexture = StrOf(j, "cellTexture");
    s.slice = Vec4Of(j.value("slice", json()), glm::vec4(0.0f));
    return s;
}

// The overlay rule shared by ApplyStyle and the self-test: fill only UNSET fields.
void Overlay(UIElement& el, const Style& s) {
    const auto col = [](glm::vec4& e, const glm::vec4& v) {
        if (e.a <= 0.0f && v.a > 0.0f) e = v;
    };
    col(el.hoverColor, s.hoverColor);
    col(el.pressedColor, s.pressedColor);
    col(el.focusedColor, s.focusedColor);
    col(el.disabledColor, s.disabledColor);
    col(el.selectedColor, s.selectedColor);
    const auto str = [](std::string& e, const std::string& v) {
        if (e.empty() && !v.empty()) e = v;
    };
    str(el.font, s.font);
    str(el.hoverSound, s.hoverSound);
    str(el.clickSound, s.clickSound);
    str(el.trackTexture, s.trackTexture);
    str(el.fillTexture, s.fillTexture);
    str(el.handleTexture, s.handleTexture);
    str(el.onTexture, s.onTexture);
    str(el.offTexture, s.offTexture);
    str(el.hoverTexture, s.hoverTexture);
    str(el.pressedTexture, s.pressedTexture);
    str(el.disabledTexture, s.disabledTexture);
    str(el.cellTexture, s.cellTexture);
    if (el.slice.x + el.slice.y + el.slice.z + el.slice.w <= 0.0f) el.slice = s.slice;
}

std::unordered_map<std::string, Theme>& ThemeCache() {
    static std::unordered_map<std::string, Theme> c;
    return c;
}

} // namespace

bool ParseTheme(const std::string& text, Theme& out) {
    // The WHOLE parse+extract is guarded: even with is_number/is_string hardening
    // above, a malformed document (or an unforeseen nlohmann type_error) must degrade
    // to "theme failed to parse" (warn + no styling), never an uncaught throw in the
    // per-frame emit loop - that would terminate the process, and re-throw every frame.
    try {
        const json j = json::parse(text);
        if (!j.is_object()) return false;
        const auto styles = j.find("styles");
        if (styles == j.end() || !styles->is_object()) return false;
        for (auto it = styles->begin(); it != styles->end(); ++it)
            out.styles[it.key()] = ParseStyle(it.value());
        return true;
    } catch (...) {
        return false;
    }
}

const Style* ResolveStyle(const std::filesystem::path& assetsDir,
                          const std::string& themeRel, const std::string& styleName) {
    if (themeRel.empty() || styleName.empty()) return nullptr;
    Theme* theme = nullptr;
    if (auto it = ThemeCache().find(themeRel); it != ThemeCache().end()) {
        theme = &it->second;
    } else {
        Theme t;
        if (auto data = vfs::ReadFile(assetsDir / themeRel)) {
            const std::string text(reinterpret_cast<const char*>(data->data()), data->size());
            if (!ParseTheme(text, t)) HBE_WARN("UI: theme '{}' failed to parse.", themeRel);
        } else {
            HBE_WARN("UI: theme '{}' missing.", themeRel);
        }
        // Cache even an empty/failed theme so it is not re-read every frame.
        theme = &ThemeCache().emplace(themeRel, std::move(t)).first->second;
    }
    auto s = theme->styles.find(styleName);
    return s == theme->styles.end() ? nullptr : &s->second;
}

void ApplyStyle(UIElement& el, const std::filesystem::path& assetsDir,
                const std::string& themeRel, const std::string& styleName) {
    if (const Style* s = ResolveStyle(assetsDir, themeRel, styleName)) Overlay(el, *s);
}

void ClearThemeCache() {
    ThemeCache().clear();
}

bool ThemeSelfTest() {
    int fails = 0;
    const auto check = [&](bool c, const char* m) {
        if (c) {
            HBE_INFO("  [ok]   {}", m);
        } else {
            HBE_ERROR("  [FAIL] {}", m);
            ++fails;
        }
    };

    const char* themeJson = R"({
      "version": 1, "kind": "hbtheme",
      "styles": {
        "Primary": {
          "hoverColor": [1,0,0,1],
          "pressedColor": [0,1,0,1],
          "focusedColor": [0,0,1,1],
          "font": "Fonts/Theme.uaf",
          "hoverTexture": "UI/hover.png"
        }
      }
    })";
    Theme t;
    check(ParseTheme(themeJson, t), "parse theme");
    check(t.styles.count("Primary") == 1, "style 'Primary' present");
    check(t.styles.count("Missing") == 0, "unknown style absent");

    // Overlay onto an element with a PARTIAL override: its hoverColor is set
    // (magenta) so it must WIN; pressedColor is unset so it takes the style's green;
    // focusedColor/font are unset so they take the style; a texture the element
    // already has must not be overwritten.
    const Style& s = t.styles.at("Primary");
    UIElement el;
    el.hoverColor = {1, 0, 1, 1};                 // element override (set)
    el.hoverTexture = "UI/mine.png";              // element override (set)
    Overlay(el, s);
    check(el.hoverColor == glm::vec4(1, 0, 1, 1), "element override wins (hover magenta)");
    check(el.pressedColor == glm::vec4(0, 1, 0, 1), "unset field takes the style (pressed green)");
    check(el.focusedColor == glm::vec4(0, 0, 1, 1), "focused takes the style (blue)");
    check(el.font == "Fonts/Theme.uaf", "unset font takes the style");
    check(el.hoverTexture == "UI/mine.png", "set texture is NOT overwritten by the style");

    Theme bad;
    check(!ParseTheme("{ not json", bad), "malformed theme rejected");
    Theme noStyles;
    check(!ParseTheme(R"({"kind":"hbtheme"})", noStyles), "theme without styles rejected");

    HBE_INFO("--test-uitheme: {} failure(s)", fails);
    return fails == 0;
}

} // namespace hbe::ui::style

// UI/UIData.h - P9.4 UI data-binding. A keyed value store the UI reads, plus the resolve
// step that pushes bound values into element runtime fields. Unifies the ad-hoc dynamic-
// content paths (token substitution, settings seeding, interact/caption pushes) behind one
// channel. See docs/Design-UIDataBinding.md.
#pragma once

#include "Core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hbe {
class Scene;
namespace ui {
struct UIContext;

// A small tagged value. `str` doubles as the texture path for Tex.
struct Value {
    enum class T { None, Number, Text, Bool, Tex };
    T t = T::None;
    f64 num = 0.0;
    std::string str;

    static Value Num(f64 n) { return {T::Number, n, {}}; }
    static Value Str(std::string s) { return {T::Text, 0.0, std::move(s)}; }
    static Value Boolean(bool b) { return {T::Bool, b ? 1.0 : 0.0, {}}; }
    static Value TexPath(std::string p) { return {T::Tex, 0.0, std::move(p)}; }

    bool operator==(const Value& o) const {
        return t == o.t && num == o.num && str == o.str;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }
};

// A keyed store with a per-key revision (bumped only on change) and pull-on-demand
// providers. One instance is owned by the Engine and read by ResolveBindings each frame.
class UIDataModel {
public:
    void Set(std::string_view key, Value v);      // bumps rev + globalRev only if changed
    const Value* Get(std::string_view key) const; // nullptr = never set
    u64 Rev(std::string_view key) const;          // 0 = never set
    u64 GlobalRev() const { return globalRev_; }

    using Provider = std::function<Value()>;
    void Provide(std::string key, Provider p);    // pull-on-demand source for a key
    void RefreshProviders();                       // re-run providers + Set results (1/frame)

    void Clear(); // drop values/revs (providers kept); resets on scene/flow change

private:
    std::unordered_map<std::string, Value> vals_;
    std::unordered_map<std::string, u64> revs_;
    std::unordered_map<std::string, Provider> providers_;
    u64 globalRev_ = 1;
};

// Push bound model values into element RUNTIME fields (runtimeText / value+fill / visible /
// texture) - never the serialized authored fields, mirroring how runtimeText overrides text.
// Fast-skips the whole walk when the model's globalRev is unchanged since last call, and
// writes a field only when its value actually differs (so a no-op Set performs zero writes).
// Returns the number of field writes performed (for the --test-uibind write counter).
u32 ResolveBindings(Scene& scene, const UIDataModel& model, UIContext& ctx);

// Headless self-test (--test-uibind): drives a model + ResolveBindings and asserts the
// runtime fields + the rev/no-op write guard. Pure CPU. Returns true on PASS.
bool DataBindSelfTest();

} // namespace ui
} // namespace hbe

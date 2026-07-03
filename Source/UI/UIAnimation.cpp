// UI/UIAnimation.cpp
#include "UI/UIAnimation.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>

namespace hbe::ui {
namespace {
using json = nlohmann::json;

// Loaded clips are tiny and reused every frame, so cache by absolute path. A negative
// cache avoids re-hitting disk for a missing/broken clip each frame.
std::unordered_map<std::string, UIClip> g_clipCache;
std::unordered_map<std::string, bool> g_clipMissing;

const UIClip* GetClip(const std::filesystem::path& path) {
    const std::string key = path.string();
    if (auto it = g_clipCache.find(key); it != g_clipCache.end()) return &it->second;
    if (g_clipMissing.count(key)) return nullptr;
    UIClip clip;
    if (!LoadClip(path, clip)) {
        g_clipMissing[key] = true;
        return nullptr;
    }
    return &(g_clipCache[key] = std::move(clip));
}
} // namespace

void ClearClipCache() {
    g_clipCache.clear();
    g_clipMissing.clear();
}

bool LoadClip(const std::filesystem::path& path, UIClip& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    json j;
    try {
        f >> j;
    } catch (const std::exception&) {
        return false;
    }
    out = UIClip{};
    out.duration = j.value("duration", 1.0f);
    out.loop = j.value("loop", false);
    const int kMaxTarget = static_cast<int>(AnimTarget::Count) - 1;
    for (const json& jt : j.value("tracks", json::array())) {
        AnimTrack tr;
        tr.target = static_cast<AnimTarget>(glm::clamp(jt.value("target", 8), 0, kMaxTarget));
        for (const json& jk : jt.value("keys", json::array())) {
            AnimKey k;
            k.time = jk.value("t", 0.0f);
            k.value = jk.value("v", 0.0f);
            k.curve = static_cast<ease::Curve>(glm::clamp(jk.value("curve", 0), 0, 7));
            tr.keys.push_back(k);
        }
        std::sort(tr.keys.begin(), tr.keys.end(),
                  [](const AnimKey& a, const AnimKey& b) { return a.time < b.time; });
        out.tracks.push_back(std::move(tr));
    }
    return true;
}

bool SaveClip(const std::filesystem::path& path, const UIClip& clip) {
    json j;
    j["duration"] = clip.duration;
    j["loop"] = clip.loop;
    json jtracks = json::array();
    for (const AnimTrack& tr : clip.tracks) {
        json jt;
        jt["target"] = static_cast<int>(tr.target);
        json jkeys = json::array();
        for (const AnimKey& k : tr.keys)
            jkeys.push_back(
                {{"t", k.time}, {"v", k.value}, {"curve", static_cast<int>(k.curve)}});
        jt["keys"] = jkeys;
        jtracks.push_back(jt);
    }
    j["tracks"] = jtracks;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string s = j.dump(2);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return static_cast<bool>(f);
}

f32 SampleTrack(const AnimTrack& track, f32 t) {
    if (track.keys.empty()) return 0.0f;
    if (t <= track.keys.front().time) return track.keys.front().value;
    if (t >= track.keys.back().time) return track.keys.back().value;
    for (std::size_t i = 1; i < track.keys.size(); ++i) {
        if (t <= track.keys[i].time) {
            const AnimKey& a = track.keys[i - 1];
            const AnimKey& b = track.keys[i];
            const f32 span = glm::max(b.time - a.time, 1e-5f);
            const f32 lt = ease::Ease(b.curve, (t - a.time) / span);
            return glm::mix(a.value, b.value, lt);
        }
    }
    return track.keys.back().value;
}

void Apply(const UIClip& clip, f32 t, UIElement& el, const glm::vec2& baseOffset) {
    glm::vec2 offAdd(0.0f);
    bool hasOff = false;
    for (const AnimTrack& tr : clip.tracks) {
        const f32 v = SampleTrack(tr, t);
        switch (tr.target) {
            case AnimTarget::OffsetX:  offAdd.x = v; hasOff = true; break;
            case AnimTarget::OffsetY:  offAdd.y = v; hasOff = true; break;
            case AnimTarget::ScaleX:   el.scale.x = v; break;
            case AnimTarget::ScaleY:   el.scale.y = v; break;
            case AnimTarget::Rotation: el.rotation = v; break;
            case AnimTarget::ColorR:   el.color.r = v; break;
            case AnimTarget::ColorG:   el.color.g = v; break;
            case AnimTarget::ColorB:   el.color.b = v; break;
            case AnimTarget::Opacity:  el.color.a = v; break;
            case AnimTarget::SpriteFrame:
                if (!el.frames.empty()) {
                    const int idx = glm::clamp(static_cast<int>(std::floor(v)), 0,
                                               static_cast<int>(el.frames.size()) - 1);
                    if (el.texture != el.frames[idx]) {
                        el.texture = el.frames[idx];
                        el.textureResolved = false; // re-resolve to the new frame's texture
                    }
                }
                break;
            default: break;
        }
    }
    if (hasOff) el.offset = baseOffset + offAdd;
}

void UpdateAnimations(Scene& scene, f32 dt, const std::filesystem::path& assetsDir) {
    auto& reg = scene.Registry();

    // Auto-scrolling ScrollViews (credits rolls) drift here - the one UI tick
    // with a dt. Looping content wraps around: it re-enters from below/right
    // (scrollPos -view, i.e. content parked just past the view's far edge)
    // once it has fully scrolled past (scrollPos > contentExtent).
    if (dt > 0.0f) {
        for (const entt::entity e : reg.view<UIElement>()) {
            UIElement& el = reg.get<UIElement>(e);
            if (el.type != UIElement::Type::ScrollView || el.autoScroll == 0.0f ||
                !el.visible)
                continue;
            f32& pos = el.scrollVertical ? el.scrollPos.y : el.scrollPos.x;
            const f32 content =
                el.scrollVertical ? el.contentExtent.y : el.contentExtent.x;
            const f32 view = el.scrollVertical ? el.viewExtent.y : el.viewExtent.x;
            if (view <= 0.0f) continue; // not laid out yet (first frame)
            pos += el.autoScroll * dt;
            if (el.autoScrollLoop) {
                if (pos > content) pos = -view;      // wrapped: re-enter from below
                else if (pos < -view) pos = content; // (negative autoScroll)
            } else {
                pos = glm::clamp(pos, 0.0f, glm::max(content - view, 0.0f));
            }
        }
    }

    for (const entt::entity e : reg.view<UIAnimator, UIElement>()) {
        UIAnimator& an = reg.get<UIAnimator>(e);
        UIElement& el = reg.get<UIElement>(e);
        if (an.clip.empty()) continue;
        const UIClip* clip = GetClip(assetsDir / an.clip);
        if (!clip) continue;

        const auto restart = [&]() {
            // Restarting mid-flight: put the element back at its captured home
            // FIRST, or the re-capture below would adopt a half-animated offset
            // as the new base (accumulating drift on repeated Hover/Click plays).
            if (an.captured) el.offset = an.baseOffset;
            an.time = 0.0f;
            an.playing = true;
            an.baseOffset = el.offset; // capture home position for additive offset tracks
            an.captured = true;
            an.justStarted = true; // hold the exact t=0 pose on the start frame
        };
        // Edge-detected trigger -> (re)start.
        switch (an.trigger) {
            case UIAnimator::Trigger::Loop:   if (!an.playing) restart(); break;
            case UIAnimator::Trigger::OnShow: if (el.visible && !an.prevVisible) restart(); break;
            case UIAnimator::Trigger::OnHide: if (!el.visible && an.prevVisible) restart(); break;
            case UIAnimator::Trigger::Hover:  if (el.hovered && !an.prevHovered) restart(); break;
            case UIAnimator::Trigger::Click:  if (el.clicked) restart(); break;
            case UIAnimator::Trigger::Manual: default: break; // started externally
        }
        an.prevVisible = el.visible;
        an.prevHovered = el.hovered;

        if (!an.playing) continue;
        // Externally-started plays (UIManager panel activation, schematic
        // UIPlayAnim set playing/time and cleared captured): capture the base
        // and hold t=0 this frame, exactly like an edge-triggered restart.
        if (!an.captured) {
            an.baseOffset = el.offset;
            an.captured = true;
            an.justStarted = true;
        }
        if (an.justStarted)
            an.justStarted = false; // show the authored first keyframe this frame
        else
            an.time += dt;
        f32 t = an.time;
        const bool looping = clip->loop || an.trigger == UIAnimator::Trigger::Loop;
        if (looping) {
            t = clip->duration > 1e-4f ? std::fmod(an.time, clip->duration) : 0.0f;
        } else if (an.time >= clip->duration) {
            t = clip->duration;
            an.playing = false; // finished: hold the final pose
        }
        Apply(*clip, t, el, an.baseOffset);
    }
}

} // namespace hbe::ui

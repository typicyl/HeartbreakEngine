// Scene/FacialSystem.cpp - lip-sync + blink + expression driver.
#include "Scene/FacialSystem.h"

#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <unordered_map>

namespace hbe::facial {

namespace fs = std::filesystem;

namespace {

// path -> amplitude envelope (RMS windows, normalized to [0,1] at ~60 Hz).
std::unordered_map<std::string, std::vector<f32>> g_envCache;
// preset name -> { morph channel -> weight }.
std::unordered_map<std::string, std::unordered_map<std::string, f32>> g_presets;

f32 RandRange(u32& s, f32 a, f32 b) {
    s = s * 1664525u + 1013904223u; // LCG
    return a + (b - a) * (static_cast<f32>(s >> 8) / static_cast<f32>(1u << 24));
}

const std::vector<f32>& GetEnvelope(const fs::path& path, f32 envRate) {
    const std::string key = path.string() + "@" + std::to_string(envRate); // rate affects windows
    if (auto it = g_envCache.find(key); it != g_envCache.end()) return it->second;
    std::vector<f32> env;
    if (auto a = uaf::ReadAudio(path); a && !a->pcm.empty() && a->bitsPerSample == 16) {
        const auto* s = reinterpret_cast<const i16*>(a->pcm.data());
        const usize totalSamples = a->pcm.size() / sizeof(i16);
        const u32 ch = a->channels ? a->channels : 1;
        const usize frames = totalSamples / ch;
        const usize win = std::max<usize>(1, static_cast<usize>(a->sampleRate / std::max(1.0f, envRate)));
        f32 peak = 1e-6f;
        for (usize f = 0; f < frames; f += win) {
            f64 sum = 0.0;
            usize n = 0;
            for (usize i = f; i < f + win && i < frames; ++i) {
                f32 mono = 0.0f;
                for (u32 c = 0; c < ch; ++c) mono += s[i * ch + c] / 32768.0f;
                mono /= static_cast<f32>(ch);
                sum += static_cast<f64>(mono) * mono;
                ++n;
            }
            const f32 rms = n ? static_cast<f32>(std::sqrt(sum / static_cast<f64>(n))) : 0.0f;
            env.push_back(rms);
            peak = std::max(peak, rms);
        }
        for (f32& v : env) v = std::min(1.0f, std::max(0.0f, v / peak));
        for (usize i = 1; i < env.size(); ++i) env[i] = env[i] * 0.6f + env[i - 1] * 0.4f; // low-pass
    }
    return g_envCache.emplace(key, std::move(env)).first->second;
}

const std::unordered_map<std::string, f32>* FindPreset(const std::string& name) {
    auto it = g_presets.find(name);
    return it != g_presets.end() ? &it->second : nullptr;
}

} // namespace

MorphState* ResolveMorphTarget(Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    // Reached from a schematic Entity pin (raw handle bits), so a despawned target is
    // routine; try_get on an invalid handle is an assert in Debug and an out-of-bounds
    // sparse-set index in Release.
    if (e == entt::null || !reg.valid(e)) return nullptr;
    if (MorphState* ms = reg.try_get<MorphState>(e)) return ms;
    if (Character* ch = reg.try_get<Character>(e)) {
        if (auto it = ch->liveParts.find("head");
            it != ch->liveParts.end() && reg.valid(it->second))
            return reg.try_get<MorphState>(it->second);
    }
    return nullptr;
}

void Update(Scene& scene, f32 dt) {
    entt::registry& reg = scene.Registry();
    for (auto e : reg.view<FacialAnimator>()) {
        FacialAnimator& fa = reg.get<FacialAnimator>(e);
        MorphState* ms = ResolveMorphTarget(scene, e);
        if (!ms) continue;

        std::unordered_map<std::string, f32> now; // channels driven THIS frame

        // Lip-sync: sample the envelope, attack/release-smooth the jaw.
        if (fa.lipSync) {
            f32 target = 0.0f;
            if (!fa.env.empty()) {
                const usize idx = static_cast<usize>(fa.envTime * fa.envRate);
                if (idx < fa.env.size()) target = fa.env[idx];
            }
            const f32 rate = target > fa.jawCur ? fa.jawAttack : fa.jawRelease;
            fa.jawCur += (target - fa.jawCur) * std::min(1.0f, std::max(0.0f, rate * dt));
            fa.envTime += dt;
            if (!fa.jawTarget.empty())
                now[fa.jawTarget] = std::min(1.0f, std::max(0.0f, fa.jawCur * fa.jawStrength));
        }

        // Eye-blink: timed quadratic pulse (deterministic per-entity PRNG).
        if (fa.autoBlink) {
            if (!fa.seeded) {
                fa.seeded = true;
                fa.rng = 0x9e3779b9u ^ static_cast<u32>(e);
                fa.blinkTimer = RandRange(fa.rng, fa.blinkMin, fa.blinkMax);
            }
            if (fa.blinkPhase >= 0.0f) {
                fa.blinkPhase += dt;
                const f32 t = fa.blinkPhase / std::max(0.01f, fa.blinkDuration);
                const f32 w = t >= 1.0f ? 0.0f : 1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f);
                if (!fa.blinkL.empty()) now[fa.blinkL] = w;
                if (!fa.blinkR.empty()) now[fa.blinkR] = w;
                if (t >= 1.0f) {
                    fa.blinkPhase = -1.0f;
                    fa.blinkTimer = RandRange(fa.rng, fa.blinkMin, fa.blinkMax);
                }
            } else {
                fa.blinkTimer -= dt;
                if (fa.blinkTimer <= 0.0f) fa.blinkPhase = 0.0f;
            }
        }

        // Expression preset (takes the max with any lip-sync/blink on shared channels).
        if (!fa.expression.empty()) {
            if (const auto* preset = FindPreset(fa.expression))
                for (const auto& [name, w] : *preset) {
                    const f32 v = std::min(1.0f, std::max(0.0f, w * fa.expressionWeight));
                    auto it = now.find(name);
                    now[name] = it != now.end() ? std::max(it->second, v) : v;
                }
        }

        // Apply: zero channels driven last frame but not this frame, then write.
        for (const std::string& ch : fa.driven)
            if (!now.count(ch)) ms->weights[ch] = 0.0f;
        fa.driven.clear();
        fa.driven.reserve(now.size());
        for (const auto& [ch, w] : now) {
            ms->weights[ch] = w;
            fa.driven.push_back(ch);
        }
    }
}

void StartLipSync(Scene& scene, entt::entity actor, const fs::path& assetsDir,
                  const std::string& clip) {
    if (clip.empty() || actor == entt::null) return;
    FacialAnimator* fa = scene.Registry().try_get<FacialAnimator>(actor);
    if (!fa || !fa->lipSync) return;
    fa->env = GetEnvelope(assetsDir / clip, fa->envRate);
    fa->envTime = 0.0f;
    fa->jawCur = 0.0f;
}

void ClearEnvelopeCache() { g_envCache.clear(); }

u32 LoadPresetLibrary(const fs::path& hbface) {
    std::ifstream in(hbface);
    if (!in) return 0;
    u32 n = 0;
    try {
        nlohmann::json j;
        in >> j;
        const nlohmann::json& presets = j.contains("presets") ? j["presets"] : j;
        for (auto it = presets.begin(); it != presets.end(); ++it) {
            if (!it->is_object()) continue;
            std::unordered_map<std::string, f32> weights;
            for (auto w = it->begin(); w != it->end(); ++w)
                if (w->is_number()) weights[w.key()] = w->get<f32>();
            g_presets[it.key()] = std::move(weights);
            ++n;
        }
    } catch (const std::exception& e) {
        HBE_WARN("facial: failed to parse preset library '{}': {}", hbface.string(), e.what());
    }
    return n;
}

} // namespace hbe::facial

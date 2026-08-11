// Scene/LightningSystem.cpp - see LightningSystem.h.
#include "Scene/LightningSystem.h"

#include "Audio/AudioSystem.h"
#include "Scene/Scene.h"

#include <algorithm>

namespace hbe::lightning {

namespace {
u32 HashU32(u32 n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return n;
}
} // namespace

void Update(Scene& scene, f32 dt, AudioSystem* audio, const std::filesystem::path& assetsDir) {
    SceneEnvironment& env = scene.Environment();
    f32& flash = scene.LightningFlash();

    // Fast decay of the current flash (a strike lights up then fades over ~1/6 s).
    flash = std::max(0.0f, flash - dt * 6.0f);

    // Per-frame pacing state (single active scene; a level reload just re-paces harmlessly).
    static f32 s_strikeTimer = 3.0f;   // time until the next strike
    static f32 s_thunderTimer = -1.0f; // >0 = thunder scheduled
    static int s_flickers = 0;         // remaining flicker sub-flashes this strike
    static f32 s_flickerTimer = 0.0f;
    static u32 s_seed = 1u;

    // Flicker: a strike is a burst of 1-3 quick sub-flashes.
    if (s_flickers > 0) {
        s_flickerTimer -= dt;
        if (s_flickerTimer <= 0.0f) {
            flash = 1.0f;
            --s_flickers;
            s_flickerTimer = 0.06f;
        }
    }

    const bool storm = env.lightning != 0 && env.precipType == 1u &&
                       env.precipIntensity > 0.55f && env.overcast > 0.45f;
    if (storm) {
        s_strikeTimer -= dt;
        if (s_strikeTimer <= 0.0f) {
            const u32 h = HashU32(s_seed++);
            flash = 1.0f;
            s_flickers = 1 + static_cast<int>(h % 3u);                   // 1-3 flickers
            s_flickerTimer = 0.05f;
            s_thunderTimer = 0.4f + ((h >> 8) & 0xffu) / 255.0f * 3.0f;  // 0.4-3.4 s delay = distance
            const f32 intensity = std::clamp(env.precipIntensity, 0.0f, 1.0f);
            const f32 base = 9.0f - 6.5f * intensity;                    // heavier storm = more frequent
            s_strikeTimer = base * (0.5f + ((h >> 16) & 0xffu) / 255.0f);
        }
    } else {
        // Not storming: keep the next-strike soon so a new storm strikes within ~1.5 s.
        s_strikeTimer = std::min(s_strikeTimer, 1.5f);
        s_flickers = 0;
    }

    // Delayed thunder (optional; only when a thunder asset is configured).
    if (s_thunderTimer > 0.0f) {
        s_thunderTimer -= dt;
        if (s_thunderTimer <= 0.0f) {
            s_thunderTimer = -1.0f;
            if (audio && !env.thunderSound.empty() && !assetsDir.empty())
                audio->PlayUAF(assetsDir / env.thunderSound, {}, false);
        }
    }
}

} // namespace hbe::lightning

// Scene/WeatherSystem.cpp - see WeatherSystem.h.
#include "Scene/WeatherSystem.h"

#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace hbe::weather {

namespace {
// Ease `cur` toward `target` at `rate` per second, frame-rate independent: over dt
// seconds close the gap by 1 - exp(-rate*dt). rate is roughly "fraction of the
// remaining gap closed per second at small dt".
f32 Approach(f32 cur, f32 target, f32 rate, f32 dt) {
    const f32 t = 1.0f - std::exp(-std::max(rate, 0.0f) * std::max(dt, 0.0f));
    return cur + (target - cur) * t;
}
} // namespace

void Update(Scene& scene, f32 dt) {
    SceneEnvironment& env = scene.Environment();
    if (env.dynamicWeather == 0) return;

    const f32 intensity = std::clamp(env.precipIntensity, 0.0f, 1.0f);
    const bool raining = (env.precipType == 1u) && intensity > 0.001f;
    const bool snowing = (env.precipType == 2u) && intensity > 0.001f;

    // Targets. Rain wets the ground almost immediately (a floor of 0.3 the moment it
    // starts) and slowly builds standing water toward the rain intensity. Snow builds
    // toward its intensity. Anything not currently falling decays to 0 - wetness
    // evaporates, puddles drain, snow melts - so switching the weather off recovers a
    // dry scene without the author touching the sliders.
    const f32 wetTarget    = raining ? std::min(1.0f, 0.3f + 0.7f * intensity) : 0.0f;
    const f32 puddleTarget = raining ? intensity : 0.0f;
    const f32 snowTarget   = snowing ? intensity : 0.0f;

    // Rates (per second): wetting is fast, drying slow; pooling is slow either way;
    // snow accumulates slowly and melts slower still. The asymmetry is what makes it
    // read as real weather rather than a slider that snaps.
    env.wetness    = Approach(env.wetness,    wetTarget,    raining ? 0.60f : 0.15f, dt);
    env.puddles    = Approach(env.puddles,    puddleTarget, raining ? 0.12f : 0.06f, dt);
    env.snowAmount = Approach(env.snowAmount, snowTarget,   snowing ? 0.06f : 0.015f, dt);

    env.wetness    = std::clamp(env.wetness,    0.0f, 1.0f);
    env.puddles    = std::clamp(env.puddles,    0.0f, 1.0f);
    env.snowAmount = std::clamp(env.snowAmount, 0.0f, 1.0f);
}

} // namespace hbe::weather

// Scene/WeatherSystem.h - real-time weather-condition simulation.
//
// The engine already renders the analytic sky's cloud/overcast/wind layer and the
// day/night sun. This system owns the GROUND-side weather response: how wet the
// surfaces are, how much standing water has pooled, and how much snow has settled.
//
// When SceneEnvironment::dynamicWeather is on, Update() eases those three values
// (wetness / puddles / snowAmount) toward the targets implied by the current
// precipType + precipIntensity - rain wets the ground quickly and slowly pools;
// snow accumulates; clear weather dries the wetness (evaporation) and melts the snow
// (slower). When dynamicWeather is off it is a no-op and the authored values are used
// as-is, so a permanently wet or snowed scene is just data.
//
// This mirrors Engine::UpdateDayNight: a free function that mutates
// scene.Environment() once per frame, before RenderScene reads it into the SceneView.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;

namespace weather {

// Advances the simulated ground weather (no-op unless SceneEnvironment::dynamicWeather).
// Frame-rate independent; cheap (a handful of scalar eases). Call once per frame,
// before Renderer::RenderScene.
void Update(Scene& scene, f32 dt);

} // namespace weather
} // namespace hbe

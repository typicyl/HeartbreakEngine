// Scene/PostSettingsSerialization.h - shared JSON (de)serialization for the HDR
// post-process stack (rhi::PostSettings). Used by BOTH the scene serializer
// (.hbscene) and the project settings (.hbproj) so the key list lives in one
// place. Inline + header-only; only the two .cpp that need it include it.
#pragma once

#include "RHI/RHI.h"

#include <nlohmann/json.hpp>

namespace hbe::scene {

inline nlohmann::json PostToJson(const rhi::PostSettings& p) {
    return nlohmann::json{
        {"bloom", p.bloomEnabled},          {"bloomIntensity", p.bloomIntensity},
        {"bloomThreshold", p.bloomThreshold}, {"ssao", p.ssaoEnabled},
        {"ssaoRadius", p.ssaoRadius},       {"ssaoIntensity", p.ssaoIntensity},
        {"fxaa", p.fxaaEnabled},            {"taa", p.taaEnabled},
        {"vignette", p.vignette},           {"saturation", p.saturation},
        {"contrast", p.contrast},           {"dof", p.dofEnabled},
        {"dofFocusDistance", p.dofFocusDistance}, {"dofFocusRange", p.dofFocusRange},
        {"dofMaxBlur", p.dofMaxBlur},       {"motionBlur", p.motionBlurEnabled},
        {"motionBlurIntensity", p.motionBlurIntensity},
        {"motionBlurMaxRadius", p.motionBlurMaxRadius}, {"ssr", p.ssrEnabled},
        {"ssrIntensity", p.ssrIntensity},   {"ssrMaxDistance", p.ssrMaxDistance},
        {"autoExposure", p.autoExposureEnabled}, {"autoExposureKey", p.autoExposureKey},
        {"autoExposureSpeed", p.autoExposureSpeed}, {"autoExposureMin", p.autoExposureMin},
        {"autoExposureMax", p.autoExposureMax},
        {"fog", p.fogEnabled},              {"fogDensity", p.fogDensity},
        {"fogHeightFalloff", p.fogHeightFalloff}, {"fogHeight", p.fogHeight},
        {"fogAnisotropy", p.fogAnisotropy}, {"fogSunIntensity", p.fogSunIntensity},
        {"fogMaxDistance", p.fogMaxDistance}, {"fogAmbient", p.fogAmbient},
        {"fogStepCount", p.fogStepCount},
        {"fogColor", {p.fogColor.x, p.fogColor.y, p.fogColor.z}},
        {"fogGodRays", p.fogGodRays},
        {"ssgi", p.ssgiEnabled},            {"ssgiIntensity", p.ssgiIntensity},
        {"ssgiRadius", p.ssgiRadius},       {"ssgiSamples", p.ssgiSamples},
        {"painterly", p.painterlyEnabled},  {"painterlyRadius", p.painterlyRadius},
        {"painterlyWarmCool", p.painterlyWarmCool},
        {"painterlyStrokeFlow", p.painterlyStrokeFlow},
        {"painterlyStrength", p.painterlyStrength},
        {"painterlyEdge", p.painterlyEdge},
        {"painterlyLightTint", p.painterlyLightTint},
        {"painterlyStrokeDetail", p.painterlyStrokeDetail},
        {"painterlyCanvasScale", p.painterlyCanvasScale},
        {"painterlyCanvasStrength", p.painterlyCanvasStrength},
        {"painterlyPosterize", p.painterlyPosterize},
        {"painterlyStrokes", p.painterlyStrokes},
        {"painterlyStrokeLength", p.painterlyStrokeLength},
        {"painterlyStrokeDensity", p.painterlyStrokeDensity},
        {"painterlyStrokeSharp", p.painterlyStrokeSharp},
        {"painterlyStrokeBoil", p.painterlyStrokeBoil},
        {"painterlyStrokeMask", p.painterlyStrokeMask},
        {"painterlyStrokeMaskMinX", p.painterlyStrokeMaskMinX},
        {"painterlyStrokeMaskMinY", p.painterlyStrokeMaskMinY},
        {"painterlyStrokeMaskMaxX", p.painterlyStrokeMaskMaxX},
        {"painterlyStrokeMaskMaxY", p.painterlyStrokeMaskMaxY},
        {"painterly3D", p.painterly3D},
    };
}

inline void PostFromJson(const nlohmann::json& j, rhi::PostSettings& p) {
    p.bloomEnabled = j.value("bloom", p.bloomEnabled);
    p.bloomIntensity = j.value("bloomIntensity", p.bloomIntensity);
    p.bloomThreshold = j.value("bloomThreshold", p.bloomThreshold);
    p.ssaoEnabled = j.value("ssao", p.ssaoEnabled);
    p.ssaoRadius = j.value("ssaoRadius", p.ssaoRadius);
    p.ssaoIntensity = j.value("ssaoIntensity", p.ssaoIntensity);
    p.fxaaEnabled = j.value("fxaa", p.fxaaEnabled);
    p.taaEnabled = j.value("taa", p.taaEnabled);
    p.vignette = j.value("vignette", p.vignette);
    p.saturation = j.value("saturation", p.saturation);
    p.contrast = j.value("contrast", p.contrast);
    p.dofEnabled = j.value("dof", p.dofEnabled);
    p.dofFocusDistance = j.value("dofFocusDistance", p.dofFocusDistance);
    p.dofFocusRange = j.value("dofFocusRange", p.dofFocusRange);
    p.dofMaxBlur = j.value("dofMaxBlur", p.dofMaxBlur);
    p.motionBlurEnabled = j.value("motionBlur", p.motionBlurEnabled);
    p.motionBlurIntensity = j.value("motionBlurIntensity", p.motionBlurIntensity);
    p.motionBlurMaxRadius = j.value("motionBlurMaxRadius", p.motionBlurMaxRadius);
    p.ssrEnabled = j.value("ssr", p.ssrEnabled);
    p.ssrIntensity = j.value("ssrIntensity", p.ssrIntensity);
    p.ssrMaxDistance = j.value("ssrMaxDistance", p.ssrMaxDistance);
    p.autoExposureEnabled = j.value("autoExposure", p.autoExposureEnabled);
    p.autoExposureKey = j.value("autoExposureKey", p.autoExposureKey);
    p.autoExposureSpeed = j.value("autoExposureSpeed", p.autoExposureSpeed);
    p.autoExposureMin = j.value("autoExposureMin", p.autoExposureMin);
    p.autoExposureMax = j.value("autoExposureMax", p.autoExposureMax);
    p.fogEnabled = j.value("fog", p.fogEnabled);
    p.fogDensity = j.value("fogDensity", p.fogDensity);
    p.fogHeightFalloff = j.value("fogHeightFalloff", p.fogHeightFalloff);
    p.fogHeight = j.value("fogHeight", p.fogHeight);
    p.fogAnisotropy = j.value("fogAnisotropy", p.fogAnisotropy);
    p.fogSunIntensity = j.value("fogSunIntensity", p.fogSunIntensity);
    p.fogMaxDistance = j.value("fogMaxDistance", p.fogMaxDistance);
    p.fogAmbient = j.value("fogAmbient", p.fogAmbient);
    p.fogStepCount = j.value("fogStepCount", p.fogStepCount);
    if (auto it = j.find("fogColor"); it != j.end() && it->is_array() && it->size() == 3)
        p.fogColor = {(*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>()};
    p.fogGodRays = j.value("fogGodRays", p.fogGodRays);
    p.ssgiEnabled = j.value("ssgi", p.ssgiEnabled);
    p.ssgiIntensity = j.value("ssgiIntensity", p.ssgiIntensity);
    p.ssgiRadius = j.value("ssgiRadius", p.ssgiRadius);
    p.ssgiSamples = j.value("ssgiSamples", p.ssgiSamples);
    p.painterlyEnabled = j.value("painterly", p.painterlyEnabled);
    p.painterlyRadius = j.value("painterlyRadius", p.painterlyRadius);
    p.painterlyWarmCool = j.value("painterlyWarmCool", p.painterlyWarmCool);
    p.painterlyStrokeFlow = j.value("painterlyStrokeFlow", p.painterlyStrokeFlow);
    p.painterlyStrength = j.value("painterlyStrength", p.painterlyStrength);
    p.painterlyEdge = j.value("painterlyEdge", p.painterlyEdge);
    p.painterlyLightTint = j.value("painterlyLightTint", p.painterlyLightTint);
    p.painterlyStrokeDetail = j.value("painterlyStrokeDetail", p.painterlyStrokeDetail);
    p.painterlyCanvasScale = j.value("painterlyCanvasScale", p.painterlyCanvasScale);
    p.painterlyCanvasStrength = j.value("painterlyCanvasStrength", p.painterlyCanvasStrength);
    p.painterlyPosterize = j.value("painterlyPosterize", p.painterlyPosterize);
    p.painterlyStrokes = j.value("painterlyStrokes", p.painterlyStrokes);
    p.painterlyStrokeLength = j.value("painterlyStrokeLength", p.painterlyStrokeLength);
    p.painterlyStrokeDensity = j.value("painterlyStrokeDensity", p.painterlyStrokeDensity);
    p.painterlyStrokeSharp = j.value("painterlyStrokeSharp", p.painterlyStrokeSharp);
    p.painterlyStrokeBoil = j.value("painterlyStrokeBoil", p.painterlyStrokeBoil);
    p.painterlyStrokeMask = j.value("painterlyStrokeMask", p.painterlyStrokeMask);
    p.painterlyStrokeMaskMinX = j.value("painterlyStrokeMaskMinX", p.painterlyStrokeMaskMinX);
    p.painterlyStrokeMaskMinY = j.value("painterlyStrokeMaskMinY", p.painterlyStrokeMaskMinY);
    p.painterlyStrokeMaskMaxX = j.value("painterlyStrokeMaskMaxX", p.painterlyStrokeMaskMaxX);
    p.painterlyStrokeMaskMaxY = j.value("painterlyStrokeMaskMaxY", p.painterlyStrokeMaskMaxY);
    p.painterly3D = j.value("painterly3D", p.painterly3D);
}

} // namespace hbe::scene

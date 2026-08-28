// Scene/PaintReference.cpp - see PaintReference.h.
#include "Scene/PaintReference.h"

#include "Editor/MovieRender.h" // WritePng
#include "Scene/Components.h"
#include "Scene/PaintSystem.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace hbe::paint {
namespace {

// A hand-like stroke path in canvas UV: a gentle arc, the kind of sweep an arm
// makes. Deliberately NOT a straight line - a straight ribbon is exactly what a
// procedural system produces by accident, and the difference is the point.
std::vector<StrokePoint> ArcPath(glm::vec2 a, glm::vec2 b, f32 bow, f32 radius, int n) {
    std::vector<StrokePoint> path;
    path.reserve(static_cast<usize>(n));
    const glm::vec2 d = b - a;
    const glm::vec2 perp(-d.y, d.x);
    for (int i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n - 1);
        StrokePoint p;
        p.uv = a + d * t + perp * (std::sin(t * 3.14159265f) * bow);
        p.radius = radius;
        // Pressure arc: a real stroke lands, loads, and lifts. This is what makes
        // the ends fade rather than stop dead.
        p.pressure = 0.35f + 0.65f * std::sin(t * 3.14159265f);
        path.push_back(p);
    }
    return path;
}

void WriteChannelPng(const std::filesystem::path& p, u32 res, const std::vector<u8>& buf,
                     int channel, bool asGrey) {
    std::vector<u8> out(static_cast<usize>(res) * res * 4);
    for (usize i = 0; i < static_cast<usize>(res) * res; ++i) {
        if (asGrey) {
            const u8 v = buf[i * 4 + static_cast<usize>(channel)];
            out[i * 4 + 0] = v;
            out[i * 4 + 1] = v;
            out[i * 4 + 2] = v;
            out[i * 4 + 3] = 255;
        } else {
            // Composite the painted colour over a mid ground so coverage reads:
            // an un-painted texel is transparent, and on black everything would
            // look like coverage.
            const f32 a = buf[i * 4 + 3] / 255.0f;
            for (int k = 0; k < 3; ++k)
                out[i * 4 + static_cast<usize>(k)] =
                    static_cast<u8>(buf[i * 4 + static_cast<usize>(k)] * a + 96.0f * (1.0f - a));
            out[i * 4 + 3] = 255;
        }
    }
    movie::WritePng(p, res, res, out);
}

} // namespace

bool WriteReferenceSwatches(const std::filesystem::path& outDir, std::string& report) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::vector<BrushDef> kit = DefaultBrushes();
    if (kit.empty()) {
        report = "DefaultBrushes() is empty";
        return false;
    }
    constexpr u32 kRes = 768;
    int written = 0;

    for (const BrushDef& def : kit) {
        PaintComponent p;
        p.resolution = kRes;
        EnsureCanvas(p, kRes);
        if (p.layers.empty()) continue;
        const u32 layerId = p.layers[0].id;

        // Five overlapping sweeps at different angles - enough to show what a
        // stroke's body, ends and overlaps look like without becoming a wash.
        const struct { glm::vec2 a, b; f32 bow; f32 rad; glm::vec4 col; } kSweeps[] = {
            {{0.08f, 0.22f}, {0.92f, 0.30f},  0.05f, 0.055f, {0.62f, 0.30f, 0.22f, 1.0f}},
            {{0.10f, 0.45f}, {0.88f, 0.38f}, -0.04f, 0.045f, {0.80f, 0.62f, 0.35f, 1.0f}},
            {{0.12f, 0.58f}, {0.90f, 0.66f},  0.06f, 0.060f, {0.30f, 0.38f, 0.52f, 1.0f}},
            {{0.15f, 0.80f}, {0.85f, 0.74f}, -0.05f, 0.040f, {0.86f, 0.84f, 0.78f, 1.0f}},
            {{0.30f, 0.10f}, {0.55f, 0.92f},  0.08f, 0.035f, {0.45f, 0.50f, 0.30f, 1.0f}},
        };
        for (const auto& s : kSweeps) {
            Stroke st;
            st.type = StrokeType::Path;
            st.layerId = layerId;
            st.projection = 0; // mesh-UV disc stamping
            st.brush = def;
            st.color = s.col;
            st.flow = def.flow;
            st.height = def.relief;
            st.roughness = 0.55f;
            st.colorVar = def.colorVar > 0.0f ? def.colorVar : 0.18f;
            st.paintColor = true;
            st.paintMaterial = true;
            st.path = ArcPath(s.a, s.b, s.bow, s.rad, 48);
            p.strokes.push_back(st);
        }

        BakeFromStrokes(p, nullptr);
        Flatten(p);
        if (p.flatColor.empty()) continue;

        // --- measure the structure, so the procedural renderer has NUMBERS to hit --
        // Occupancy: share of the canvas actually carrying paint. Run-length
        // anisotropy: mean run of covered texels along the dominant stroke
        // direction (X here - four of the five sweeps run across) divided by the
        // mean run across it. A BODY scores high; a grain field scores ~1.0.
        {
            const auto covered = [&](u32 x, u32 y) {
                return p.flatColor[(static_cast<usize>(y) * kRes + x) * 4 + 3] > 128;
            };
            usize on = 0;
            for (u32 y = 0; y < kRes; ++y)
                for (u32 x = 0; x < kRes; ++x)
                    if (covered(x, y)) ++on;
            const auto meanRun = [&](bool alongX) {
                usize runs = 0, total = 0, cur = 0;
                for (u32 a = 0; a < kRes; ++a) {
                    cur = 0;
                    for (u32 b = 0; b < kRes; ++b) {
                        const bool c = alongX ? covered(b, a) : covered(a, b);
                        if (c) { ++cur; ++total; }
                        else if (cur) { ++runs; cur = 0; }
                    }
                    if (cur) ++runs;
                }
                return runs ? static_cast<f32>(total) / static_cast<f32>(runs) : 0.0f;
            };
            const f32 rx = meanRun(true), ry = meanRun(false);
            std::printf("  %-14s occupancy %5.1f%%  run along %6.1f px  across %6.1f px  "
                        "anisotropy %5.2f\n",
                        def.name.c_str(),
                        100.0f * static_cast<f32>(on) / static_cast<f32>(kRes * kRes), rx, ry,
                        ry > 0.01f ? rx / ry : 0.0f);
        }

        std::string name;
        for (char c : def.name) name += (c == ' ') ? '_' : static_cast<char>(std::tolower(c));
        WriteChannelPng(outDir / ("ref_" + name + "_color.png"), kRes, p.flatColor, 0, false);
        // Relief is the material canvas's B channel (0.5 = neutral).
        WriteChannelPng(outDir / ("ref_" + name + "_relief.png"), kRes, p.flatMaterial, 2, true);
        ++written;
    }

    report = "wrote " + std::to_string(written) + " brush swatch pairs";
    return written > 0;
}

} // namespace hbe::paint

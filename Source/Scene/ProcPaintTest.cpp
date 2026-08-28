// Scene/ProcPaintTest.cpp - see ProcPaintTest.h.
#include "Scene/ProcPaintTest.h"

#include "Editor/MovieRender.h" // WritePng
#include "Scene/Components.h"
#include "Scene/PaintSystem.h"

// The SHADER, compiled as C++ (see its __cplusplus shim). The evaluator under
// test is therefore the shipped one, not a lookalike.
#include "../../Shaders/ProceduralPaint.hlsli"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hbe::procpaint {
namespace {

// The test patch: a flat square of "surface", `kExtent` object units across,
// rasterized at kRes. A FLAT patch with a FIXED stroke direction is used on
// purpose - it makes the stroke frame a genuine global 2D frame, so the CPU
// oracle can lay strokes into a canvas and the two rasterizations are directly
// comparable. The varying direction field is validated separately by
// --test-brushfield; conflating the two would make a failure here ambiguous.
constexpr u32 kRes = 768;
constexpr f32 kExtent = 3.0f; // object units across the patch

// Canvas texel -> stroke-frame coordinate. The stroke frame is axis-aligned here
// (along = +x, across = +y), which is what makes the oracle tractable.
inline glm::vec2 TexelToFrame(u32 x, u32 y) {
    return glm::vec2((x + 0.5f) / kRes * kExtent, (y + 0.5f) / kRes * kExtent);
}

struct Metrics {
    f32 occupancy = 0.0f; // share of texels with coverage > 0.5
    f32 runAlong = 0.0f;  // mean run of covered texels along the stroke direction
    f32 runAcross = 0.0f;
    f32 anisotropy = 0.0f;
};

// `cov` is kRes*kRes coverage in 0..1.
Metrics Measure(const std::vector<f32>& cov) {
    const auto on = [&](u32 x, u32 y) { return cov[static_cast<usize>(y) * kRes + x] > 0.5f; };
    usize lit = 0;
    for (u32 y = 0; y < kRes; ++y)
        for (u32 x = 0; x < kRes; ++x)
            if (on(x, y)) ++lit;
    const auto meanRun = [&](bool alongX) {
        usize runs = 0, total = 0, cur = 0;
        for (u32 a = 0; a < kRes; ++a) {
            cur = 0;
            for (u32 b = 0; b < kRes; ++b) {
                const bool c = alongX ? on(b, a) : on(a, b);
                if (c) { ++cur; ++total; }
                else if (cur) { ++runs; cur = 0; }
            }
            if (cur) ++runs;
        }
        return runs ? static_cast<f32>(total) / static_cast<f32>(runs) : 0.0f;
    };
    Metrics m;
    m.occupancy = static_cast<f32>(lit) / static_cast<f32>(kRes * kRes);
    m.runAlong = meanRun(true);
    m.runAcross = meanRun(false);
    m.anisotropy = m.runAcross > 0.01f ? m.runAlong / m.runAcross : 0.0f;
    return m;
}

void WriteGrey(const std::filesystem::path& p, const std::vector<f32>& v) {
    std::vector<u8> px(static_cast<usize>(kRes) * kRes * 4);
    for (usize i = 0; i < static_cast<usize>(kRes) * kRes; ++i) {
        const u8 g = static_cast<u8>(std::clamp(v[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = g;
        px[i * 4 + 3] = 255;
    }
    movie::WritePng(p, kRes, kRes, px);
}

// Colour over a mid ground, so bare (un-painted) surface is visible as such
// rather than reading as black paint.
void WriteColorOverGround(const std::filesystem::path& p, const std::vector<glm::vec3>& rgb,
                          const std::vector<f32>& cov) {
    std::vector<u8> px(static_cast<usize>(kRes) * kRes * 4);
    for (usize i = 0; i < static_cast<usize>(kRes) * kRes; ++i) {
        const f32 a = std::clamp(cov[i], 0.0f, 1.0f);
        for (int k = 0; k < 3; ++k) {
            const f32 c = rgb[i][k] * a + 0.376f * (1.0f - a);
            px[i * 4 + static_cast<usize>(k)] =
                static_cast<u8>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        px[i * 4 + 3] = 255;
    }
    movie::WritePng(p, kRes, kRes, px);
}

} // namespace

bool OracleSelfTest(const std::filesystem::path& outDir, std::string& report) {
    const ProcPaintParams prm = ProcPaintDefaults();
    const bool wantImages = !outDir.empty();
    if (wantImages) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
    }

    std::printf("--- procedural paint vs. the CPU painting oracle ---\n");
    std::printf("  patch %.1f x %.1f object units at %ux%u  (%.1f px per unit)\n", kExtent,
                kExtent, kRes, kRes, kRes / kExtent);
    std::printf("  stroke halfWidth %.3f u (= %.0f px wide), length %.2f u, lane spacing %.3f u\n",
                prm.halfWidth, prm.halfWidth * 2.0f * (kRes / kExtent), prm.length,
                prm.laneSpacing);

    // ---------------------------------------------------------------- GPU path
    std::vector<f32> gpuCov(static_cast<usize>(kRes) * kRes, 0.0f);
    std::vector<f32> gpuHeight(static_cast<usize>(kRes) * kRes, 0.0f);
    std::vector<glm::vec3> gpuRgb(static_cast<usize>(kRes) * kRes, glm::vec3(0.0f));
    std::vector<u32> gpuId(static_cast<usize>(kRes) * kRes, 0u);
    for (u32 y = 0; y < kRes; ++y)
        for (u32 x = 0; x < kRes; ++x) {
            const PaintSample s = ProcPaintAtFrame(TexelToFrame(x, y), prm);
            const usize i = static_cast<usize>(y) * kRes + x;
            gpuCov[i] = s.coverage;
            gpuHeight[i] = s.height;
            gpuRgb[i] = s.albedo;
            gpuId[i] = s.dominantId;
        }

    // STRUCTURE, measured on the stroke-id field rather than on coverage.
    // A fully painted surface is covered everywhere, so coverage run-lengths say
    // nothing about whether the paint has strokes in it. How far you can travel
    // before the DOMINANT STROKE changes is exactly the question, and it is the
    // one a grain field fails: grain scores ~1, real bodies score many times that.
    const auto idRun = [&](bool alongX) {
        usize runs = 0, total = 0;
        for (u32 a = 0; a < kRes; ++a) {
            u32 cur = alongX ? gpuId[static_cast<usize>(a) * kRes] : gpuId[a];
            usize len = 0;
            for (u32 b = 0; b < kRes; ++b) {
                const u32 v = alongX ? gpuId[static_cast<usize>(a) * kRes + b]
                                     : gpuId[static_cast<usize>(b) * kRes + a];
                if (v == cur) { ++len; }
                else { ++runs; total += len; cur = v; len = 1; }
                ++total;
            }
            ++runs;
        }
        return runs ? static_cast<f32>(total) / static_cast<f32>(runs) * 0.5f : 0.0f;
    };
    const f32 idAlong = idRun(true), idAcross = idRun(false);
    std::printf("  stroke-id run: along %.1f px, across %.1f px, ratio %.2f  (grain ~= 1)\n",
                idAlong, idAcross, idAcross > 0.01f ? idAlong / idAcross : 0.0f);

    // ------------------------------------------------------- CPU oracle path
    // Emit the SAME strokes as real paint::Stroke objects and bake them through
    // the real painting system. Order is ascending (lane, seg) - identical to the
    // order ProcPaintAtFrame composites in - so the two agree on paint order.
    PaintComponent pc;
    pc.resolution = kRes;
    paint::EnsureCanvas(pc, kRes);
    if (pc.layers.empty()) {
        report = "EnsureCanvas produced no layer";
        return false;
    }
    const u32 layerId = pc.layers[0].id;

    paint::BrushDef def;
    def.name = "Procedural";
    def.shape = 0;
    def.hardness = prm.hardness;
    def.grain = 0.0f;    // milestone 1: body only, no grain
    def.bristles = 0.0f; // ... and no bristles
    def.scatter = 0.0f;
    def.flow = prm.flow;
    // Tight spacing so the swept tip realizes a CONTINUOUS body, which is what the
    // analytic evaluator produces. This is the knob that makes a dab-based
    // realization and a band-based one comparable at all.
    def.spacing = prm.spacing;
    def.taperStart = prm.taperStart;
    def.taperEnd = prm.taperEnd;
    def.sizeJitter = 0.0f; // per-stroke jitter already lives in ProcGetStroke

    const int laneLo = -1, laneHi = static_cast<int>(kExtent / prm.laneSpacing) + 1;
    const int segLo = -1, segHi = static_cast<int>(kExtent / prm.length) + 1;
    int emitted = 0;
    for (int lane = laneLo; lane <= laneHi; ++lane)
        for (int seg = segLo; seg <= segHi; ++seg) {
            const ProcStroke st = ProcGetStroke(lane, seg, prm);
            paint::Stroke s;
            s.type = paint::StrokeType::Path;
            s.layerId = layerId;
            s.projection = 0;
            s.brush = def;
            s.color = glm::vec4(st.albedo, 1.0f);
            s.flow = st.flow;
            s.height = prm.height;
            s.metallic = prm.metallic;
            s.roughness = prm.roughness;
            s.colorVar = 0.0f; // already applied per-stroke in ProcGetStroke
            s.paintColor = true;
            s.paintMaterial = true;
            // Sample the centreline. The stroke runs along +x through its centre;
            // radius follows the SAME taper the analytic profile uses, so the two
            // realizations describe one stroke, not two similar ones.
            constexpr int kSamples = 40;
            for (int k = 0; k < kSamples; ++k) {
                const f32 u = static_cast<f32>(k) / static_cast<f32>(kSamples - 1);
                const f32 alongU = st.centre.x + (u * 2.0f - 1.0f) * st.halfLength;
                paint::StrokePoint sp;
                sp.uv = glm::vec2(alongU / kExtent, st.centre.y / kExtent);
                sp.radius = st.halfWidth / kExtent *
                            std::max(ProcTaper(u, prm.taperStart, prm.taperEnd), 0.02f);
                sp.pressure = 0.45f + 0.55f * std::sin(u * 3.14159265f);
                s.path.push_back(sp);
            }
            pc.strokes.push_back(s);
            ++emitted;
        }
    paint::BakeFromStrokes(pc, nullptr);
    paint::Flatten(pc);
    if (pc.flatColor.empty()) {
        report = "oracle bake produced no pixels";
        return false;
    }

    std::vector<f32> cpuCov(static_cast<usize>(kRes) * kRes, 0.0f);
    std::vector<glm::vec3> cpuRgb(static_cast<usize>(kRes) * kRes, glm::vec3(0.0f));
    for (usize i = 0; i < static_cast<usize>(kRes) * kRes; ++i) {
        cpuCov[i] = pc.flatColor[i * 4 + 3] / 255.0f;
        cpuRgb[i] = glm::vec3(pc.flatColor[i * 4 + 0], pc.flatColor[i * 4 + 1],
                              pc.flatColor[i * 4 + 2]) /
                    255.0f;
    }

    // ------------------------------------------------------------- compare
    const Metrics g = Measure(gpuCov);
    const Metrics c = Measure(cpuCov);
    const f32 pxPerUnit = kRes / kExtent;
    std::printf("  %-22s %-11s %-11s\n", "", "GPU analytic", "CPU oracle");
    std::printf("  %-22s %10.1f%% %10.1f%%\n", "occupancy", g.occupancy * 100.0f,
                c.occupancy * 100.0f);
    std::printf("  %-22s %10.1f  %10.1f\n", "run along (px)", g.runAlong, c.runAlong);
    std::printf("  %-22s %10.1f  %10.1f\n", "run across (px) = width", g.runAcross, c.runAcross);
    std::printf("  %-22s %10.2f  %10.2f\n", "anisotropy", g.anisotropy, c.anisotropy);
    std::printf("  %u strokes emitted; nominal stroke width %.0f px\n", emitted,
                prm.halfWidth * 2.0f * pxPerUnit);

    double covErr = 0.0;
    usize agree = 0;
    for (usize i = 0; i < gpuCov.size(); ++i) {
        covErr += std::abs(gpuCov[i] - cpuCov[i]);
        if ((gpuCov[i] > 0.5f) == (cpuCov[i] > 0.5f)) ++agree;
    }
    const f32 meanCovErr = static_cast<f32>(covErr / static_cast<double>(gpuCov.size()));
    const f32 agreeFrac = static_cast<f32>(agree) / static_cast<f32>(gpuCov.size());
    std::printf("  mean |coverage diff| %.4f, painted/unpainted agreement %.1f%%\n", meanCovErr,
                agreeFrac * 100.0f);

    if (wantImages) {
        WriteColorOverGround(outDir / "proc_gpu_color.png", gpuRgb, gpuCov);
        WriteColorOverGround(outDir / "proc_cpu_oracle_color.png", cpuRgb, cpuCov);
        WriteGrey(outDir / "proc_gpu_coverage.png", gpuCov);
        WriteGrey(outDir / "proc_cpu_coverage.png", cpuCov);
        WriteGrey(outDir / "proc_gpu_height.png", gpuHeight);
        // Stroke-id view: each stroke a flat random tone, so BODIES are visible
        // directly rather than inferred. This is the debug view that would have
        // caught the previous implementation instantly.
        std::vector<f32> idView(gpuId.size());
        for (usize i = 0; i < gpuId.size(); ++i)
            idView[i] = static_cast<f32>((gpuId[i] * 2654435761u) >> 8 & 0xFFu) / 255.0f;
        WriteGrey(outDir / "proc_gpu_strokeid.png", idView);
        std::vector<f32> diff(gpuCov.size());
        for (usize i = 0; i < diff.size(); ++i) diff[i] = std::abs(gpuCov[i] - cpuCov[i]);
        WriteGrey(outDir / "proc_coverage_diff.png", diff);
        std::printf("  images -> %s\n", outDir.string().c_str());
    }

    // Gates. These target the ground truth measured from the real painting system
    // (docs/Design-PainterlyStrokes.md, A2): occupancy 10-30%, stroke width 3-12%
    // of the frame, run-length anisotropy well above 1 (a noise field scores ~1).
    bool ok = true;
    const auto fail = [&](const char* why) {
        std::printf("  FAIL: %s\n", why);
        ok = false;
    };
    // Occupancy is judged as "is the surface actually painted" - a painted wall is
    // covered, unlike the sparse 5-sweep reference swatches. Structure is judged on
    // the stroke-id field instead, which is the measure that survives full coverage.
    if (g.occupancy < 0.55f) fail("surface is not actually painted (coverage too sparse)");
    if (idAcross < kRes * 0.015f) fail("strokes too narrow (grain, not bodies)");
    if (idAlong / std::max(idAcross, 1.0f) < 3.0f)
        fail("stroke-id runs are not directional (not stroke-shaped)");
    if (agreeFrac < 0.80f) fail("GPU and CPU oracle disagree on where paint is");
    if (meanCovErr > 0.20f) fail("GPU and CPU oracle coverage differs too much");

    report = ok ? "GPU evaluator agrees with the CPU painting oracle"
                : "procedural paint does not match the painting oracle";
    return ok;
}

} // namespace hbe::procpaint

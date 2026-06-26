// Assets/StrokeGen.cpp - see StrokeGen.h.
#include "Assets/StrokeGen.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <random>

namespace hbe::stroke {

void GenerateSurfaceStrokes(const MeshData& mesh, float density, u32 maxStrokes,
                            std::vector<StrokeInstance>& out) {
    out.clear();
    if (mesh.indices.size() < 3 || mesh.vertices.empty() || density <= 0.0f || maxStrokes == 0)
        return;

    const std::vector<Vertex>& V = mesh.vertices;
    const std::vector<u32>& I = mesh.indices;
    const u32 triCount = static_cast<u32>(I.size() / 3);

    // Per-triangle object-space area + total, so strokes distribute evenly by area
    // (big walls get more strokes than tiny faces) and we can hit a global cap.
    std::vector<float> area(triCount);
    double total = 0.0;
    for (u32 t = 0; t < triCount; ++t) {
        const glm::vec3& a = V[I[t * 3 + 0]].position;
        const glm::vec3& b = V[I[t * 3 + 1]].position;
        const glm::vec3& c = V[I[t * 3 + 2]].position;
        const float ar = 0.5f * glm::length(glm::cross(b - a, c - a));
        area[t] = ar;
        total += ar;
    }
    if (total <= 0.0) return;

    const float target = std::min(static_cast<float>(total * density),
                                  static_cast<float>(maxStrokes));
    const float perUnit = static_cast<float>(target / total);

    // Deterministic per-mesh RNG (stable cache across frames/sessions).
    std::mt19937 rng(0x5EED1234u ^ static_cast<u32>(V.size()) ^
                     (static_cast<u32>(I.size()) << 16));
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    out.reserve(static_cast<size_t>(target) + 16);

    for (u32 t = 0; t < triCount && out.size() < maxStrokes; ++t) {
        // Fractional stroke count -> stochastic rounding so density is unbiased.
        const float fc = area[t] * perUnit;
        int n = static_cast<int>(fc);
        if (U(rng) < (fc - static_cast<float>(n))) ++n;
        if (n <= 0) continue;

        const Vertex& va = V[I[t * 3 + 0]];
        const Vertex& vb = V[I[t * 3 + 1]];
        const Vertex& vc = V[I[t * 3 + 2]];
        for (int k = 0; k < n && out.size() < maxStrokes; ++k) {
            // Uniform barycentric sample (reflect into the triangle).
            float r1 = U(rng), r2 = U(rng);
            if (r1 + r2 > 1.0f) { r1 = 1.0f - r1; r2 = 1.0f - r2; }
            const float w0 = 1.0f - r1 - r2, w1 = r1, w2 = r2;

            StrokeInstance s;
            s.posOS = va.position * w0 + vb.position * w1 + vc.position * w2;

            glm::vec3 nrm = va.normal * w0 + vb.normal * w1 + vc.normal * w2;
            s.normalOS = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : glm::vec3(0, 1, 0);

            glm::vec3 tan = glm::vec3(va.tangent) * w0 + glm::vec3(vb.tangent) * w1 +
                            glm::vec3(vc.tangent) * w2;
            if (glm::length(tan) < 1e-5f) tan = vb.position - va.position; // fallback: an edge
            s.tangentOS = glm::length(tan) > 1e-6f ? glm::normalize(tan) : glm::vec3(1, 0, 0);

            s.uv = va.uv * w0 + vb.uv * w1 + vc.uv * w2;
            s.seed = U(rng);
            out.push_back(s);
        }
    }
}

} // namespace hbe::stroke

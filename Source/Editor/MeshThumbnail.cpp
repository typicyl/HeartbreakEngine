// Editor/MeshThumbnail.cpp
#include "Editor/MeshThumbnail.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <limits>

namespace hbe::editor {

std::vector<u32> RasterizeMeshThumbnail(const Model& model, u32 size,
                                        const glm::vec3& tint) {
    std::vector<u32> image(static_cast<usize>(size) * size, 0u); // transparent
    if (model.empty() || size == 0) return image;

    // Combined bounds.
    glm::vec3 bmin(1e30f), bmax(-1e30f);
    for (const MeshData& md : model) {
        glm::vec3 mn, mx;
        ComputeBounds(md, mn, mx);
        bmin = glm::min(bmin, mn);
        bmax = glm::max(bmax, mx);
    }
    if (bmax.x < bmin.x) return image;
    const glm::vec3 center = (bmin + bmax) * 0.5f;
    const f32 radius = glm::max(glm::length(bmax - bmin) * 0.5f, 1e-4f);

    // Orthographic three-quarter view.
    const glm::vec3 eye = center + glm::normalize(glm::vec3(1.0f, 0.75f, 1.0f)) * radius * 3.0f;
    const glm::mat4 view = glm::lookAtRH(eye, center, glm::vec3(0, 1, 0));
    const f32 half = radius * 1.1f;
    const f32 fsize = static_cast<f32>(size);

    // View-space position -> pixel + depth.
    const auto project = [&](const glm::vec3& p, glm::vec3& out) {
        const glm::vec3 v = glm::vec3(view * glm::vec4(p, 1.0f));
        out.x = (v.x / half * 0.5f + 0.5f) * fsize;
        out.y = (0.5f - v.y / half * 0.5f) * fsize;
        out.z = -v.z; // distance from camera (smaller = closer)
    };

    std::vector<f32> depth(static_cast<usize>(size) * size,
                           std::numeric_limits<f32>::max());
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.45f, 1.0f, 0.55f));

    for (const MeshData& md : model) {
        const usize triCount = md.indices.size() / 3;
        for (usize t = 0; t < triCount; ++t) {
            const glm::vec3& a = md.vertices[md.indices[t * 3 + 0]].position;
            const glm::vec3& b = md.vertices[md.indices[t * 3 + 1]].position;
            const glm::vec3& c = md.vertices[md.indices[t * 3 + 2]].position;

            glm::vec3 pa, pb, pc;
            project(a, pa);
            project(b, pb);
            project(c, pc);

            // Flat shading from the world-space face normal.
            const glm::vec3 n = glm::cross(b - a, c - a);
            const f32 len = glm::length(n);
            if (len < 1e-12f) continue;
            const f32 ndl = glm::max(glm::dot(n / len, lightDir), 0.0f);
            const f32 shade = 0.30f + 0.70f * ndl;
            const u32 r = static_cast<u32>(glm::clamp(tint.r * 255.0f * shade, 0.0f, 255.0f));
            const u32 g = static_cast<u32>(glm::clamp(tint.g * 255.0f * shade, 0.0f, 255.0f));
            const u32 bl = static_cast<u32>(glm::clamp(tint.b * 255.0f * shade, 0.0f, 255.0f));
            const u32 color = 0xFF000000u | (bl << 16) | (g << 8) | r;

            // Bounding box of the triangle, clamped to the image.
            const int x0 = glm::max(0, static_cast<int>(glm::floor(
                                           glm::min(pa.x, glm::min(pb.x, pc.x)))));
            const int x1 = glm::min(static_cast<int>(size) - 1,
                                    static_cast<int>(glm::ceil(
                                        glm::max(pa.x, glm::max(pb.x, pc.x)))));
            const int y0 = glm::max(0, static_cast<int>(glm::floor(
                                           glm::min(pa.y, glm::min(pb.y, pc.y)))));
            const int y1 = glm::min(static_cast<int>(size) - 1,
                                    static_cast<int>(glm::ceil(
                                        glm::max(pa.y, glm::max(pb.y, pc.y)))));
            if (x0 > x1 || y0 > y1) continue;

            const f32 area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
            if (std::abs(area) < 1e-9f) continue;
            const f32 invArea = 1.0f / area;

            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const f32 px = x + 0.5f, py = y + 0.5f;
                    const f32 w0 = ((pb.x - px) * (pc.y - py) - (pb.y - py) * (pc.x - px)) * invArea;
                    const f32 w1 = ((pc.x - px) * (pa.y - py) - (pc.y - py) * (pa.x - px)) * invArea;
                    const f32 w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const f32 z = w0 * pa.z + w1 * pb.z + w2 * pc.z;
                    const usize idx = static_cast<usize>(y) * size + x;
                    if (z < depth[idx]) {
                        depth[idx] = z;
                        image[idx] = color;
                    }
                }
            }
        }
    }
    return image;
}

} // namespace hbe::editor

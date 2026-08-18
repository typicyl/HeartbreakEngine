// Editor/ProctreeImport.cpp - parametric ("proctree-style") tree importer (editor-only).
#include "Editor/ProctreeImport.h"
#include "Core/Rng.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

namespace hbe::editor {
namespace {

// Grows ONE branch of `segments` nodes from `pos` along `dir`, then (pre-order) recurses into
// `childCount` thinner/shorter/angled child branches and, at a twig, sprinkles leaf clusters.
// Emitting this branch's nodes before its children keeps every parent index < its node index.
void GrowBranch(veg::PlantSkeleton& s, Rng& rng, glm::vec3 pos, glm::vec3 dir, f32 length,
                f32 radius, u8 order, i32 parentIdx, const ProctreeParams& p) {
    dir = glm::normalize(dir);
    const u32 seg = glm::max(2u, p.segments);
    const f32 segLen = length / static_cast<f32>(seg);
    const bool isTwig = (static_cast<u32>(order) + 1u >= p.levels);
    const veg::PlantPart kind = order == 0 ? veg::PlantPart::Trunk
                                : isTwig   ? veg::PlantPart::Twig
                                           : veg::PlantPart::Branch;

    std::vector<i32> chain;
    chain.reserve(seg);
    i32 cur = parentIdx;
    glm::vec3 d = dir;
    glm::vec3 pt = pos;
    for (u32 i = 1; i <= seg; ++i) {
        // Wobble the growth direction and bias it downward more at higher orders (gravity).
        const glm::vec3 jitter(rng.Signed(), rng.Signed(), rng.Signed());
        d = glm::normalize(d + jitter * p.wobble +
                           glm::vec3(0.0f, -1.0f, 0.0f) * (p.droop * static_cast<f32>(order) * 0.15f));
        pt += d * segLen;
        const f32 t = static_cast<f32>(i) / static_cast<f32>(seg);
        const f32 r = glm::max(0.004f, radius * (1.0f - t * 0.5f)); // taper along the branch
        cur = static_cast<i32>(s.AddNode(pt, cur, r, order, kind));
        chain.push_back(cur);
    }
    if (chain.empty()) return;

    // Leaf clusters clumped around the twig tip (BuildTreeMesh cards LeafCluster nodes).
    if (isTwig) {
        const glm::vec3 tip = s.pos[chain.back()];
        const u32 clusters = 3u + static_cast<u32>(rng.NextFloat() * 4.0f);
        for (u32 c = 0; c < clusters; ++c) {
            const glm::vec3 off(rng.Signed(), rng.Signed(), rng.Signed());
            s.AddNode(tip + off * (p.leafSize * 0.6f), chain.back(), p.leafSize, order,
                      veg::PlantPart::LeafCluster);
        }
    }

    // Recurse into child branches, attached in the upper half of this branch.
    if (static_cast<u32>(order) + 1u < p.levels) {
        glm::vec3 up = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 side = glm::normalize(glm::cross(d, up));
        const f32 ang = glm::radians(p.branchAngle);
        for (u32 c = 0; c < p.childCount; ++c) {
            const f32 f = 0.5f + 0.5f * (static_cast<f32>(c + 1) / static_cast<f32>(p.childCount + 1));
            const usize ai = glm::min(static_cast<usize>(f * static_cast<f32>(chain.size())),
                                      chain.size() - 1);
            const i32 attach = chain[ai];
            const f32 azimuth = glm::radians(p.twist) * static_cast<f32>(c) + rng.NextFloat() * 0.6f;
            const glm::vec3 outAxis = glm::angleAxis(azimuth, d) * side;
            const glm::vec3 childDir = glm::normalize(d * std::cos(ang) + outAxis * std::sin(ang));
            GrowBranch(s, rng, s.pos[attach], childDir, length * p.lengthFalloff,
                       radius * p.taper, static_cast<u8>(order + 1), attach, p);
        }
    }
}

} // namespace

bool ImportProctreeSkeleton(const ProctreeParams& params, veg::PlantSkeleton& out) {
    out.Clear();
    if (params.levels == 0 || params.segments == 0 || params.trunkHeight <= 0.0f) return false;
    out.sourceSeed = params.seed;
    Rng rng(params.seed ? params.seed : 1u);
    const u32 root =
        out.AddNode(glm::vec3(0.0f), -1, glm::max(0.004f, params.trunkRadius), 0, veg::PlantPart::Trunk);
    GrowBranch(out, rng, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), params.trunkHeight,
               params.trunkRadius, 0, static_cast<i32>(root), params);
    return out.NodeCount() > 1 && out.Validate();
}

bool ProctreeSelfTest() {
    ProctreeParams p;
    veg::PlantSkeleton a, b;
    if (!ImportProctreeSkeleton(p, a) || !ImportProctreeSkeleton(p, b)) return false;
    if (veg::HashSkeleton(a) != veg::HashSkeleton(b)) return false; // deterministic
    if (!a.Validate()) return false;                                // parent invariant

    bool woody = false, leaf = false;
    for (const u8 k : a.kind) {
        const auto part = static_cast<veg::PlantPart>(k);
        if (part == veg::PlantPart::Trunk || part == veg::PlantPart::Branch ||
            part == veg::PlantPart::Twig)
            woody = true;
        if (part == veg::PlantPart::LeafCluster) leaf = true;
    }
    if (!woody || !leaf) return false; // meshable: both tubes and foliage
    for (const glm::vec3& v : a.pos)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;

    ProctreeParams p2 = p;
    p2.seed = 24601;
    veg::PlantSkeleton c;
    if (!ImportProctreeSkeleton(p2, c)) return false;
    if (veg::HashSkeleton(a) == veg::HashSkeleton(c)) return false; // seed-sensitive
    return true;
}

} // namespace hbe::editor

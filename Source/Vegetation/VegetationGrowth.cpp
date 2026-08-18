// Vegetation/VegetationGrowth.cpp - incremental structural growth.
#include "Vegetation/VegetationGrowth.h"
#include "Core/Rng.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::veg {

void GrowSkeleton(PlantSkeleton& skel, const Species& sp, f32 a0, f32 a1) {
    const u32 n0 = skel.NodeCount();
    if (n0 == 0 || a1 <= a0) return;
    a0 = glm::clamp(a0, 0.001f, 1.0f);
    a1 = glm::clamp(a1, 0.001f, 1.0f);
    const f32 dAge = a1 - a0;

    Rng rng(skel.sourceSeed ^ 0x67726F7708ull); // "grow"

    // Thicken existing WOOD toward its mature radius (bounded by the trunk). Leaf/flower/
    // fruit clusters carry their own size (often larger than the trunk), so they are left
    // alone - thickening + capping them would shrink the canopy.
    const f32 thick = glm::clamp(a1 / a0, 1.0f, 1.8f);
    for (u32 i = 0; i < n0; ++i) {
        const PlantPart k = static_cast<PlantPart>(skel.kind[i]);
        if (k == PlantPart::Trunk || k == PlantPart::Branch || k == PlantPart::Twig ||
            k == PlantPart::Root)
            skel.radius[i] = glm::min(skel.radius[i] * thick, sp.trunkRadius);
    }

    // Extend the GROWING tips: woody nodes with no children that have not reached the
    // species' max branch order. Snapshot them first (adding nodes mutates childCount).
    std::vector<u32> childCount(n0, 0);
    for (u32 i = 0; i < n0; ++i)
        if (skel.parent[i] >= 0) childCount[skel.parent[i]]++;

    std::vector<u32> tips;
    for (u32 i = 0; i < n0; ++i) {
        const PlantPart k = static_cast<PlantPart>(skel.kind[i]);
        const bool woody = (k == PlantPart::Trunk || k == PlantPart::Branch || k == PlantPart::Twig);
        if (woody && childCount[i] == 0 && skel.order[i] < sp.maxBranchOrder) tips.push_back(i);
    }

    const f32 extLen = glm::max(sp.maxHeight * 0.05f * dAge, 0.04f);
    for (u32 tip : tips) {
        Rng tr = rng.Split(tip);
        const i32 par = skel.parent[tip];
        glm::vec3 dir = (par >= 0)
                            ? glm::normalize(skel.pos[tip] - skel.pos[par] + glm::vec3(0, 1e-4f, 0))
                            : glm::vec3(0.0f, 1.0f, 0.0f);
        dir = glm::normalize(dir + glm::vec3(tr.Signed(), tr.Signed() * 0.3f, tr.Signed()) * 0.3f);
        const glm::vec3 np = skel.pos[tip] + dir * extLen;
        const u8 ord = static_cast<u8>(glm::min<u32>(sp.maxBranchOrder, skel.order[tip] + 1u));
        const u32 ext = skel.AddNode(np, static_cast<i32>(tip), skel.radius[tip] * 0.7f, ord,
                                     PlantPart::Twig, a1);
        if (sp.leafDensity > 0.0f)
            skel.AddNode(np + dir * (sp.leafSize * 0.5f), static_cast<i32>(ext), sp.leafSize, ord,
                         PlantPart::LeafCluster, a1);
    }
}

} // namespace hbe::veg

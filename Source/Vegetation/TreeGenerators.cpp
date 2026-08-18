// Vegetation/TreeGenerators.cpp - the two first-class in-house structure generators.
//
//   * SpaceColonizationGenerator ("spacecol") - Runions et al. 2007: attractor points in
//     the crown pull the skeleton outward, producing natural, competition-shaped
//     branching. Also the natural GROWTH model (it is inherently incremental).
//   * LSystemGenerator ("lsystem") - a bracketed 3D L-system with a turtle interpreter,
//     for rule-based, self-similar species.
//
// Both are pure CPU, job-safe and DETERMINISTIC (same species+seed -> identical skeleton
// on every run/thread/compiler): all randomness comes from Core/Rng with per-consumer
// Split salts, never std::random or global state. Both emit the shared PlantSkeleton, so
// the mesher/wind/LOD/growth/damage treat them identically. Neither is a library - the
// algorithms are public-domain math (design doc dependency table).
#include "Vegetation/VegetationBackends.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Core/Rng.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace hbe::veg {
namespace {

// Assigns node radii by the pipe model (da Vinci / Murray): a parent's cross-section
// equals the sum of its children's, so radius ~ (tip-count)^(1/n). Traverses leaves->root
// using the invariant that a node's parent always has a LOWER index than the node.
void AssignPipeRadii(PlantSkeleton& s, f32 trunkRadius) {
    const u32 n = s.NodeCount();
    if (n == 0) return;
    std::vector<u32> childCount(n, 0);
    for (u32 i = 0; i < n; ++i)
        if (s.parent[i] >= 0) childCount[s.parent[i]]++;

    std::vector<f32> weight(n, 0.0f); // number of tip descendants
    for (i32 i = static_cast<i32>(n) - 1; i >= 0; --i) {
        if (childCount[i] == 0) weight[i] = 1.0f; // a tip
        if (s.parent[i] >= 0) weight[s.parent[i]] += weight[i];
    }
    const f32 rootW = std::max(1.0f, weight[0]);
    constexpr f32 kPipe = 2.3f;
    for (u32 i = 0; i < n; ++i) {
        const f32 frac = std::pow(weight[i] / rootW, 1.0f / kPipe);
        s.radius[i] = std::max(0.004f, trunkRadius * frac);
    }
}

// Classifies each node's PlantPart from its branch order, then clothes the FINE outer
// branches (twig-order nodes) and every tip in several leaf CLUMPS, so the canopy reads as
// a mass of foliage instead of a few bare specks at the very ends. Density-gated.
void ClassifyAndLeaf(PlantSkeleton& s, const Species& sp, Rng leafRng) {
    const u32 n = s.NodeCount();
    std::vector<u32> childCount(n, 0);
    for (u32 i = 0; i < n; ++i)
        if (s.parent[i] >= 0) childCount[s.parent[i]]++;

    for (u32 i = 0; i < n; ++i) {
        const u8 ord = s.order[i];
        s.kind[i] = static_cast<u8>(ord == 0 ? PlantPart::Trunk
                                    : (ord >= sp.maxBranchOrder ? PlantPart::Twig
                                                                : PlantPart::Branch));
    }
    if (sp.leafDensity <= 0.0f) return;

    // Leaves grow on the fine outer wood (twig order) AND at every branch tip. Snapshot
    // the target nodes first, because adding leaf children below mutates the arrays.
    const u8 leafOrder = static_cast<u8>(glm::max(1, static_cast<i32>(sp.maxBranchOrder) - 1));
    std::vector<u32> leafy;
    for (u32 i = 0; i < n; ++i) {
        const bool twig = s.order[i] >= leafOrder;
        const bool tip = childCount[i] == 0 && s.order[i] >= 1;
        if (twig || tip) leafy.push_back(i);
    }

    // Several clumps per node, each a real CLUMP (not a single 12 cm leaf), scattered in a
    // small volume around the wood so they overlap into a canopy.
    const u32 perNode = 3u + static_cast<u32>(glm::clamp(sp.leafDensity, 0.0f, 1.0f) * 5.0f);
    const f32 clumpSize = sp.leafSize * 4.5f;
    const f32 spread = clumpSize * 1.1f;
    for (u32 i : leafy) {
        const glm::vec3 p = s.pos[i];
        for (u32 k = 0; k < perNode; ++k) {
            const glm::vec3 jit(leafRng.Signed(), leafRng.Signed() * 0.7f, leafRng.Signed());
            const f32 sz = clumpSize * (0.7f + 0.6f * leafRng.NextFloat());
            s.AddNode(p + jit * spread, static_cast<i32>(i), sz, s.order[i],
                      PlantPart::LeafCluster, s.age.empty() ? 1.0f : s.age[i]);
        }
    }
}

// ===================================================================================
// Space colonization
// ===================================================================================
class SpaceColonizationGenerator final : public IPlantGenerator {
public:
    const char* Name() const override { return "spacecol"; }

    bool Generate(const PlantGenParams& gp, const Species& sp, PlantSkeleton& out) override {
        out.Clear();
        out.sourceSeed = gp.seed;

        const f32 age = glm::clamp(gp.age01, 0.05f, 1.0f);
        const f32 height = sp.maxHeight * age;
        const f32 crownR = glm::max(0.5f, sp.crownWidth * 0.5f * age);
        const f32 crownBase = height * 0.35f;
        const f32 crownTop = height * 1.05f;
        const f32 crownMidY = 0.5f * (crownBase + crownTop);
        const f32 crownHalfH = 0.5f * (crownTop - crownBase);

        Rng rng(gp.seed);
        Rng attrRng = rng.Split(0x1);

        // Attractors uniformly in the crown ellipsoid (rejection sample the unit sphere).
        // The COUNT scales with age, so a young plant is genuinely SPARSE (few branches)
        // and a mature one fills its crown - structural GROWTH, not a scaled copy.
        const f32 fullAttr = 110.0f + glm::clamp(sp.branchDensity, 0.0f, 1.0f) * 440.0f;
        const u32 nAttr = 40u + static_cast<u32>(fullAttr * age);
        std::vector<glm::vec3> attractors;
        attractors.reserve(nAttr);
        u32 guard = 0;
        while (attractors.size() < nAttr && guard < nAttr * 20u) {
            ++guard;
            const glm::vec3 u(attrRng.Signed(), attrRng.Signed(), attrRng.Signed());
            if (glm::dot(u, u) > 1.0f) continue;
            attractors.push_back(glm::vec3(u.x * crownR, crownMidY + u.y * crownHalfH,
                                           u.z * crownR));
        }

        // Growth parameters. segLen + killDist are ABSOLUTE (per species, age-independent)
        // so a larger mature crown genuinely needs MORE segments than a small young one -
        // that is what makes age add structure (growth) instead of scaling one fixed tree.
        // The crown SIZE and attractor COUNT are what scale with age (above).
        const f32 segLen = glm::max(0.05f, sp.maxHeight * 0.04f);
        const f32 influence = crownR * 1.6f;
        const f32 killDist = segLen * 2.0f;

        // Node arrays (temp). parent index is ALWAYS < node index (children appended).
        std::vector<glm::vec3> pos;
        std::vector<i32> parent;
        std::vector<u8> order;
        std::vector<u8> childCnt;
        auto addNode = [&](const glm::vec3& p, i32 par, u8 ord) -> i32 {
            pos.push_back(p);
            parent.push_back(par);
            order.push_back(ord);
            childCnt.push_back(0);
            if (par >= 0 && childCnt[par] < 255) childCnt[par]++;
            return static_cast<i32>(pos.size()) - 1;
        };

        // Seed trunk: a vertical column up to the crown base.
        i32 prev = addNode(glm::vec3(0.0f), -1, 0);
        for (f32 y = segLen; y < crownBase; y += segLen)
            prev = addNode(glm::vec3(0.0f, y, 0.0f), prev, 0);

        const u32 kMaxNodes = 4000;
        const u32 kMaxIter = 400;
        std::vector<glm::vec3> grow;   // accumulated pull direction per node
        std::vector<u32> pulls;        // attractor count per node
        for (u32 iter = 0; iter < kMaxIter && !attractors.empty(); ++iter) {
            const u32 nodeN = static_cast<u32>(pos.size());
            grow.assign(nodeN, glm::vec3(0.0f));
            pulls.assign(nodeN, 0);

            // Each attractor pulls its single nearest node (within influence).
            for (const glm::vec3& a : attractors) {
                i32 best = -1;
                f32 bestD2 = influence * influence;
                for (u32 ni = 0; ni < nodeN; ++ni) {
                    const glm::vec3 d = a - pos[ni];
                    const f32 d2 = glm::dot(d, d);
                    if (d2 < bestD2) { bestD2 = d2; best = static_cast<i32>(ni); }
                }
                if (best >= 0) {
                    grow[best] += glm::normalize(a - pos[best]);
                    pulls[best]++;
                }
            }

            bool grew = false;
            for (u32 ni = 0; ni < nodeN; ++ni) {
                if (pulls[ni] == 0) continue;
                glm::vec3 dir = grow[ni] / static_cast<f32>(pulls[ni]);
                dir += glm::vec3(0.0f, 0.15f, 0.0f); // slight upward bias (phototropism)
                const f32 l = glm::length(dir);
                if (l < 1e-4f) continue;
                dir /= l;
                const glm::vec3 np = pos[ni] + dir * segLen;
                // A second+ child off a node starts a higher branch order.
                const u8 ord = static_cast<u8>(
                    glm::min<u32>(sp.maxBranchOrder,
                                  order[ni] + (childCnt[ni] > 0 ? 1u : 0u)));
                addNode(np, static_cast<i32>(ni), ord);
                grew = true;
                if (pos.size() >= kMaxNodes) break;
            }
            if (!grew || pos.size() >= kMaxNodes) break;

            // Remove satisfied attractors (near any node).
            std::vector<glm::vec3> keep;
            keep.reserve(attractors.size());
            const f32 kill2 = killDist * killDist;
            for (const glm::vec3& a : attractors) {
                bool dead = false;
                for (const glm::vec3& p : pos) {
                    const glm::vec3 d = a - p;
                    if (glm::dot(d, d) < kill2) { dead = true; break; }
                }
                if (!dead) keep.push_back(a);
            }
            attractors.swap(keep);
        }

        // Emit into the skeleton (radii + kinds assigned below).
        out.Reserve(static_cast<u32>(pos.size()));
        for (usize i = 0; i < pos.size(); ++i)
            out.AddNode(pos[i], parent[i], 0.01f, order[i], PlantPart::Branch, age);

        AssignPipeRadii(out, sp.trunkRadius * age);
        ClassifyAndLeaf(out, sp, rng.Split(0x2));
        return out.NodeCount() > 0 && out.Validate();
    }
};

// ===================================================================================
// L-system (bracketed 3D turtle)
// ===================================================================================
class LSystemGenerator final : public IPlantGenerator {
public:
    const char* Name() const override { return "lsystem"; }

    bool Generate(const PlantGenParams& gp, const Species& sp, PlantSkeleton& out) override {
        out.Clear();
        out.sourceSeed = gp.seed;

        const f32 age = glm::clamp(gp.age01, 0.05f, 1.0f);
        // Iteration depth scales with age: a young plant runs fewer rewrites (a simpler,
        // sparser structure) and a mature one reaches full depth - structural growth.
        const u32 maxIters = glm::clamp<u32>(sp.maxBranchOrder, 2u, 4u);
        const u32 iterations =
            glm::clamp<u32>(static_cast<u32>(static_cast<f32>(maxIters) * age + 0.5f), 2u, maxIters);

        // Production: a shoot F becomes a longer LEADING axis ("FF" -> a real trunk that
        // grows 2^iterations segments tall) that forks into three lateral branches. '[' ']'
        // bracket a branch; +/-,&/^ are turtle rotations. Expanded up to `iterations` with a
        // hard char cap so a runaway species cannot explode (branching factor 5/iteration).
        std::string s = "F";
        const std::string rule = "FF[+F][-F][&F]";
        for (u32 it = 0; it < iterations; ++it) {
            std::string next;
            next.reserve(s.size() * 4);
            for (char c : s) {
                if (c == 'F') next += rule;
                else next += c;
                if (next.size() > 200000) break;
            }
            s.swap(next);
            if (s.size() > 200000) break;
        }

        const f32 baseLen = sp.maxHeight * age /
                            std::pow(2.0f, static_cast<f32>(iterations)); // so depth ~ maxHeight
        const f32 angle = glm::radians(sp.branchAngleDeg);
        Rng jitterRng(gp.seed);
        Rng jr = jitterRng.Split(0x1);

        struct TurtleState {
            glm::vec3 pos;
            glm::quat rot;   // orientation; forward = rot * +Y
            u8 order;
            i32 node;        // parent node index
            f32 len;
        };
        TurtleState st;
        st.pos = glm::vec3(0.0f);
        st.rot = glm::quat(1, 0, 0, 0);
        st.order = 0;
        st.len = baseLen;
        st.node = out.AddNode(st.pos, -1, 0.01f, 0, PlantPart::Branch, age);

        std::vector<TurtleState> stack;
        const glm::vec3 up(0, 1, 0), right(1, 0, 0), fwdAxis(0, 0, 1);
        auto rotate = [&](glm::quat& q, const glm::vec3& axis, f32 a) {
            q = glm::normalize(q * glm::angleAxis(a, axis));
        };

        for (char c : s) {
            switch (c) {
                case 'F': {
                    const glm::vec3 dir = glm::normalize(st.rot * up);
                    const f32 jl = st.len * (0.85f + 0.3f * jr.NextFloat());
                    st.pos += dir * jl;
                    const u8 ord = glm::min<u32>(sp.maxBranchOrder, st.order);
                    st.node = out.AddNode(st.pos, st.node, 0.01f, ord, PlantPart::Branch, age);
                    break;
                }
                case '+': rotate(st.rot, up,       angle * (0.8f + 0.4f * jr.NextFloat())); break;
                case '-': rotate(st.rot, up,      -angle * (0.8f + 0.4f * jr.NextFloat())); break;
                case '&': rotate(st.rot, right,    angle * (0.8f + 0.4f * jr.NextFloat())); break;
                case '^': rotate(st.rot, right,   -angle * (0.8f + 0.4f * jr.NextFloat())); break;
                case '/': rotate(st.rot, fwdAxis,  angle); break;
                case '\\':rotate(st.rot, fwdAxis, -angle); break;
                case '[':
                    stack.push_back(st);
                    st.order = static_cast<u8>(glm::min<u32>(sp.maxBranchOrder, st.order + 1u));
                    st.len *= 0.7f;
                    break;
                case ']':
                    if (!stack.empty()) { st = stack.back(); stack.pop_back(); }
                    break;
                default: break;
            }
            if (out.NodeCount() > 60000) break; // hard cap
        }

        AssignPipeRadii(out, sp.trunkRadius * age);
        ClassifyAndLeaf(out, sp, jitterRng.Split(0x2));
        return out.NodeCount() > 0 && out.Validate();
    }
};

} // namespace

std::unique_ptr<IPlantGenerator> MakeSpaceColonizationGenerator() {
    return std::make_unique<SpaceColonizationGenerator>();
}
std::unique_ptr<IPlantGenerator> MakeLSystemGenerator() {
    return std::make_unique<LSystemGenerator>();
}

} // namespace hbe::veg

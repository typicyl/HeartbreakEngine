// Scene/BrushSystem.cpp - see BrushSystem.h.
#include "Scene/BrushSystem.h"

#include "Assets/MeshCsg.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::brush {
namespace {

csg::Box BodyBox(const BrushComponent& bc) {
    csg::Box b;
    b.transform = glm::mat4(1.0f); // the mesh is in the additive brush's OWN local frame
    b.half = glm::max(bc.halfExtents, glm::vec3(1e-4f));
    return b;
}

} // namespace

MeshData BuildEntityMesh(const Scene& scene, entt::entity e) {
    const entt::registry& reg = scene.Registry();
    const BrushComponent* bc = reg.try_get<const BrushComponent>(e);
    if (!bc || bc->op != static_cast<int>(csg::Op::Add)) return {};

    const glm::mat4 invA = glm::inverse(scene.WorldMatrix(e));

    // Every subtractive brush, expressed in THIS brush's local frame, becomes a cutter.
    std::vector<csg::Box> cutters;
    for (const entt::entity se : reg.view<const BrushComponent>()) {
        if (se == e) continue;
        const BrushComponent& sbc = reg.get<const BrushComponent>(se);
        if (sbc.op != static_cast<int>(csg::Op::Subtract)) continue;
        csg::Box c;
        c.transform = invA * scene.WorldMatrix(se);
        c.half = glm::max(sbc.halfExtents, glm::vec3(1e-4f));
        cutters.push_back(c);
    }

    MeshData md = csg::CarveBox(BodyBox(*bc), cutters, glm::max(bc->uvScale, 1e-3f));
    md.name = "Brush";
    return md;
}

void Update(Scene& scene, Renderer& renderer) {
    if (!renderer.SupportsScene()) return;
    entt::registry& reg = scene.Registry();

    // Collect brushes first (we mutate other component pools while processing).
    std::vector<entt::entity> brushes;
    bool anyDirty = false;
    for (const entt::entity e : reg.view<BrushComponent>()) {
        brushes.push_back(e);
        if (reg.get<BrushComponent>(e).dirty) anyDirty = true;
    }
    if (!anyDirty) return; // cheap when clean

    for (const entt::entity e : brushes) {
        BrushComponent& bc = reg.get<BrushComponent>(e);

        if (bc.op == static_cast<int>(csg::Op::Subtract)) {
            // Carving tool: strip any drawable/collider a prior Additive state left behind, keep a
            // box AABB so the editor can still pick + gizmo + wireframe it.
            if (MeshInstance* mi = reg.try_get<MeshInstance>(e)) {
                if (mi->mesh.IsValid()) renderer.DestroyMesh(mi->mesh);
                reg.remove<MeshInstance>(e);
            }
            if (reg.all_of<RigidBody>(e)) reg.remove<RigidBody>(e);
            const glm::vec3 h = glm::max(bc.halfExtents, glm::vec3(1e-4f));
            reg.emplace_or_replace<AABB>(e, AABB{-h, h});
            bc.dirty = false;
            continue;
        }

        // Additive: (re)build the carved solid. Topology changes every edit, so destroy + recreate
        // the GPU mesh (UpdateMesh never grows) and reassign the handle.
        MeshData md = BuildEntityMesh(scene, e);

        const bool hadInstance = reg.all_of<MeshInstance>(e);
        MeshInstance& mi = reg.get_or_emplace<MeshInstance>(e);
        if (!hadInstance) {
            mi.surface.base_color = {0.72f, 0.72f, 0.75f, 1.0f}; // neutral blockout grey
            mi.surface.base_metalness = 0.0f;
            mi.surface.specular_roughness = 0.65f;
        }
        if (mi.mesh.IsValid()) renderer.DestroyMesh(mi.mesh);
        mi.mesh = md.vertices.empty() ? rhi::MeshHandle{} : renderer.UploadMesh(md);

        glm::vec3 bmin, bmax;
        ComputeBounds(md, bmin, bmax);
        reg.emplace_or_replace<AABB>(e, AABB{bmin, bmax});

        // Static exact-triangle collision (Jolt MeshShape). collisionVertices are mesh-local, never
        // serialized; setting bodyId = kInvalidBody makes PhysicsWorld reap + rebuild the body.
        RigidBody& rb = reg.get_or_emplace<RigidBody>(e);
        rb.shape = RigidBody::Shape::Mesh;
        rb.motion = RigidBody::Motion::Static;
        rb.collisionVertices.clear();
        rb.collisionVertices.reserve(md.vertices.size());
        for (const Vertex& v : md.vertices) rb.collisionVertices.push_back(v.position);
        rb.collisionIndices = md.indices;
        rb.centerOffset = 0.5f * (bmin + bmax);
        rb.bodyId = RigidBody::kInvalidBody;

        bc.dirty = false;
    }
}

void MarkAllDirty(Scene& scene) {
    entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<BrushComponent>()) reg.get<BrushComponent>(e).dirty = true;
}

bool AllClean(const Scene& scene) {
    const entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<const BrushComponent>())
        if (reg.get<const BrushComponent>(e).dirty) return false;
    return true;
}

} // namespace hbe::brush

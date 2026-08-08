#include "Construction/ConstructionSystem.h"

#include "Construction/ConstructionChunk.h"
#include "Construction/ConstructionIO.h"
#include "Core/Log.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"

#include <string>
#include <vector>

namespace hbe::construction {

void ClearGenerated(Scene& scene, entt::entity owner) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> doomed;
    // Collected first, destroyed after. Destroying while iterating an EnTT view invalidates it,
    // and the failure is a corrupted pool rather than an exception.
    for (const entt::entity e : reg.view<BuildChunkTag>()) {
        const BuildChunkTag& t = reg.get<BuildChunkTag>(e);
        if (owner == entt::null || t.owner == owner) doomed.push_back(e);
    }
    for (const entt::entity e : doomed)
        if (reg.valid(e)) reg.destroy(e);
}

u32 Sync(Scene& scene, Renderer& renderer) {
    entt::registry& reg = scene.Registry();
    u32 rebuilt = 0;

    std::vector<entt::entity> owners;
    for (const entt::entity e : reg.view<ProceduralBuilding>()) {
        const ProceduralBuilding& pb = reg.get<ProceduralBuilding>(e);
        if (pb.builtRevision != pb.revision) owners.push_back(e);
    }
    if (owners.empty()) return 0;

    for (const entt::entity owner : owners) {
        if (!reg.valid(owner)) continue;
        ProceduralBuilding& pb = reg.get<ProceduralBuilding>(owner);
        // Marked built BEFORE any early return. A building whose asset is missing must not retry
        // every frame - that would hammer the disk and spam the log forever.
        pb.builtRevision = pb.revision;

        ClearGenerated(scene, owner);
        if (pb.source.empty()) continue;

        // VFS FIRST, DISK SECOND. This is the regenerator that runs in a SHIPPED build, where
        // `.hbbuild` lives inside a mounted .uap pack and not as a loose file - the cooker folds
        // it in precisely because it is registered runtimeLoaded. Reading it with std::ifstream
        // works in the editor and returns nothing in a shipped game, which is exactly how
        // `.hbgi`, `.hbuianim` and `.hbworld` each shipped broken. The disk fallback exists only
        // for an editor session whose VFS has no loose mount.
        BuildAsset asset;
        std::string err;
        bool loaded = LoadBuildVfs(pb.source, asset, err);
        if (!loaded) {
            std::string diskErr;
            loaded = LoadBuild(Project::Active().AssetsDir() / pb.source, asset, diskErr);
            if (!loaded) err = diskErr;
        }
        if (!loaded) {
            // ERROR, not a warning: the building is now invisible and, because the revision is
            // latched above, it will not retry until something bumps it. That is worth a loud
            // line rather than one lost in the log.
            HBE_ERROR("ProceduralBuilding '%s': %s (nothing will be generated)",
                      pb.source.c_str(), err.c_str());
            continue;
        }
        if (!err.empty()) HBE_WARN("ProceduralBuilding '%s': %s", pb.source.c_str(), err.c_str());

        ChunkedSection chunked;
        BuildChunked(asset.def, &asset.damage, kInvalidComponent,
                     pb.chunkSize > 0.01f ? pb.chunkSize : asset.chunkSize, chunked);

        u32 created = 0;
        for (const ConstructionChunk& chunk : chunked.chunks) {
            for (usize sub = 0; sub < chunk.model.size(); ++sub) {
                const MeshData& md = chunk.model[sub];
                if (md.Empty()) continue; // CreateMesh returns an INVALID handle for empty data

                const rhi::MeshHandle handle = renderer.UploadMesh(md);
                if (!handle.IsValid()) continue;

                const std::string name = "Chunk " + std::to_string(chunk.cx) + "," +
                                         std::to_string(chunk.cy) + "," +
                                         std::to_string(chunk.cz) + " #" + std::to_string(sub);
                const entt::entity e = scene.CreateEntity(name);

                // Parented to the building, so moving the building moves its geometry - and the
                // pieces are already in the building's local space, so the child transform is
                // identity rather than a second copy of the placement.
                reg.emplace<Parent>(e, owner);
                reg.emplace<Transform>(e);

                MeshInstance mi;
                mi.mesh = handle;
                mi.baseColor = md.material.baseColor;
                mi.metallic = md.material.metallic;
                mi.roughness = md.material.roughness;
                // The procedural surface for this submesh's material. Without it every piece draws
                // as one flat colour and a brick wall reads as a pink slab - the geometry is right
                // and the SURFACE is missing.
                if (sub < chunk.materials.size())
                    mi.materialFlags |= ProceduralSurfaceFlagsFor(chunk.materials[sub]);
                reg.emplace<MeshInstance>(e, mi);

                // NO MeshRef, deliberately. There is no provenance scheme for generated geometry -
                // MeshRef understands only "prim:" and "uaf:" - which is exactly why these
                // entities carry BuildChunkTag and are excluded from the scene file. Giving them a
                // MeshRef they could not resolve is what makes an entity reload with no geometry.
                reg.emplace<BuildChunkTag>(e, BuildChunkTag{owner, chunk.cx, chunk.cy, chunk.cz});
                reg.emplace<AABB>(e, AABB{chunk.boundsMin, chunk.boundsMax});

                // COLLISION IS SEPARATE FROM RENDER GEOMETRY (brief SS24) - but here it is built
                // from the same chunk mesh, and that is a deliberate first step rather than the
                // end state. A brick wall's render mesh is a few thousand boxes; its IDEAL
                // collider is one slab. Until there is a simplified-collision pass, an exact
                // triangle collider per chunk is correct-but-heavy, and correct-but-heavy beats a
                // building the player walks straight through.
                //
                // Static + Mesh: exact triangles are static-only in this engine (a dynamic body
                // needs ConvexHull), and a building does not move.
                RigidBody rb;
                rb.shape = RigidBody::Shape::Mesh;
                rb.motion = RigidBody::Motion::Static;
                rb.friction = 0.8f;
                rb.restitution = 0.0f;
                rb.collisionVertices.reserve(md.vertices.size());
                for (const Vertex& v : md.vertices) rb.collisionVertices.push_back(v.position);
                rb.collisionIndices = md.indices;
                // centerOffset is deliberately LEFT AT ZERO. PhysicsWorld applies it only to
                // primitive shapes (`if (!meshCollider && ...)` at PhysicsWorld.cpp:829), and
                // these vertices already carry their full position in the building's local space -
                // setting it would either do nothing or, if that guard ever changed, shift every
                // collider off its geometry.
                reg.emplace<RigidBody>(e, std::move(rb));

                ++created;
            }
        }

        // THE COST OF REGENERATION, stated once per rebuild rather than hidden. Every upload above
        // is permanent VRAM: the RHI cannot free a mesh, so a rebuild abandons the previous
        // buffers for the process lifetime. This is why Sync is revision-gated instead of running
        // on every parameter edit.
        HBE_INFO("ProceduralBuilding '%s': %u chunk(s), %u draw(s), %u logical unit(s)",
                 pb.source.c_str(), static_cast<u32>(chunked.chunks.size()), created,
                 chunked.TotalPieces());
        ++rebuilt;
    }
    return rebuilt;
}

} // namespace hbe::construction

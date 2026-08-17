// Scene/CharacterSystem.cpp
#include "Scene/CharacterSystem.h"

#include "Assets/AssetLoader.h"
#include "Assets/CharacterAsset.h"
#include "Assets/CharacterBuild.h"
#include "Assets/MaterialAsset.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/glm.hpp>

#include <unordered_map>

namespace hbe::character {

namespace fs = std::filesystem;

namespace {

// A built + uploaded character, shared by every instance of the same .hbchar.
struct Cached {
    CharacterAsset asset;
    assets::BuiltCharacter build;
    std::unordered_map<std::string, rhi::MeshHandle> meshes; // variant id -> GPU mesh
    std::unordered_map<std::string, rhi::TextureHandle> texCache;
};

std::unordered_map<std::string, Cached>& Cache() {
    static std::unordered_map<std::string, Cached> c;
    return c;
}

// Loads + welds + uploads a character the first time it is seen. Returns nullptr on
// failure (missing/invalid asset).
Cached* EnsureBuilt(Renderer& renderer, const fs::path& assetsDir, const std::string& hbcharRel) {
    auto& cache = Cache();
    if (const auto it = cache.find(hbcharRel); it != cache.end())
        return it->second.build.ok ? &it->second : nullptr;

    Cached c;
    const auto loaded = assets::LoadCharacter(assetsDir / hbcharRel);
    if (!loaded) {
        HBE_ERROR("Character: cannot load '{}'.", hbcharRel);
        cache.emplace(hbcharRel, std::move(c)); // negative cache (build.ok == false)
        return nullptr;
    }
    c.asset = *loaded;
    c.build = assets::BuildCharacter(assetsDir, c.asset);
    if (c.build.ok) {
        for (auto& [id, mesh] : c.build.welded) {
            const rhi::MeshHandle h = renderer.UploadMesh(mesh);
            if (h.IsValid()) c.meshes.emplace(id, h);
        }
    }
    auto [it, _] = cache.emplace(hbcharRel, std::move(c));
    return it->second.build.ok ? &it->second : nullptr;
}

void ApplyPartMaterial(Renderer& renderer, const fs::path& assetsDir, Cached& c,
                       const CharacterVariant& v, MeshInstance& mi) {
    // 1) explicit variant .hbmat, 2) the mesh's own generated .hbmat, 3) raw fields.
    std::string matAsset = v.material;
    const MeshData* md = nullptr;
    if (const auto it = c.build.welded.find(v.id); it != c.build.welded.end()) md = &it->second;
    if (matAsset.empty() && md) matAsset = md->material.materialAsset;

    if (!matAsset.empty()) {
        if (const auto mat = assets::LoadMaterial(assetsDir / matAsset)) {
            assets::ApplyMaterial(renderer, assetsDir, *mat, mi, c.texCache);
            return;
        }
    }
    if (md) {
        mi.surface.base_color = md->material.baseColor;
        mi.surface.base_metalness = md->material.metallic;
        mi.surface.specular_roughness = md->material.roughness;
        mi.surface.emission_color = md->material.emissive;
        const auto tex = [&](const std::string& rel) -> rhi::TextureHandle {
            if (rel.empty()) return {};
            if (const auto it = c.texCache.find(rel); it != c.texCache.end()) return it->second;
            const rhi::TextureHandle h = assets::LoadTexture(renderer, assetsDir / rel);
            c.texCache[rel] = h;
            return h;
        };
        mi.albedoTexture = tex(md->material.baseColorTex);
        mi.normalTexture = tex(md->material.normalTex);
        mi.mrTexture = tex(md->material.mrTex);
        mi.aoTexture = tex(md->material.aoTex);
        mi.emissiveTexture = tex(md->material.emissiveTex);
    }
}

// Spawns (or clears) the part for one slot. Destroys any existing live part first.
void SpawnSlot(Scene& scene, Renderer& renderer, const fs::path& assetsDir, entt::entity root,
               Cached& c, Character& ch, const std::string& slot, const std::string& variantId) {
    entt::registry& reg = scene.Registry();

    // Remove the currently-live part for this slot, if any.
    if (const auto it = ch.liveParts.find(slot); it != ch.liveParts.end()) {
        if (reg.valid(it->second)) reg.destroy(it->second);
        ch.liveParts.erase(it);
    }
    if (variantId.empty()) {
        ch.activeVariant[slot] = ""; // slot hidden this loadout
        return;
    }

    const CharacterVariant* v = c.asset.FindVariant(variantId);
    const auto mh = c.meshes.find(variantId);
    if (!v || mh == c.meshes.end() || !mh->second.IsValid()) {
        // Unknown variant (e.g. deleted from the .hbchar): HIDE the slot rather than
        // record the stale id - otherwise it would re-serialize as permanent
        // corruption + activeVariant would disagree with liveParts.
        HBE_WARN("Character: slot '{}' variant '{}' has no built mesh; hiding slot.", slot, variantId);
        ch.activeVariant[slot] = "";
        return;
    }

    const entt::entity part = scene.CreateEntity(slot + ":" + variantId);
    reg.emplace<Transform>(part); // identity: skinning is in the shared skeleton's space
    reg.emplace<Parent>(part, Parent{root});
    reg.emplace<SkinnedPartRef>(part, SkinnedPartRef{root, slot, variantId});
    MeshInstance mi;
    mi.mesh = mh->second;
    ApplyPartMaterial(renderer, assetsDir, c, *v, mi);
    reg.emplace<MeshInstance>(part, mi);

    // AABB from the welded mesh bounds (culling + picking).
    if (const auto it = c.build.welded.find(variantId); it != c.build.welded.end()) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex& vx : it->second.vertices) {
            lo = glm::min(lo, vx.position);
            hi = glm::max(hi, vx.position);
        }
        if (lo.x <= hi.x) reg.emplace<AABB>(part, AABB{lo, hi});
    }
    ch.liveParts[slot] = part;
    ch.activeVariant[slot] = variantId; // record ONLY after a successful spawn
}

// Fills ch.activeVariant for a loadout name (unknown/empty -> per-slot defaults).
void ResolveLoadout(Cached& c, Character& ch, const std::string& loadout) {
    ch.loadout = loadout;
    ch.activeVariant.clear();
    // Everything below is DERIVED from the .hbchar, so the map is no longer authored;
    // the serializer must not freeze it into the scene (see Character::variantAuthored).
    ch.variantAuthored = false;
    const CharacterLoadout* lo = c.asset.FindLoadout(loadout);
    for (const CharacterSlot& s : c.asset.slots) {
        std::string variant = c.asset.DefaultVariant(s.name);
        if (lo) {
            const auto it = lo->slots.find(s.name);
            variant = (it != lo->slots.end()) ? it->second : std::string(); // absent = hidden
        }
        ch.activeVariant[s.name] = variant;
    }
}

// Unions the live parts' bounds onto the ROOT.
//
// Parts are parented to the root with an IDENTITY transform (skinning happens in
// the shared skeleton's space), so their local AABBs are already in the root's
// space and the union is exact. Without this the root - which is the entity an
// author puts `Interactable` on, and the one docs/NarrativeSystem.md tells them to
// use - had NO AABB at all: interact::Pick then fell back to a 0.75 m cube at the
// rig PIVOT (the feet, half underground), so aiming at an NPC's head or chest
// missed it entirely. Culling gets the same benefit.
void UpdateRootBounds(entt::registry& reg, entt::entity root) {
    const Character* ch = reg.try_get<Character>(root);
    if (!ch) return;
    glm::vec3 lo(1e30f), hi(-1e30f);
    bool any = false;
    for (const auto& [slot, part] : ch->liveParts) {
        if (!reg.valid(part)) continue;
        const AABB* bb = reg.try_get<AABB>(part);
        if (!bb) continue;
        lo = glm::min(lo, glm::min(bb->min, bb->max));
        hi = glm::max(hi, glm::max(bb->min, bb->max));
        any = true;
    }
    if (any)
        reg.emplace_or_replace<AABB>(root, AABB{lo, hi});
    else
        reg.remove<AABB>(root);
}

} // namespace

void ClearParts(Scene& scene, entt::entity root) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(root)) return;
    Character* ch = reg.try_get<Character>(root);
    if (!ch) return;
    for (auto& [slot, part] : ch->liveParts)
        if (reg.valid(part)) reg.destroy(part);
    ch->liveParts.clear();
    UpdateRootBounds(reg, root);
}

void Instantiate(Scene& scene, Renderer& renderer, entt::entity root, const fs::path& assetsDir) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(root)) return;
    Character* ch = reg.try_get<Character>(root);
    if (!ch || ch->asset.empty()) return;

    Cached* c = EnsureBuilt(renderer, assetsDir, ch->asset);
    if (!c) return;

    // Root poses the shared skeleton but is never drawn: it carries an Animator +
    // a MeshRef to the skeleton .uaf (UpdateSkeletal resolves the target skeleton
    // from MeshRef), and NO MeshInstance (so CollectDrawItems skips it).
    if (!reg.all_of<Animator>(root)) reg.emplace<Animator>(root);
    reg.emplace_or_replace<MeshRef>(root, MeshRef{c->build.skeletonSource});
    reg.remove<MeshInstance>(root); // never draw the root

    // A named loadout (or a first-time instance with no choices yet) resolves from
    // the asset; a persisted CUSTOM loadout (loadout=="" with saved activeVariant
    // overrides) is kept as-is so an equipped outfit survives save/load.
    if (!ch->loadout.empty() || ch->activeVariant.empty())
        ResolveLoadout(*c, *ch, ch->loadout);
    // Fill any slot the persisted set is missing (e.g. a slot added to the .hbchar
    // after this instance was saved) with that slot's default.
    for (const CharacterSlot& s : c->asset.slots)
        if (!ch->activeVariant.count(s.name)) ch->activeVariant[s.name] = c->asset.DefaultVariant(s.name);

    ClearParts(scene, root);
    for (const CharacterSlot& s : c->asset.slots)
        SpawnSlot(scene, renderer, assetsDir, root, *c, *ch, s.name, ch->activeVariant[s.name]);
    UpdateRootBounds(reg, root);
}

void SetLoadout(Scene& scene, Renderer& renderer, entt::entity root, const fs::path& assetsDir,
                const std::string& loadout) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(root)) return;
    Character* ch = reg.try_get<Character>(root);
    if (!ch || ch->asset.empty()) return;
    Cached* c = EnsureBuilt(renderer, assetsDir, ch->asset);
    if (!c) return;

    ResolveLoadout(*c, *ch, loadout);
    ClearParts(scene, root);
    for (const CharacterSlot& s : c->asset.slots)
        SpawnSlot(scene, renderer, assetsDir, root, *c, *ch, s.name, ch->activeVariant[s.name]);
    UpdateRootBounds(reg, root);
}

void SetSlotVariant(Scene& scene, Renderer& renderer, entt::entity root, const fs::path& assetsDir,
                    const std::string& slot, const std::string& variant) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(root)) return;
    Character* ch = reg.try_get<Character>(root);
    if (!ch || ch->asset.empty()) return;
    Cached* c = EnsureBuilt(renderer, assetsDir, ch->asset);
    if (!c) return;
    ch->loadout.clear(); // now a custom loadout
    SpawnSlot(scene, renderer, assetsDir, root, *c, *ch, slot, variant);
    // A real equip: this map is now the author's / the player's choice and MUST
    // survive save+load, unlike a set that was merely resolved from the defaults.
    ch->variantAuthored = true;
    UpdateRootBounds(reg, root);
}

void ClearCache() { Cache().clear(); }

} // namespace hbe::character

#include "Construction/ConstructionGeometry.h"

#include "Construction/ConstructionMasonry.h"
#include "Construction/ConstructionWood.h"
#include "Construction/ConstructionOpenings.h"
#include "Construction/ConstructionChunk.h"
#include "RHI/RHI.h"  // ProceduralSurface bits ride in MaterialFlags

#include <algorithm>
#include <cmath>
#include <map>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace hbe::construction {

namespace {

// UV DENSITY IS WORLD-SCALE: one UV unit per metre, on every surface, regardless of the
// component's size.
//
// This deliberately diverges from GenerateCube, which packs each face into its own cell of a 4x4
// atlas so painting one face cannot bleed onto another. That is right for a unit primitive and
// wrong for architecture: it makes UVs a function of the object's SIZE, so an 8 m wall and a 2 m
// wall would get four times the texel density difference and their brick courses would visibly
// disagree along a shared corner. Architecture wants the texture pinned to the world, not to the
// object.
constexpr f32 kUvPerMetre = 1.0f;

struct FaceDef {
    glm::vec3 origin, du, dv;
};

// Appends one quad. Follows Assets/MeshGenerator.cpp's convention exactly: normal = cross(du,dv),
// tangent = du (w = +1), corners (o, o+du, o+du+dv, o+dv), indices (0,1,2),(0,2,3).
void EmitQuad(MeshData& m, const FaceDef& f, const glm::mat4& xform, const glm::mat3& normalMat) {
    const glm::vec3 nLocal = glm::cross(f.du, f.dv);
    const f32 nLen = glm::length(nLocal);
    if (nLen <= 1e-12f) return; // degenerate face (a zero extent) - emit nothing rather than NaNs

    const glm::vec3 n = glm::normalize(normalMat * (nLocal / nLen));
    const glm::vec3 t = glm::normalize(glm::mat3(xform) * f.du);
    // UV axes measured in METRES along the face, so density is independent of component size.
    const f32 uLen = glm::length(f.du);
    const f32 vLen = glm::length(f.dv);

    const u32 base = m.VertexCount();
    const glm::vec3 corners[4] = {f.origin, f.origin + f.du, f.origin + f.du + f.dv,
                                  f.origin + f.dv};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f},
                              {uLen * kUvPerMetre, 0.0f},
                              {uLen * kUvPerMetre, vLen * kUvPerMetre},
                              {0.0f, vLen * kUvPerMetre}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = glm::vec3(xform * glm::vec4(corners[i], 1.0f));
        v.normal = n;
        v.tangent = glm::vec4(t, 1.0f);
        v.uv = uvs[i];
        m.vertices.push_back(v);
    }
    m.indices.insert(m.indices.end(),
                     {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
}

// Appends one triangle (the gable ends). Same winding rule; the caller orders a,b,c so that
// cross(b-a, c-a) points outward.
void EmitTri(MeshData& m, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::mat4& xform, const glm::mat3& normalMat) {
    const glm::vec3 nLocal = glm::cross(b - a, c - a);
    const f32 nLen = glm::length(nLocal);
    if (nLen <= 1e-12f) return;

    const glm::vec3 n = glm::normalize(normalMat * (nLocal / nLen));
    const glm::vec3 t = glm::normalize(glm::mat3(xform) * (b - a));
    const glm::vec3 tri[3] = {a, b, c};
    const glm::vec2 uv[3] = {{0.0f, 0.0f},
                             {glm::length(b - a) * kUvPerMetre, 0.0f},
                             {glm::length(c - a) * kUvPerMetre * 0.5f,
                              glm::length(c - a) * kUvPerMetre}};
    const u32 base = m.VertexCount();
    for (int i = 0; i < 3; ++i) {
        Vertex v;
        v.position = glm::vec3(xform * glm::vec4(tri[i], 1.0f));
        v.normal = n;
        v.tangent = glm::vec4(t, 1.0f);
        v.uv = uv[i];
        m.vertices.push_back(v);
    }
    m.indices.insert(m.indices.end(), {base + 0, base + 1, base + 2});
}

// A solid box from half-extents. 24 vertices (flat-shaded: each face owns its corners so the
// normals are not averaged across the edge), 36 indices.
void EmitBox(MeshData& m, const glm::vec3& e, const glm::mat4& xform, const glm::mat3& normalMat) {
    const f32 x = e.x, y = e.y, z = e.z;
    const FaceDef faces[6] = {
        {{ x, -y,  z}, { 0, 0, -2 * z}, {0, 2 * y, 0}},  // +X
        {{-x, -y, -z}, { 0, 0,  2 * z}, {0, 2 * y, 0}},  // -X
        {{-x,  y,  z}, { 2 * x, 0, 0}, {0, 0, -2 * z}},  // +Y
        {{-x, -y, -z}, { 2 * x, 0, 0}, {0, 0,  2 * z}},  // -Y
        {{-x, -y,  z}, { 2 * x, 0, 0}, {0, 2 * y, 0}},   // +Z
        {{ x, -y, -z}, {-2 * x, 0, 0}, {0, 2 * y, 0}},   // -Z
    };
    for (const FaceDef& f : faces) EmitQuad(m, f, xform, normalMat);
}

// A gable prism: ridge along local X at +Y, eaves at -Y. 18 vertices, 24 indices.
// This is the one Phase 2 shape that is not a box, and it is what makes a roof read as a roof.
void EmitGable(MeshData& m, const glm::vec3& e, const glm::mat4& xform, const glm::mat3& normalMat) {
    const f32 x = e.x, y = e.y, z = e.z;

    // Underside.
    EmitQuad(m, {{-x, -y, -z}, {2 * x, 0, 0}, {0, 0, 2 * z}}, xform, normalMat);
    // +Z slope: from the +Z eave up to the ridge.
    EmitQuad(m, {{-x, -y, z}, {2 * x, 0, 0}, {0, 2 * y, -z}}, xform, normalMat);
    // -Z slope.
    EmitQuad(m, {{x, -y, -z}, {-2 * x, 0, 0}, {0, 2 * y, z}}, xform, normalMat);
    // Gable ends. Wound so the normal faces outward along +X / -X respectively.
    EmitTri(m, {x, -y, z}, {x, -y, -z}, {x, y, 0}, xform, normalMat);
    EmitTri(m, {-x, -y, -z}, {-x, -y, z}, {-x, y, 0}, xform, normalMat);
}

// Does this component lay discrete UNITS instead of one solid block? Both conditions matter:
// the material must be a unit material, and the kind must be something actually built out of
// units - a brick-material BEAM is a lintel, one piece, not a course of bricks.
bool LaysUnits(const ConstructionComponent& c) {
    if (!IsMasonry(c.material)) return false;
    switch (c.kind) {
        case ComponentKind::Wall:
        case ComponentKind::Foundation:
        case ComponentKind::Column:
        case ComponentKind::Siding:
            return true;
        default:
            return false;
    }
}

// A Door or Window OCCUPIES an opening and builds its own frame + leaf/glazing, rather than
// being a plain massing box.
bool IsFilling(ComponentKind k) {
    return k == ComponentKind::Door || k == ComponentKind::Window;
}

enum class Shape : u8 { None, Box, Gable };

Shape ShapeOf(ComponentKind k) {
    switch (k) {
        // Containers and holes are real components with real relationships that draw nothing.
        case ComponentKind::Building:
        case ComponentKind::Opening:
        case ComponentKind::Unknown:
            return Shape::None;
        case ComponentKind::Roof:
        case ComponentKind::RoofSurface:
            return Shape::Gable;
        default:
            // Everything else is massing at this phase: walls, slabs, framing members, cladding.
            // Phases 3-5 replace the interesting ones with real construction.
            return Shape::Box;
    }
}

constexpr u32 kBoxVerts = 24, kBoxIndices = 36;
constexpr u32 kGableVerts = 18, kGableIndices = 24;

// A stable, published base colour per construction material, so a Phase 2 massing block reads as
// the material it claims to be before any texture work exists. Replaced by real `.hbmat` assets
// in a later phase - this is scaffolding and is marked as such rather than pretending otherwise.
glm::vec4 DebugColorFor(MaterialKind m) {
    switch (m) {
        case MaterialKind::Brick:           return {0.55f, 0.24f, 0.18f, 1.0f};
        case MaterialKind::ConcreteBlock:   return {0.62f, 0.62f, 0.58f, 1.0f};
        case MaterialKind::Stone:           return {0.48f, 0.47f, 0.44f, 1.0f};
        case MaterialKind::PouredConcrete:  return {0.68f, 0.68f, 0.66f, 1.0f};
        case MaterialKind::TimberFrame:     return {0.65f, 0.48f, 0.28f, 1.0f};
        case MaterialKind::WoodPlank:       return {0.58f, 0.41f, 0.24f, 1.0f};
        case MaterialKind::Plywood:         return {0.72f, 0.57f, 0.36f, 1.0f};
        case MaterialKind::OSB:             return {0.70f, 0.58f, 0.38f, 1.0f};
        case MaterialKind::WoodShingle:     return {0.42f, 0.32f, 0.22f, 1.0f};
        case MaterialKind::Drywall:         return {0.88f, 0.87f, 0.84f, 1.0f};
        case MaterialKind::Plaster:         return {0.90f, 0.89f, 0.86f, 1.0f};
        case MaterialKind::Metal:           return {0.60f, 0.62f, 0.65f, 1.0f};
        case MaterialKind::CorrugatedMetal: return {0.55f, 0.57f, 0.60f, 1.0f};
        case MaterialKind::Glass:           return {0.70f, 0.80f, 0.85f, 0.35f};
        // DELIBERATELY MUCH LIGHTER THAN ANY UNIT MATERIAL. The colour break between unit and
        // joint is what actually makes masonry read - a 10 mm recess alone is invisible past a
        // couple of metres, which is exactly why a brick wall was rendering as a flat slab.
        case MaterialKind::Mortar:          return {0.78f, 0.76f, 0.72f, 1.0f};
        default:                            return {0.75f, 0.75f, 0.75f, 1.0f};
    }
}

// Age darkens and desaturates, moisture pulls toward the green of algae and moss. Applied to the
// whole submesh because there is no per-unit colour channel on this renderer - which is a real
// limit, not a shortcut: see docs/Design-ProceduralConstruction.md SS2.2.
glm::vec4 WeatherColor(glm::vec4 c, const WeatheringParams& w) {
    const f32 t = w.ColourShift();
    if (t <= 0.0f) return c;
    const f32 grey = c.r * 0.30f + c.g * 0.59f + c.b * 0.11f;
    c.r = glm::mix(c.r, grey * 0.62f, t * 0.75f);
    c.g = glm::mix(c.g, grey * 0.66f, t * 0.75f);
    c.b = glm::mix(c.b, grey * 0.60f, t * 0.75f);
    const f32 moss = std::clamp(w.moisture, 0.0f, 1.0f) * t * 0.30f;
    c.g = glm::mix(c.g, 0.34f, moss);
    c.r = glm::mix(c.r, 0.22f, moss * 0.7f);
    return c;
}

void ApplyMaterial(MeshData& m, MaterialKind kind) {
    m.name = ToString(kind);
    m.material.baseColor = DebugColorFor(kind);
    switch (kind) {
        case MaterialKind::Metal:
        case MaterialKind::CorrugatedMetal:
            m.material.metallic = 1.0f;
            m.material.roughness = 0.35f;
            break;
        case MaterialKind::Glass:
            m.material.metallic = 0.0f;
            m.material.roughness = 0.05f;
            break;
        case MaterialKind::PouredConcrete:
        case MaterialKind::ConcreteBlock:
        case MaterialKind::Stone:
        case MaterialKind::Brick:
            m.material.metallic = 0.0f;
            m.material.roughness = 0.92f;
            break;
        default:
            m.material.metallic = 0.0f;
            m.material.roughness = 0.78f;
            break;
    }
}

// Is `id` inside the subtree rooted at `root`? `root == kInvalidComponent` means everything.
bool InSubtree(const ConstructionDef& def, ComponentId id, ComponentId root) {
    if (root == kInvalidComponent) return true;
    ComponentId walk = id;
    usize steps = 0;
    while (walk != kInvalidComponent && steps++ <= def.components.size()) {
        if (walk == root) return true;
        const ConstructionComponent* c = def.Find(walk);
        walk = c ? c->parent : kInvalidComponent;
    }
    return false; // includes the cycle case, which Validate reports separately
}

} // namespace

bool EmitsGeometry(ComponentKind kind) { return ShapeOf(kind) != Shape::None; }

u32 ProceduralSurfaceFlagsFor(MaterialKind m) {
    switch (m) {
        case MaterialKind::Brick:
        case MaterialKind::ConcreteBlock:  return rhi::PackProcSurface(rhi::ProcSurface_Brick);
        case MaterialKind::Stone:          return rhi::PackProcSurface(rhi::ProcSurface_Stone);
        case MaterialKind::PouredConcrete: return rhi::PackProcSurface(rhi::ProcSurface_Concrete);
        case MaterialKind::TimberFrame:
        case MaterialKind::WoodPlank:
        case MaterialKind::Plywood:
        case MaterialKind::OSB:            return rhi::PackProcSurface(rhi::ProcSurface_Wood);
        case MaterialKind::WoodShingle:    return rhi::PackProcSurface(rhi::ProcSurface_Shingle);
        case MaterialKind::Drywall:
        case MaterialKind::Plaster:        return rhi::PackProcSurface(rhi::ProcSurface_Plaster);
        case MaterialKind::Metal:
        case MaterialKind::CorrugatedMetal:return rhi::PackProcSurface(rhi::ProcSurface_Metal);
        case MaterialKind::Mortar:         return rhi::PackProcSurface(rhi::ProcSurface_Mortar);
        default:                           return 0;
    }
}

const MeshRange* SectionMesh::RangeFor(ComponentId id) const {
    for (const MeshRange& r : ranges)
        if (r.id == id) return &r;
    return nullptr;
}

std::vector<const ElementRange*> SectionMesh::ElementsOf(ComponentId id) const {
    std::vector<const ElementRange*> out;
    for (const ElementRange& e : elements)
        if (e.component == id) out.push_back(&e);
    return out;
}

u32 SectionMesh::TotalIndices() const {
    u32 n = 0;
    for (const MeshData& m : model) n += m.IndexCount();
    return n;
}

u32 SectionMesh::TotalVertices() const {
    u32 n = 0;
    for (const MeshData& m : model) n += m.VertexCount();
    return n;
}

glm::mat4 WorldMatrix(const ConstructionDef& def, ComponentId id) {
    // Built root-first so the multiplication order matches the scene's own parent-local
    // convention (world = parentWorld * local).
    std::vector<const ConstructionComponent*> chain;
    ComponentId walk = id;
    usize steps = 0;
    while (walk != kInvalidComponent && steps++ <= def.components.size()) {
        const ConstructionComponent* c = def.Find(walk);
        if (!c) break;
        chain.push_back(c);
        walk = c->parent;
    }
    glm::mat4 m(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        m = m * glm::translate(glm::mat4(1.0f), (*it)->position) * glm::mat4_cast((*it)->rotation);
    }
    return m;
}

MeshCapacity EstimateCapacity(const ConstructionDef& def, ComponentId root, f32 headroom) {
    MeshCapacity cap;
    for (const ConstructionComponent& c : def.components) {
        if (!InSubtree(def, c.id, root)) continue;
        if (c.subtract) continue; // a subtractive brush is a tool: it draws nothing to reserve for
        // Counts the HIDDEN and the DESTROYED too. A reservation must survive the artist
        // un-hiding something or a repair pass restoring it - reserving only for what is
        // currently visible would refuse the very next UpdateMesh, and a refused update leaves
        // stale geometry on the GPU with no way to free it.
        // WEATHERING IS EVALUATED AT ZERO FOR THE RESERVATION. Ageing only ever REMOVES units, so
        // sizing to the current age would refuse the update the moment an artist turned the dial
        // back down and the missing bricks returned - the same trap as sizing to a disabled cutter.
        ConstructionComponent pristine = c;
        pristine.weathering = WeatheringParams{};
        const ConstructionComponent& cc = pristine;

        if (LaysUnits(cc)) {
            // Counted by the SAME course/bond walk that generates them (MasonryUnitCount and
            // LayoutMasonry share one traversal), so the estimate cannot drift from the geometry.
            // A count one unit short here is not a rounding error in this engine: UpdateMesh would
            // refuse forever and there is no mesh destroy to recover with.
            const u32 units = MasonryUnitCount(def, cc);
            cap.vertices += units * kBoxVerts;
            cap.indices += units * kBoxIndices;
            if (cc.masonry.generateMortar && units > 0) {
                cap.vertices += kBoxVerts; // the recessed backing slab
                cap.indices += kBoxIndices;
            }
            continue;
        }
        // CUTTERS ARE COUNTED BOTH WAYS, and the larger wins.
        //
        // Toggling an opening on or off is THE core action of a non-destructive cutter workflow,
        // so the reservation has to survive both states. And the direction is not obvious:
        // subtracting a cutter from a solid box makes it BIGGER (one box becomes up to six), and a
        // stud crossing a window SPLITS into a cripple above and one below - while masonry only
        // ever loses units. Sizing to either state alone would refuse the update on the other.
        std::vector<BoxPiece> cutters;
        GatherCutters(def, c, nullptr, cutters, /*includeDisabled=*/true);

        if (IsWoodConstruction(cc)) {
            // Counted by RUNNING THE ACTUAL LAYOUT, not by re-deriving the arithmetic. Wood has
            // four different layouts with end-member, clipping and opening-framing rules that are
            // easy to get subtly wrong twice; a count that disagrees with generation by one member
            // means UpdateMesh is refused forever, with no mesh destroy to recover with.
            std::vector<BoardPlacement> uncut, cut;
            LayoutWood(def, cc, uncut);
            LayoutWood(def, cc, cut, &cutters);
            const u32 n = static_cast<u32>(std::max(uncut.size(), cut.size()));
            cap.vertices += n * kBoxVerts;
            cap.indices += n * kBoxIndices;
            // The sheathing deck under a shingled roof. Emitted by CollectPieces but NOT part of
            // LayoutWood's member list, so counting only the members under-counts by one gable -
            // and an under-count by even one piece is a permanently refused UpdateMesh.
            if ((cc.kind == ComponentKind::Roof || cc.kind == ComponentKind::RoofSurface) &&
                cc.material == MaterialKind::WoodShingle) {
                cap.vertices += kGableVerts;
                cap.indices += kGableIndices;
            }
            continue;
        }
        if (IsFilling(cc.kind)) {
            std::vector<BoxPiece> frame, panel;
            BuildFillingGeometry(def, cc, frame, panel);
            const u32 n = static_cast<u32>(frame.size() + panel.size());
            cap.vertices += n * kBoxVerts;
            cap.indices += n * kBoxIndices;
            continue;
        }
        switch (ShapeOf(cc.kind)) {
            case Shape::Box: {
                u32 boxes = 1;
                if (!cutters.empty()) {
                    std::vector<BoxPiece> pieces;
                    SubtractCutters(glm::vec3(0.0f), cc.extent, cutters, pieces);
                    boxes = std::max<u32>(1, static_cast<u32>(pieces.size()));
                }
                cap.vertices += boxes * kBoxVerts;
                cap.indices += boxes * kBoxIndices;
                break;
            }
            case Shape::Gable: cap.vertices += kGableVerts; cap.indices += kGableIndices; break;
            case Shape::None:  break;
        }
    }
    if (headroom > 1.0f) {
        cap.vertices = static_cast<u32>(static_cast<f32>(cap.vertices) * headroom);
        cap.indices = static_cast<u32>(static_cast<f32>(cap.indices) * headroom);
    }
    return cap;
}

// The ONE place that decides what geometry a component produces. Both the flat and the chunked
// builders consume its output, so there is no second dispatch to keep in lockstep - the mistake
// this system already made once with a capacity estimate that could drift from its generator.
void CollectPieces(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                   std::vector<PlacedPiece>& out) {
    out.clear();

    // Gather in ASCENDING ID ORDER. Determinism is the whole point of the seed design, and it
    // would be pointless if emission order depended on how the definition happened to be authored.
    std::vector<const ConstructionComponent*> emit;
    emit.reserve(def.components.size());
    for (const ConstructionComponent& c : def.components) {
        if (ShapeOf(c.kind) == Shape::None && !IsFilling(c.kind)) continue;
        // A subtractive brush is a TOOL, not geometry - it carves and is never drawn.
        if (c.subtract) continue;
        if (c.hidden) continue;
        if (damage && damage->IsDestroyed(c.id)) continue;
        if (!InSubtree(def, c.id, root)) continue;
        emit.push_back(&c);
    }
    std::sort(emit.begin(), emit.end(),
              [](const ConstructionComponent* a, const ConstructionComponent* b) {
                  return a->id < b->id;
              });

    for (const ConstructionComponent* c : emit) {
        const glm::mat4 xform = WorldMatrix(def, c->id);

        auto push = [&](const glm::mat4& x, const glm::vec3& e, u32 element, bool gable) {
            PlacedPiece p;
            p.component = c->id;
            p.element = element;
            p.material = c->material;
            p.xform = x;
            p.extent = e;
            p.gable = gable;
            p.weathering = c->weathering;
            out.push_back(p);
        };

        std::vector<BoxPiece> cutters;
        GatherCutters(def, *c, damage, cutters);

        if (LaysUnits(*c)) {
            std::vector<BrickPlacement> units;
            LayoutMasonry(def, *c, units);
            if (!cutters.empty()) {
                std::vector<BrickPlacement> kept;
                kept.reserve(units.size());
                for (BrickPlacement u : units) {
                    if (FullyCut(u.center, u.extent, cutters)) continue;
                    glm::vec3 uc = u.center, ue = u.extent;
                    if (ClipToSingleBox(uc, ue, cutters)) {
                        u.center = uc;
                        u.extent = ue;
                    }
                    kept.push_back(u);
                }
                units.swap(kept);
            }
            // UNITS FIRST, MORTAR AFTER. Not a draw-order concern - these are opaque and
            // depth-tested - but `SectionMesh::RangeFor` returns a component's FIRST range, and a
            // masonry wall now spans two submeshes. Emitting the mortar first made "the wall's
            // submesh" resolve to the mortar's, which broke the material-ordering contract every
            // caller reads that way.
            for (const BrickPlacement& b : units) {
                glm::mat4 ux = xform * glm::translate(glm::mat4(1.0f), b.center);
                if (b.yaw != 0.0f) ux = glm::rotate(ux, b.yaw, glm::vec3(0, 1, 0));
                if (b.roll != 0.0f) ux = glm::rotate(ux, b.roll, glm::vec3(0, 0, 1));
                push(ux, b.extent, b.ElementId(), false);
            }
            if (c->masonry.generateMortar && !units.empty()) {
                // INSET ON EVERY AXIS, INCLUDING DEPTH. This slab used to be inset only a half
                // joint in Z while the units were clamped to their nominal unit depth, so on any
                // wall thicker than a brick the backing completely ENGULFED the brickwork and the
                // wall drew as a flat box. Now the units stand proud of it by a full joint on both
                // faces, so the joints read as real recesses.
                // THE RECESS IS SHALLOW ON PURPOSE. Inset by a FULL joint in depth, each joint
                // became a slot as deep as it was wide, which self-occludes and reads as a black
                // line rather than as mortar. Real brickwork is struck nearly flush - a few
                // millimetres - so the mortar face still catches light.
                const f32 inset = std::max(c->masonry.joint, 0.002f);
                const f32 depthInset = inset * 0.25f;
                PlacedPiece m;
                m.component = c->id;
                m.element = kWholeComponent;
                m.material = MaterialKind::Mortar; // its own submesh, its own colour
                m.xform = xform;
                m.extent = glm::vec3(std::max(c->extent.x - inset, 0.0f),
                                     std::max(c->extent.y - inset, 0.0f),
                                     std::max(c->extent.z - depthInset, 0.0f));
                m.gable = false;
                out.push_back(m);
            }
        } else if (IsWoodConstruction(*c)) {
            // A SHINGLED ROOF NEEDS A DECK UNDER IT. Shingles are separate overlapping boards with
            // real gaps between them; with nothing behind, you see straight through into an unlit
            // interior - which is the black, speckled roof this produced before. Real roofs are
            // sheathed before they are covered, so emit the sheathing first and let the courses
            // sit on it.
            if ((c->kind == ComponentKind::Roof || c->kind == ComponentKind::RoofSurface) &&
                c->material == MaterialKind::WoodShingle) {
                PlacedPiece deck;
                deck.component = c->id;
                deck.element = kWholeComponent;
                deck.material = MaterialKind::Plywood; // sheathing, not the covering
                deck.xform = xform;
                // Slightly under the shingle plane so the courses always win the depth test.
                deck.extent = glm::vec3(c->extent.x, std::max(c->extent.y - 0.01f, 0.01f),
                                        std::max(c->extent.z - 0.01f, 0.01f));
                deck.gable = true;
                out.push_back(deck);
            }
            std::vector<BoardPlacement> members;
            LayoutWood(def, *c, members, &cutters);
            for (const BoardPlacement& b : members) {
                glm::mat4 ux = xform * glm::translate(glm::mat4(1.0f), b.center);
                if (b.yaw != 0.0f) ux = glm::rotate(ux, b.yaw, glm::vec3(0, 1, 0));
                if (b.pitch != 0.0f) ux = glm::rotate(ux, b.pitch, glm::vec3(1, 0, 0));
                if (b.roll != 0.0f) ux = glm::rotate(ux, b.roll, glm::vec3(0, 0, 1));
                push(ux, b.extent, b.ElementId(), false);
            }
        } else if (IsFilling(c->kind)) {
            std::vector<BoxPiece> frame, panel;
            BuildFillingGeometry(def, *c, frame, panel);
            for (const BoxPiece& p : frame)
                push(xform * glm::translate(glm::mat4(1.0f), p.center), p.extent, kWholeComponent,
                     false);
            for (const BoxPiece& p : panel)
                push(xform * glm::translate(glm::mat4(1.0f), p.center), p.extent, kWholeComponent,
                     false);
        } else if (ShapeOf(c->kind) == Shape::Box && !cutters.empty()) {
            std::vector<BoxPiece> pieces;
            SubtractCutters(glm::vec3(0.0f), c->extent, cutters, pieces);
            for (const BoxPiece& p : pieces)
                push(xform * glm::translate(glm::mat4(1.0f), p.center), p.extent, kWholeComponent,
                     false);
        } else {
            push(xform, c->extent, kWholeComponent, ShapeOf(c->kind) == Shape::Gable);
        }
    }
}

namespace {

// Emits one piece into `m` and reports the index slice it occupied.
void EmitPiece(MeshData& m, const PlacedPiece& p, u32& outFirst, u32& outCount, u32& outFirstVert,
               u32& outVertCount) {
    outFirst = m.IndexCount();
    outFirstVert = m.VertexCount();
    const glm::mat3 nm = glm::inverseTranspose(glm::mat3(p.xform));
    if (p.gable) EmitGable(m, p.extent, p.xform, nm);
    else EmitBox(m, p.extent, p.xform, nm);
    outCount = m.IndexCount() - outFirst;
    outVertCount = m.VertexCount() - outFirstVert;
}

// Materials present, ordered by the MaterialKind enum. The enum is append-only, so a material
// introduced later can only ever APPEND a submesh - never renumber one, which would silently
// repoint every "uaf:<path>#<index>" reference at different geometry.
std::vector<MaterialKind> MaterialsOf(const std::vector<const PlacedPiece*>& pieces) {
    std::vector<MaterialKind> mats;
    for (const PlacedPiece* p : pieces)
        if (std::find(mats.begin(), mats.end(), p->material) == mats.end())
            mats.push_back(p->material);
    std::sort(mats.begin(), mats.end());
    return mats;
}

void EmitBucket(const std::vector<const PlacedPiece*>& pieces, Model& model,
                std::vector<MeshRange>& ranges, std::vector<ElementRange>& elements,
                std::vector<MaterialKind>* outMaterials = nullptr) {
    const std::vector<MaterialKind> mats = MaterialsOf(pieces);
    model.resize(mats.size());
    for (usize i = 0; i < mats.size(); ++i) {
        ApplyMaterial(model[i], mats[i]);
        // The weathering of the FIRST piece using this material wins the submesh.
        //
        // A REAL LIMIT, stated rather than hidden: two components sharing a material but aged
        // differently will share one colour, because this renderer has no per-instance or
        // per-vertex colour channel at all (design doc SS2.2). Splitting by weathering as well as
        // material would fix it and multiply the draw count, which is the resource this system is
        // already tightest on.
        for (const PlacedPiece* p : pieces)
            if (p->material == mats[i]) {
                model[i].material.baseColor = WeatherColor(model[i].material.baseColor,
                                                           p->weathering);
                break;
            }
    }

    // One MeshRange per (component, submesh) present, accumulated as pieces arrive so a component
    // whose pieces are interleaved with another's still reports one contiguous-per-submesh slice.
    for (const PlacedPiece* p : pieces) {
        const auto at = std::find(mats.begin(), mats.end(), p->material);
        const u32 sub = static_cast<u32>(std::distance(mats.begin(), at));
        MeshData& m = model[sub];

        u32 first = 0, count = 0, firstVert = 0, vertCount = 0;
        EmitPiece(m, *p, first, count, firstVert, vertCount);
        if (count == 0) continue;

        if (p->element != kWholeComponent) {
            ElementRange er;
            er.component = p->component;
            er.element = p->element;
            er.submesh = sub;
            er.firstIndex = first;
            er.indexCount = count;
            elements.push_back(er);
        }

        MeshRange* r = nullptr;
        for (MeshRange& existing : ranges)
            if (existing.id == p->component && existing.submesh == sub) r = &existing;
        if (!r) {
            MeshRange nr;
            nr.id = p->component;
            nr.submesh = sub;
            nr.firstIndex = first;
            nr.firstVertex = firstVert;
            nr.indexCount = count;
            nr.vertexCount = vertCount;
            ranges.push_back(nr);
        } else {
            r->indexCount = (first + count) - r->firstIndex;
            r->vertexCount = (firstVert + vertCount) - r->firstVertex;
        }
    }

    // Drop empty submeshes, REMAPPING the ranges. A zero-extent component emits no faces, so a
    // material used only by such pieces yields an empty submesh - and CreateMesh returns an
    // INVALID handle for empty MeshData on every backend. Erasing without remapping would point
    // every later range at a different material's geometry.
    std::vector<u32> remap(model.size(), 0);
    u32 next = 0;
    for (usize i = 0; i < model.size(); ++i) {
        remap[i] = next;
        if (!model[i].Empty()) ++next;
    }
    if (next != model.size()) {
        for (MeshRange& r : ranges) r.submesh = remap[r.submesh];
        for (ElementRange& e : elements) e.submesh = remap[e.submesh];
        // The material list must be compacted with the SAME predicate, or it stops being parallel
        // to `model` and every submesh gets the wrong procedural surface.
        std::vector<MaterialKind> keptMats;
        for (usize i = 0; i < model.size(); ++i)
            if (!model[i].Empty()) keptMats.push_back(mats[i]);
        model.erase(std::remove_if(model.begin(), model.end(),
                                   [](const MeshData& m) { return m.Empty(); }),
                    model.end());
        if (outMaterials) *outMaterials = keptMats;
    } else if (outMaterials) {
        *outMaterials = mats;
    }
}

void AccumulateBounds(const Model& model, glm::vec3& mn, glm::vec3& mx, bool& any) {
    for (const MeshData& m : model)
        for (const Vertex& v : m.vertices) {
            if (!any) {
                mn = mx = v.position;
                any = true;
            } else {
                mn = glm::min(mn, v.position);
                mx = glm::max(mx, v.position);
            }
        }
}

} // namespace

void BuildSection(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                  SectionMesh& out) {
    out.model.clear();
    out.ranges.clear();
    out.elements.clear();
    out.boundsMin = glm::vec3(0.0f);
    out.boundsMax = glm::vec3(0.0f);

    std::vector<PlacedPiece> pieces;
    CollectPieces(def, damage, root, pieces);
    if (pieces.empty()) return;

    std::vector<const PlacedPiece*> all;
    all.reserve(pieces.size());
    for (const PlacedPiece& p : pieces) all.push_back(&p);

    EmitBucket(all, out.model, out.ranges, out.elements);

    bool any = false;
    AccumulateBounds(out.model, out.boundsMin, out.boundsMax, any);
}

u32 ChunkedSection::TotalPieces() const {
    u32 n = 0;
    for (const ConstructionChunk& c : chunks) n += c.pieceCount;
    return n;
}

u32 ChunkedSection::TotalIndices() const {
    u32 n = 0;
    for (const ConstructionChunk& c : chunks)
        for (const MeshData& m : c.model) n += m.IndexCount();
    return n;
}

u32 ChunkedSection::DrawCount() const {
    u32 n = 0;
    for (const ConstructionChunk& c : chunks) n += static_cast<u32>(c.model.size());
    return n;
}

const ConstructionChunk* ChunkedSection::ChunkAt(i32 x, i32 y, i32 z) const {
    for (const ConstructionChunk& c : chunks)
        if (c.cx == x && c.cy == y && c.cz == z) return &c;
    return nullptr;
}

std::vector<const ConstructionChunk*> ChunkedSection::ChunksInBounds(const glm::vec3& mn,
                                                                     const glm::vec3& mx) const {
    std::vector<const ConstructionChunk*> hit;
    for (const ConstructionChunk& c : chunks) {
        if (c.boundsMax.x < mn.x || c.boundsMin.x > mx.x) continue;
        if (c.boundsMax.y < mn.y || c.boundsMin.y > mx.y) continue;
        if (c.boundsMax.z < mn.z || c.boundsMin.z > mx.z) continue;
        hit.push_back(&c);
    }
    return hit;
}

void BuildChunked(const ConstructionDef& def, const DamageState* damage, ComponentId root,
                  f32 chunkSize, ChunkedSection& out) {
    out.chunks.clear();
    out.chunkSize = chunkSize > 0.01f ? chunkSize : 4.0f;
    out.boundsMin = out.boundsMax = glm::vec3(0.0f);

    std::vector<PlacedPiece> pieces;
    CollectPieces(def, damage, root, pieces);
    if (pieces.empty()) return;

    // Bucket by the piece's CENTRE. A piece is never split across chunks: a brick straddling a
    // boundary lands wholly in one of them, so every logical unit stays addressable exactly once.
    // Splitting at borders would leave units half-present in two places, and no destruction query
    // could then answer "is this brick still there" with a single answer.
    struct Key {
        i32 x, y, z;
        bool operator<(const Key& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::map<Key, std::vector<const PlacedPiece*>> buckets;
    const f32 inv = 1.0f / out.chunkSize;
    for (const PlacedPiece& p : pieces) {
        const glm::vec3 c = p.Center();
        const Key k{static_cast<i32>(std::floor(c.x * inv)), static_cast<i32>(std::floor(c.y * inv)),
                    static_cast<i32>(std::floor(c.z * inv))};
        buckets[k].push_back(&p);
    }

    bool anyBounds = false;
    for (const auto& [k, list] : buckets) { // std::map iterates sorted -> deterministic order
        ConstructionChunk chunk;
        chunk.cx = k.x;
        chunk.cy = k.y;
        chunk.cz = k.z;
        chunk.pieceCount = static_cast<u32>(list.size());
        EmitBucket(list, chunk.model, chunk.ranges, chunk.elements, &chunk.materials);
        if (chunk.model.empty()) continue;

        bool any = false;
        AccumulateBounds(chunk.model, chunk.boundsMin, chunk.boundsMax, any);
        if (!any) continue;

        if (!anyBounds) {
            out.boundsMin = chunk.boundsMin;
            out.boundsMax = chunk.boundsMax;
            anyBounds = true;
        } else {
            out.boundsMin = glm::min(out.boundsMin, chunk.boundsMin);
            out.boundsMax = glm::max(out.boundsMax, chunk.boundsMax);
        }
        out.chunks.push_back(std::move(chunk));
    }
}

} // namespace hbe::construction
